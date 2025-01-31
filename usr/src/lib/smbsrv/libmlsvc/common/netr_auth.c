/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2021 Tintri by DDN, Inc. All rights reserved.
 */

/*
 * NETR challenge/response client functions.
 *
 * NT_STATUS_INVALID_PARAMETER
 * NT_STATUS_NO_TRUST_SAM_ACCOUNT
 * NT_STATUS_ACCESS_DENIED
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <security/cryptoki.h>
#include <security/pkcs11.h>

#include <smbsrv/libsmb.h>
#include <smbsrv/libsmbns.h>
#include <smbsrv/libmlsvc.h>
#include <mlsvc.h>
#include <smbsrv/ndl/netlogon.ndl>
#include <smbsrv/smbinfo.h>
#include <smbsrv/netrauth.h>

#define	NETR_SESSKEY_ZEROBUF_SZ		4
/* The DES algorithm uses a 56-bit encryption key. */
#define	NETR_DESKEY_LEN			7

int netr_setup_authenticator(netr_info_t *, struct netr_authenticator *,
    struct netr_authenticator *);
DWORD netr_validate_chain(netr_info_t *, struct netr_authenticator *);

static int netr_server_req_challenge(mlsvc_handle_t *, netr_info_t *);
static int netr_server_authenticate2(mlsvc_handle_t *, netr_info_t *);
static int netr_gen_password(BYTE *, BYTE *, BYTE *);

/*
 * Shared with netr_logon.c
 */
netr_info_t netr_global_info = {
	.use_secure_rpc = B_TRUE,
	.use_logon_ex = B_TRUE
};
extern ndr_auth_ctx_t netr_ssp_ctx;

/*
 * These flags control various parts of NetLogon RPC messages.
 * The default is 0 - setting a bit disables some feature.
 * They are set in smbd/netlogon_flags in svc:/network/smb/server.
 * These are set when smbd starts. Changing them requires
 * restarting smbd.
 *
 * These shouldn't be confused with either SamLogonEx's ExtraFlags,
 * or NetrServerAuthenticate's negotiate_flags.
 *
 * DISABLE_SECURE_RPC causes Netlogon to use unauthenticated RPC.
 * Note that the underlying transport is still authenticated and signed.
 *
 * DISABLE_RESP_VERIF instructs RPC authentication to ignore failures
 * when verifying responses.
 *
 * DISABLE_SAMLOGONEX causes Netlogon to always use SamLogon, which
 * makes use of Netlogon Authenticators.
 */
#define	NETR_CFG_DISABLE_SECURE_RPC	0x00000001
#define	NETR_CFG_DISABLE_RESP_VERIF	0x00000002
#define	NETR_CFG_DISABLE_SAMLOGONEX	0x00000004

void
netlogon_init_global(uint32_t flags)
{
	netr_global_info.use_secure_rpc =
	    ((flags & NETR_CFG_DISABLE_SECURE_RPC) == 0);
	netr_ssp_ctx.auth_verify_resp =
	    ((flags & NETR_CFG_DISABLE_RESP_VERIF) == 0);
	netr_global_info.use_logon_ex =
	    ((flags & NETR_CFG_DISABLE_SAMLOGONEX) == 0);
}

/*
 * AES-CFB8 has the odd property that 1/256 keys will encrypt
 * a full block of 0s to all 0s. In order to mitigate this, Windows DCs
 * now reject Challenges and Credentials where "none of the first 5 bytes
 * are unique" (i.e. [MS-NRPC] 3.1.4.1 "Session-Key Negotiation" Step 7).
 * This detects that condition so that we can avoid having our connection
 * rejected unexpectedly.
 *
 * I've interpreted this condition as 'amongst the first 5 bytes,
 * at least one must appear exactly once'.
 *
 * NOTE: Win2012r2 seems to only reject challenges whose first 5 bytes are 0.
 */
boolean_t
passes_dc_mitigation(uint8_t *buf)
{
	int i, j;

	for (i = 0; i < 5; i++) {
		for (j = 0; j < 5; j++) {
			if (i != j && buf[i] == buf[j])
				break;
		}

		/* if this byte didn't match any other byte, this passes */
		if (j == 5)
			return (B_TRUE);
	}

	/* None of the bytes were unique - the check fails */
	return (B_FALSE);
}

/*
 * netlogon_auth
 *
 * This is the core of the NETLOGON authentication protocol.
 * Do the challenge response authentication.
 *
 * Prior to calling this function, an anonymous session to the NETLOGON
 * pipe on a domain controller(server) should have already been opened.
 *
 * Upon a successful NETLOGON credential chain establishment, the
 * netlogon sequence number will be set to match the kpasswd sequence
 * number.
 *
 */
DWORD
netlogon_auth(char *server, char *domain, DWORD flags)
{
	mlsvc_handle_t netr_handle;
	netr_info_t *netr_info;
	int rc;
	DWORD leout_rc[2];
	boolean_t retry;
	DWORD status;

	/*
	 * [MS-NRPC] 3.1.4.1 "Session-Key Negotiation"
	 * Negotiation happens on an 'unprotected RPC channel'
	 * (no RPC-level auth).
	 */
	status = netr_open(server, domain, &netr_handle);

	if (status != 0) {
		syslog(LOG_ERR, "netlogon_auth remote open failed (%s)",
		    xlate_nt_status(status));
		return (status);
	}

	netr_info = &netr_global_info;
	bzero(&netr_info->session_key, sizeof (netr_info->session_key));
	netr_info->flags = flags;

	rc = smb_getnetbiosname(netr_info->hostname, NETBIOS_NAME_SZ);
	if (rc != 0)
		goto errout;

	/* server is our DC.  Note: normally an FQDN. */
	(void) snprintf(netr_info->server, sizeof (netr_info->server),
	    "\\\\%s", server);

	/*
	 * Domain (FQDN and NetBIOS) Name needed for Netlogon SSP-based
	 * Secure RPC.
	 */
	rc = smb_getdomainname(netr_info->nb_domain,
	    sizeof (netr_info->nb_domain));
	if (rc != 0)
		goto errout;

	rc = smb_getfqdomainname(netr_info->fqdn_domain,
	    sizeof (netr_info->fqdn_domain));
	if (rc != 0)
		goto errout;

	/*
	 * [MS-NRPC] 3.1.4.1 "Session-Key Negotiation" Step 7
	 * Windows DCs will reject negotiate attempts if none of the first
	 * 5 bytes of the Challenge are unique.
	 * Keep retrying until we've generated one that satisfies this.
	 */
	do {
		retry = B_FALSE;
		LE_OUT32(&leout_rc[0], arc4random());
		LE_OUT32(&leout_rc[1], arc4random());
		(void) memcpy(&netr_info->client_challenge, leout_rc,
		    sizeof (struct netr_credential));

		if (!passes_dc_mitigation(netr_info->client_challenge.data))
			retry = B_TRUE;
	} while (retry);

	if ((rc = netr_server_req_challenge(&netr_handle, netr_info)) == 0) {
		rc = netr_server_authenticate2(&netr_handle, netr_info);
		if (rc == 0) {
			/*
			 * TODO: (later)  When joining a domain using a
			 * pre-created machine account, should do:
			 * netr_server_password_set(&netr_handle, netr_info);
			 * Nexenta issue 11960
			 */
			smb_update_netlogon_seqnum();
			netr_info->flags |= NETR_FLG_VALID;

		}
	}

errout:
	(void) netr_close(&netr_handle);

	return ((rc) ? NT_STATUS_UNSUCCESSFUL : NT_STATUS_SUCCESS);
}

/*
 * netr_open
 *
 * Open an anonymous session to the NETLOGON pipe on a domain controller
 * and bind to the NETR RPC interface.
 *
 * We store the remote server information, which is used to drive Windows
 * version specific behavior.
 *
 * Returns 0 or NT status
 */
DWORD
netr_open(char *server, char *domain, mlsvc_handle_t *netr_handle)
{
	char user[SMB_USERNAME_MAXLEN];
	DWORD status;

	smb_ipc_get_user(user, SMB_USERNAME_MAXLEN);

	status = ndr_rpc_bind(netr_handle, server, domain, user, "NETR");

	return (status);
}

uint32_t auth_context_id = 1;

DWORD
netr_open_secure(char *server, char *domain, mlsvc_handle_t *netr_handle)
{
	char user[SMB_USERNAME_MAXLEN];
	DWORD status;

	smb_ipc_get_user(user, SMB_USERNAME_MAXLEN);

	/*
	 * If the server doesn't support SECURE_RPC_FLAG, or we've disabled
	 * secure rpc (netr_global_info.use_secure_rpc), then SECURE_RPC_FLAG
	 * won't be in the set of negotiated flags. Don't use SecureRPC if
	 * that's the case.
	 */
	if ((netr_global_info.nego_flags & NETR_NEGO_SECURE_RPC_FLAG) != 0) {
		netr_ssp_ctx.auth_context_id = auth_context_id++;
		status = ndr_rpc_bind_secure(netr_handle, server, domain, user,
		    "NETR", &netr_ssp_ctx);
	} else {
		status = ndr_rpc_bind(netr_handle, server, domain, user,
		    "NETR");
	}

	return (status);
}

/*
 * netr_close
 *
 * Close a NETLOGON pipe and free the RPC context.
 */
int
netr_close(mlsvc_handle_t *netr_handle)
{
	ndr_rpc_unbind(netr_handle);
	return (0);
}

/*
 * netr_server_req_challenge
 */
static int
netr_server_req_challenge(mlsvc_handle_t *netr_handle, netr_info_t *netr_info)
{
	struct netr_ServerReqChallenge arg;
	int opnum;

	bzero(&arg, sizeof (struct netr_ServerReqChallenge));
	opnum = NETR_OPNUM_ServerReqChallenge;

	arg.servername = (unsigned char *)netr_info->server;
	arg.hostname = (unsigned char *)netr_info->hostname;

	(void) memcpy(&arg.client_challenge, &netr_info->client_challenge,
	    sizeof (struct netr_credential));

	if (ndr_rpc_call(netr_handle, opnum, &arg) != 0)
		return (-1);

	if (arg.status != 0) {
		ndr_rpc_status(netr_handle, opnum, arg.status);
		ndr_rpc_release(netr_handle);
		return (-1);
	}

	(void) memcpy(&netr_info->server_challenge, &arg.server_challenge,
	    sizeof (struct netr_credential));

	ndr_rpc_release(netr_handle);
	return (0);
}

uint32_t netr_server_auth2_flags =
    NETR_NEGO_BASE_FLAGS |
    NETR_NEGO_STRONGKEY_FLAG |
    NETR_NEGO_SECURE_RPC_FLAG;

/*
 * netr_server_authenticate2
 */
static int
netr_server_authenticate2(mlsvc_handle_t *netr_handle, netr_info_t *netr_info)
{
	struct netr_ServerAuthenticate2 arg;
	/* sizeof netr_info->hostname, + 1 for the '$' */
	char account_name[(NETBIOS_NAME_SZ * 2) + 1];
	int opnum;
	int rc;

	bzero(&arg, sizeof (struct netr_ServerAuthenticate2));
	opnum = NETR_OPNUM_ServerAuthenticate2;

	(void) snprintf(account_name, sizeof (account_name), "%s$",
	    netr_info->hostname);

	smb_tracef("server=[%s] account_name=[%s] hostname=[%s]\n",
	    netr_info->server, account_name, netr_info->hostname);

	arg.servername = (unsigned char *)netr_info->server;
	arg.account_name = (unsigned char *)account_name;
	arg.account_type = NETR_WKSTA_TRUST_ACCOUNT_TYPE;
	arg.hostname = (unsigned char *)netr_info->hostname;
	arg.negotiate_flags = netr_server_auth2_flags;

	/*
	 * If we've disabled SecureRPC, remove it from our negotiate_flags
	 * so that the returned flags don't include it. We won't later use
	 * SecureRPC if the returned flags don't include the flag.
	 */
	if (!netr_global_info.use_secure_rpc)
		arg.negotiate_flags &= ~NETR_NEGO_SECURE_RPC_FLAG;

	if (arg.negotiate_flags & NETR_NEGO_STRONGKEY_FLAG) {
		if (netr_gen_skey128(netr_info) != SMBAUTH_SUCCESS)
			return (-1);
	} else {
		if (netr_gen_skey64(netr_info) != SMBAUTH_SUCCESS)
			return (-1);
	}

	/*
	 * We can't 'fiddle' with anything here to prevent getting bitten by
	 * ClientStoredCredential-based mitigations.
	 *
	 * If we're using SamLogonEx, we won't use authenticators unless
	 * some other NetLogon command is implemented and used.
	 */
	if (netr_gen_credentials(netr_info->session_key.key,
	    &netr_info->client_challenge, 0,
	    &netr_info->client_credential, B_FALSE) != SMBAUTH_SUCCESS) {
		return (-1);
	}

	if (netr_gen_credentials(netr_info->session_key.key,
	    &netr_info->server_challenge, 0,
	    &netr_info->server_credential, B_FALSE) != SMBAUTH_SUCCESS) {
		return (-1);
	}

	(void) memcpy(&arg.client_credential, &netr_info->client_credential,
	    sizeof (struct netr_credential));

	if (ndr_rpc_call(netr_handle, opnum, &arg) != 0)
		return (-1);

	if (arg.status != 0) {
		ndr_rpc_status(netr_handle, opnum, arg.status);
		ndr_rpc_release(netr_handle);
		return (-1);
	}

	/* The server returns the intersection of our flags and their flags. */
	netr_info->nego_flags = arg.negotiate_flags;

	rc = memcmp(&netr_info->server_credential, &arg.server_credential,
	    sizeof (struct netr_credential));

	ndr_rpc_release(netr_handle);
	return (rc);
}

/*
 * netr_gen_skey128
 *
 * Generate a 128-bit session key from the client and server challenges.
 * See "Session-Key Computation" section of MS-NRPC document.
 */
int
netr_gen_skey128(netr_info_t *netr_info)
{
	unsigned char ntlmhash[SMBAUTH_HASH_SZ];
	int rc = SMBAUTH_FAILURE;
	CK_RV rv;
	CK_MECHANISM mechanism;
	CK_SESSION_HANDLE hSession;
	CK_ULONG diglen = MD_DIGEST_LEN;
	unsigned char md5digest[MD_DIGEST_LEN];
	unsigned char zerobuf[NETR_SESSKEY_ZEROBUF_SZ];

	bzero(ntlmhash, SMBAUTH_HASH_SZ);
	/*
	 * We should check (netr_info->flags & NETR_FLG_INIT) and use
	 * the appropriate password but it isn't working yet.  So we
	 * always use the default one for now.
	 */
	bzero(netr_info->password, sizeof (netr_info->password));
	rc = smb_config_getstr(SMB_CI_MACHINE_PASSWD,
	    (char *)netr_info->password, sizeof (netr_info->password));

	if ((rc != SMBD_SMF_OK) || *netr_info->password == '\0') {
		return (SMBAUTH_FAILURE);
	}

	rc = smb_auth_ntlm_hash((char *)netr_info->password, ntlmhash);
	if (rc != SMBAUTH_SUCCESS) {
		explicit_bzero(&netr_info->password,
		    sizeof (netr_info->password));
		return (SMBAUTH_FAILURE);
	}

	bzero(zerobuf, NETR_SESSKEY_ZEROBUF_SZ);

	mechanism.mechanism = CKM_MD5;
	mechanism.pParameter = 0;
	mechanism.ulParameterLen = 0;

	rv = SUNW_C_GetMechSession(mechanism.mechanism, &hSession);
	if (rv != CKR_OK) {
		rc = SMBAUTH_FAILURE;
		goto errout;
	}

	rv = C_DigestInit(hSession, &mechanism);
	if (rv != CKR_OK)
		goto cleanup;

	rv = C_DigestUpdate(hSession, (CK_BYTE_PTR)zerobuf,
	    NETR_SESSKEY_ZEROBUF_SZ);
	if (rv != CKR_OK)
		goto cleanup;

	rv = C_DigestUpdate(hSession,
	    (CK_BYTE_PTR)netr_info->client_challenge.data, NETR_CRED_DATA_SZ);
	if (rv != CKR_OK)
		goto cleanup;

	rv = C_DigestUpdate(hSession,
	    (CK_BYTE_PTR)netr_info->server_challenge.data, NETR_CRED_DATA_SZ);
	if (rv != CKR_OK)
		goto cleanup;

	rv = C_DigestFinal(hSession, (CK_BYTE_PTR)md5digest, &diglen);
	if (rv != CKR_OK)
		goto cleanup;

	rc = smb_auth_hmac_md5(md5digest, diglen, ntlmhash, SMBAUTH_HASH_SZ,
	    netr_info->session_key.key);

	netr_info->session_key.len = NETR_SESSKEY128_SZ;
cleanup:
	(void) C_CloseSession(hSession);

errout:
	explicit_bzero(&netr_info->password, sizeof (netr_info->password));
	explicit_bzero(ntlmhash, sizeof (ntlmhash));

	return (rc);

}
/*
 * netr_gen_skey64
 *
 * Generate a 64-bit session key from the client and server challenges.
 * See "Session-Key Computation" section of MS-NRPC document.
 *
 * The algorithm is a two stage hash. For the first hash, the input is
 * the combination of the client and server challenges, the key is
 * the first 7 bytes of the password. The initial password is formed
 * using the NT password hash on the local hostname in lower case.
 * The result is stored in a temporary buffer.
 *
 *		input:	challenge
 *		key:	passwd lower 7 bytes
 *		output:	intermediate result
 *
 * For the second hash, the input is the result of the first hash and
 * the key is the last 7 bytes of the password.
 *
 *		input:	result of first hash
 *		key:	passwd upper 7 bytes
 *		output:	session_key
 *
 * The final output should be the session key.
 *
 *		FYI: smb_auth_DES(output, key, input)
 *
 * If any difficulties occur using the cryptographic framework, the
 * function returns SMBAUTH_FAILURE.  Otherwise SMBAUTH_SUCCESS is
 * returned.
 */
int
netr_gen_skey64(netr_info_t *netr_info)
{
	unsigned char md4hash[32];
	unsigned char buffer[8];
	DWORD data[2];
	DWORD *client_challenge;
	DWORD *server_challenge;
	int rc;
	DWORD le_data[2];

	client_challenge = (DWORD *)(uintptr_t)&netr_info->client_challenge;
	server_challenge = (DWORD *)(uintptr_t)&netr_info->server_challenge;
	bzero(md4hash, 32);

	/*
	 * We should check (netr_info->flags & NETR_FLG_INIT) and use
	 * the appropriate password but it isn't working yet.  So we
	 * always use the default one for now.
	 */
	bzero(netr_info->password, sizeof (netr_info->password));
	rc = smb_config_getstr(SMB_CI_MACHINE_PASSWD,
	    (char *)netr_info->password, sizeof (netr_info->password));

	if ((rc != SMBD_SMF_OK) || *netr_info->password == '\0') {
		return (SMBAUTH_FAILURE);
	}

	rc = smb_auth_ntlm_hash((char *)netr_info->password, md4hash);

	if (rc != SMBAUTH_SUCCESS) {
		rc = SMBAUTH_FAILURE;
		goto out;
	}

	data[0] = LE_IN32(&client_challenge[0]) + LE_IN32(&server_challenge[0]);
	data[1] = LE_IN32(&client_challenge[1]) + LE_IN32(&server_challenge[1]);
	LE_OUT32(&le_data[0], data[0]);
	LE_OUT32(&le_data[1], data[1]);
	rc = smb_auth_DES(buffer, 8, md4hash, NETR_DESKEY_LEN,
	    (unsigned char *)le_data, 8);

	if (rc != SMBAUTH_SUCCESS)
		goto out;

	netr_info->session_key.len = NETR_SESSKEY64_SZ;
	rc = smb_auth_DES(netr_info->session_key.key,
	    netr_info->session_key.len, &md4hash[9], NETR_DESKEY_LEN, buffer,
	    8);

out:
	explicit_bzero(&netr_info->password, sizeof (netr_info->password));
	explicit_bzero(md4hash, sizeof (md4hash));
	explicit_bzero(buffer, sizeof (buffer));
	return (rc);
}

/*
 * netr_gen_credentials
 *
 * Generate a set of credentials from a challenge and a session key.
 * The algorithm is a two stage hash. For the first hash, the
 * timestamp is added to the challenge and the result is stored in a
 * temporary buffer:
 *
 *		input:	challenge (including timestamp)
 *		key:	session_key
 *		output:	intermediate result
 *
 * For the second hash, the input is the result of the first hash and
 * a strange partial key is used:
 *
 *		input:	result of first hash
 *		key:	funny partial key
 *		output:	credentiails
 *
 * The final output should be an encrypted set of credentials.
 *
 *		FYI: smb_auth_DES(output, key, input)
 *
 * If any difficulties occur using the cryptographic framework, the
 * function returns SMBAUTH_FAILURE.  Otherwise SMBAUTH_SUCCESS is
 * returned.
 */
int
netr_gen_credentials(BYTE *session_key, netr_cred_t *challenge,
    DWORD timestamp, netr_cred_t *out_cred, boolean_t retry)
{
	unsigned char buffer[8];
	DWORD data[2];
	DWORD le_data[2];
	DWORD *p;
	int rc;

	p = (DWORD *)(uintptr_t)challenge;
	data[0] = LE_IN32(&p[0]) + timestamp;
	data[1] = LE_IN32(&p[1]);

	LE_OUT32(&le_data[0], data[0]);
	LE_OUT32(&le_data[1], data[1]);

	if (smb_auth_DES(buffer, 8, session_key, NETR_DESKEY_LEN,
	    (unsigned char *)le_data, 8) != SMBAUTH_SUCCESS)
		return (SMBAUTH_FAILURE);

	rc = smb_auth_DES(out_cred->data, 8, &session_key[NETR_DESKEY_LEN],
	    NETR_DESKEY_LEN, buffer, 8);

	/*
	 * [MS-NRPC] 3.1.4.6 "Calling Methods Requiring Session-Key
	 * Establishment" Step 6
	 *
	 * Windows DCs will reject authenticators if none of the first
	 * 5 bytes of the ClientStoredCredential are unique.
	 * Keep retrying until we've generated one that satisfies this,
	 * but only if the caller can handle retries.
	 */
	if (retry && !passes_dc_mitigation(out_cred->data))
		return (SMBAUTH_RETRY);

	return (rc);
}

/*
 * netr_server_password_set
 *
 * Attempt to change the trust account password for this system.
 *
 * Note that this call may legitimately fail if the registry on the
 * domain controller has been setup to deny attempts to change the
 * trust account password. In this case we should just continue to
 * use the original password.
 *
 * Possible status values:
 *	NT_STATUS_ACCESS_DENIED
 */
int
netr_server_password_set(mlsvc_handle_t *netr_handle, netr_info_t *netr_info)
{
	struct netr_PasswordSet  arg;
	int opnum;
	BYTE new_password[NETR_OWF_PASSWORD_SZ];
	char account_name[NETBIOS_NAME_SZ * 2];

	bzero(&arg, sizeof (struct netr_PasswordSet));
	opnum = NETR_OPNUM_ServerPasswordSet;

	(void) snprintf(account_name, sizeof (account_name), "%s$",
	    netr_info->hostname);

	arg.servername = (unsigned char *)netr_info->server;
	arg.account_name = (unsigned char *)account_name;
	arg.sec_chan_type = NETR_WKSTA_TRUST_ACCOUNT_TYPE;
	arg.hostname = (unsigned char *)netr_info->hostname;

	/*
	 * Set up the client side authenticator.
	 */
	if (netr_setup_authenticator(netr_info, &arg.auth, 0) !=
	    SMBAUTH_SUCCESS) {
		return (-1);
	}

	/*
	 * Generate a new password from the old password.
	 */
	if (netr_gen_password(netr_info->session_key.key,
	    netr_info->password, new_password) == SMBAUTH_FAILURE) {
		return (-1);
	}

	(void) memcpy(&arg.owf_password, &new_password,
	    NETR_OWF_PASSWORD_SZ);

	if (ndr_rpc_call(netr_handle, opnum, &arg) != 0)
		return (-1);

	if (arg.status != 0) {
		ndr_rpc_status(netr_handle, opnum, arg.status);
		ndr_rpc_release(netr_handle);
		return (-1);
	}

	/*
	 * Check the returned credentials.  The server returns the new
	 * client credential rather than the new server credentiali,
	 * as documented elsewhere.
	 *
	 * Generate the new seed for the credential chain.  Increment
	 * the timestamp and add it to the client challenge.  Then we
	 * need to copy the challenge to the credential field in
	 * preparation for the next cycle.
	 */
	if (netr_validate_chain(netr_info, &arg.auth) == 0) {
		/*
		 * Save the new password.
		 */
		(void) memcpy(netr_info->password, new_password,
		    NETR_OWF_PASSWORD_SZ);
	}

	ndr_rpc_release(netr_handle);
	return (0);
}

/*
 * netr_gen_password
 *
 * Generate a new pasword from the old password  and the session key.
 * The algorithm is a two stage hash. The session key is used in the
 * first hash but only part of the session key is used in the second
 * hash.
 *
 * If any difficulties occur using the cryptographic framework, the
 * function returns SMBAUTH_FAILURE.  Otherwise SMBAUTH_SUCCESS is
 * returned.
 */
static int
netr_gen_password(BYTE *session_key, BYTE *old_password, BYTE *new_password)
{
	int rv;

	rv = smb_auth_DES(new_password, 8, session_key, NETR_DESKEY_LEN,
	    old_password, 8);
	if (rv != SMBAUTH_SUCCESS)
		return (rv);

	rv = smb_auth_DES(&new_password[8], 8, &session_key[NETR_DESKEY_LEN],
	    NETR_DESKEY_LEN, &old_password[8], 8);
	return (rv);
}

/*
 * Todo: need netr_server_password_set2()
 * used by "unsecure join". (NX 11960)
 */

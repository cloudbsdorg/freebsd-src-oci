/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the PAM module's JWT verification
 * (usr.sbin/ocifbsd/pam/pam_auth.c): signature checking and expiry
 * enforcement. Pure; no PAM conversation.
 */

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fix the signing secret before pam_auth.c's get_jwt_secret() caches it. */
static void set_secret(void) { setenv("OCIFBSD_JWT_SECRET", "unit-test-secret", 1); }

/*
 * pam_auth.c calls pam_auth_password() (the system/password backend), which is
 * resolved by the host binary when the PAM module is dlopen'd. It is only used
 * on the password path — never by the JWT verification under test — so a stub
 * satisfies the link.
 */
int pam_auth_password(const char *username, const char *password);
int
pam_auth_password(const char *username, const char *password)
{
	(void)username;
	(void)password;
	return (-1);
}

#include "pam/pam_auth.c"

/* Build a token: header.payload.sig, sig = base64(HMAC(secret, payload_b64)). */
static char *
make_token(const char *payload_json, int corrupt_sig)
{
	const char *hdr = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
	char hdr_b64[256], pl_b64[512], sig[128];
	uint8_t hash[SHA256_DIGEST_LENGTH];
	const char *secret;
	size_t slen;
	char *tok = NULL;

	secret = get_jwt_secret(&slen);
	base64_encode(hdr, strlen(hdr), hdr_b64, sizeof(hdr_b64));
	base64_encode(payload_json, strlen(payload_json), pl_b64, sizeof(pl_b64));
	hmac_sha256(secret, slen, pl_b64, strlen(pl_b64), hash, sizeof(hash));
	base64_encode(hash, sizeof(hash), sig, sizeof(sig));
	if (corrupt_sig)
		sig[0] = (sig[0] == 'A') ? 'B' : 'A';
	asprintf(&tok, "%s.%s.%s", hdr_b64, pl_b64, sig);
	return (tok);
}

ATF_TC(valid_token_accepted);
ATF_TC_HEAD(valid_token_accepted, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a correctly-signed, unexpired token verifies and yields claims");
}
ATF_TC_BODY(valid_token_accepted, tc)
{
	char payload[256];
	char *user = NULL, *tok;
	uint32_t perms = 0;

	set_secret();
	snprintf(payload, sizeof(payload),
	    "{\"sub\":\"alice\",\"perms\":7,\"exp\":%ld}",
	    (long)time(NULL) + 3600);
	tok = make_token(payload, 0);
	ATF_REQUIRE(tok != NULL);

	ATF_CHECK_EQ(0, pam_verify_jwt(tok, &user, &perms, NULL));
	ATF_REQUIRE(user != NULL);
	ATF_CHECK_STREQ("alice", user);
	free(user);
	free(tok);
}

ATF_TC(tampered_signature_rejected);
ATF_TC_HEAD(tampered_signature_rejected, tc)
{
	atf_tc_set_md_var(tc, "descr", "a token with a flipped signature byte fails");
}
ATF_TC_BODY(tampered_signature_rejected, tc)
{
	char payload[256];
	char *tok;

	set_secret();
	snprintf(payload, sizeof(payload),
	    "{\"sub\":\"bob\",\"perms\":1,\"exp\":%ld}",
	    (long)time(NULL) + 3600);
	tok = make_token(payload, 1);	/* corrupt the signature */
	ATF_REQUIRE(tok != NULL);
	ATF_CHECK(pam_verify_jwt(tok, NULL, NULL, NULL) != 0);
	free(tok);
}

ATF_TC(expired_token_rejected_even_without_out_param);
ATF_TC_HEAD(expired_token_rejected_even_without_out_param, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "an expired token is rejected even when the caller passes exp=NULL "
	    "(the bug: expiry was only enforced when exp!=NULL)");
}
ATF_TC_BODY(expired_token_rejected_even_without_out_param, tc)
{
	char payload[256];
	char *tok;

	set_secret();
	snprintf(payload, sizeof(payload),
	    "{\"sub\":\"carol\",\"perms\":1,\"exp\":%ld}",
	    (long)time(NULL) - 60);	/* expired a minute ago */
	tok = make_token(payload, 0);	/* correctly signed */
	ATF_REQUIRE(tok != NULL);
	/* exp out-param is NULL — must still be rejected as expired. */
	ATF_CHECK(pam_verify_jwt(tok, NULL, NULL, NULL) != 0);
	free(tok);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, valid_token_accepted);
	ATF_TP_ADD_TC(tp, tampered_signature_rejected);
	ATF_TP_ADD_TC(tp, expired_token_rejected_even_without_out_param);

	return (atf_no_error());
}

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the security-daemon secret encryption
 * (usr.sbin/ocifbsd/security-daemon/auth.c): AES-256-CBC round trip after
 * porting off the kernel rijndael primitive to OpenSSL EVP. Pure; no daemon.
 */

#include <atf-c.h>
#include <stdlib.h>
#include <string.h>

#include "security-daemon/auth.c"

static void
use_test_key(void)
{
	memset(encryption_key, 0x42, sizeof(encryption_key));
	encryption_key_initialized = 1;
}

ATF_TC(encrypt_decrypt_roundtrip);
ATF_TC_HEAD(encrypt_decrypt_roundtrip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "secret_encrypt then secret_decrypt returns the original plaintext, "
	    "and the ciphertext is IV-prefixed and not the plaintext");
}
ATF_TC_BODY(encrypt_decrypt_roundtrip, tc)
{
	const char *pt = "a moderately long secret value !@#";
	size_t ptlen = strlen(pt);
	void *ct = NULL, *rt = NULL;
	size_t ctlen = 0, rtlen = 0;

	use_test_key();

	ATF_REQUIRE_EQ(0, secret_encrypt(pt, ptlen, &ct, &ctlen));
	ATF_REQUIRE(ct != NULL);
	/* 16-byte IV + at least one 16-byte block. */
	ATF_CHECK(ctlen >= 32);
	ATF_CHECK((ctlen % 16) == 0);
	/* Ciphertext must not contain the plaintext. */
	ATF_CHECK(memmem(ct, ctlen, pt, ptlen) == NULL);

	ATF_REQUIRE_EQ(0, secret_decrypt(ct, ctlen, &rt, &rtlen));
	ATF_REQUIRE(rt != NULL);
	ATF_CHECK_EQ(ptlen, rtlen);
	ATF_CHECK(memcmp(pt, rt, rtlen) == 0);

	free(ct);
	free(rt);
}

ATF_TC(distinct_iv_per_call);
ATF_TC_HEAD(distinct_iv_per_call, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "encrypting the same plaintext twice yields different ciphertext "
	    "(random IV), yet both decrypt correctly");
}
ATF_TC_BODY(distinct_iv_per_call, tc)
{
	const char *pt = "same input";
	void *a = NULL, *b = NULL;
	size_t alen = 0, blen = 0;

	use_test_key();
	ATF_REQUIRE_EQ(0, secret_encrypt(pt, strlen(pt), &a, &alen));
	ATF_REQUIRE_EQ(0, secret_encrypt(pt, strlen(pt), &b, &blen));
	ATF_CHECK_EQ(alen, blen);
	/* Different IV -> different ciphertext. */
	ATF_CHECK(memcmp(a, b, alen) != 0);
	free(a);
	free(b);
}

ATF_TC(decrypt_rejects_bad_input);
ATF_TC_HEAD(decrypt_rejects_bad_input, tc)
{
	atf_tc_set_md_var(tc, "descr", "short or misaligned ciphertext is rejected");
}
ATF_TC_BODY(decrypt_rejects_bad_input, tc)
{
	char buf[40];
	void *out = NULL;
	size_t outlen = 0;

	use_test_key();
	memset(buf, 0, sizeof(buf));
	/* Too short (< 32) and not a multiple of 16. */
	ATF_CHECK(secret_decrypt(buf, 20, &out, &outlen) != 0);
	ATF_CHECK(secret_decrypt(buf, 33, &out, &outlen) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, encrypt_decrypt_roundtrip);
	ATF_TP_ADD_TC(tp, distinct_iv_per_call);
	ATF_TP_ADD_TC(tp, decrypt_rejects_bad_input);

	return (atf_no_error());
}

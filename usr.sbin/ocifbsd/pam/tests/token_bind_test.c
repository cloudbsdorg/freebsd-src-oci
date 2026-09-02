/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Regression test: pam_verify_token() must bind a JWT to the identity the
 * caller is authenticating. A validly-signed, unexpired token issued to one
 * user must NOT authenticate a request that claims to be a different user.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pam_auth.h"

static int failures;

static void
check(const char *name, int cond)
{
	printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
	if (!cond)
		failures++;
}

int
main(void)
{
	char *token = NULL;
	uint32_t perms = 0;

	/* Fixed secret so generate and verify agree. */
	setenv("OCIFBSD_JWT_SECRET", "test-secret-for-token-binding", 1);

	if (pam_generate_token("guest", "viewer", 0x1, 3600, &token) != 0 ||
	    token == NULL) {
		printf("FAIL: could not generate token\n");
		return (1);
	}

	/* Positive: token verifies for the user it was issued to. */
	check("token accepted for its own subject (guest)",
	    pam_verify_token(token, "guest", &perms) == 0);

	/* Negative: same valid token must be rejected for a different user. */
	check("token REJECTED when claiming a different user (admin)",
	    pam_verify_token(token, "admin", &perms) != 0);

	/* Negative: NULL expected user is rejected. */
	check("token rejected with NULL expected user",
	    pam_verify_token(token, NULL, &perms) != 0);

	free(token);
	printf("%s (%d failure%s)\n", failures ? "TESTS FAILED" : "ALL PASSED",
	    failures, failures == 1 ? "" : "s");
	return (failures ? 1 : 0);
}

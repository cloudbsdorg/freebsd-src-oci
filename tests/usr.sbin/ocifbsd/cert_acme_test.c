/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the ACME revokeCert request-body builder
 * (usr.sbin/ocifbsd/cert/acme.c). Pure; no network.
 */

#include <atf-c.h>
#include <stdlib.h>
#include <string.h>

#include "cert/acme.c"

/* A throwaway self-signed EC certificate (P-256), for parsing only. */
static const char *TEST_CERT_PEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIBgzCCASmgAwIBAgIUQ5r60VLFC51edomZijcrJFA2ymIwCgYIKoZIzj0EAwIw\n"
"FzEVMBMGA1UEAwwMdGVzdC5leGFtcGxlMB4XDTI2MDkwMjA3MTc1MFoXDTI2MDkw\n"
"MzA3MTc1MFowFzEVMBMGA1UEAwwMdGVzdC5leGFtcGxlMFkwEwYHKoZIzj0CAQYI\n"
"KoZIzj0DAQcDQgAEYIR0TSAB1uLEi/AJBVyJyNPfIXJF4EVHwM6x1TI2FwNsKPuQ\n"
"Wr3jr4m4FsGWl/FQGWVIR2SWrOwWpe8kk/aI+aNTMFEwHQYDVR0OBBYEFKdUamCk\n"
"pP22I6MS5HPjnA9zvlsQMB8GA1UdIwQYMBaAFKdUamCkpP22I6MS5HPjnA9zvlsQ\n"
"MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhAK7dtspHNHnNUqZE\n"
"sU1fci1zOhDWrEgxC6fnMyniGQweAiBPx2dpQqEOAgRfoV10zwjnyRJNFWdZtFQn\n"
"rnShiJRyGw==\n"
"-----END CERTIFICATE-----\n";

ATF_TC(revoke_payload_valid);
ATF_TC_HEAD(revoke_payload_valid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a PEM certificate becomes {\"certificate\":\"<base64url-DER>\"}");
}
ATF_TC_BODY(revoke_payload_valid, tc)
{
	char *p = acme_revoke_payload(TEST_CERT_PEM);

	ATF_REQUIRE(p != NULL);
	/* Correct JSON shape, base64url (no '+' '/' '=' padding). */
	ATF_CHECK(strncmp(p, "{\"certificate\":\"", 16) == 0);
	ATF_CHECK(strstr(p, "\"}") != NULL);
	ATF_CHECK(strchr(p, '+') == NULL);
	ATF_CHECK(strchr(p, '/') == NULL);
	ATF_CHECK(strchr(p, '=') == NULL);
	free(p);
}

ATF_TC(revoke_payload_rejects_bad);
ATF_TC_HEAD(revoke_payload_rejects_bad, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "NULL or non-PEM input yields NULL, not a bogus body");
}
ATF_TC_BODY(revoke_payload_rejects_bad, tc)
{
	ATF_CHECK(acme_revoke_payload(NULL) == NULL);
	ATF_CHECK(acme_revoke_payload("") == NULL);
	ATF_CHECK(acme_revoke_payload("not a certificate") == NULL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, revoke_payload_valid);
	ATF_TP_ADD_TC(tp, revoke_payload_rejects_bad);

	return (atf_no_error());
}

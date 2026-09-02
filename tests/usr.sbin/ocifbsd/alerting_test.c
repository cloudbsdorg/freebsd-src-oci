/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for logd alert-rule activation / silence expiry
 * (usr.sbin/ocifbsd/logd/alerting.c). Pure; no daemon or threads.
 */

#include <atf-c.h>
#include <string.h>
#include <time.h>

#include "logd/logd.h"
#include "logd/alerting.c"

static struct alert_rule
mkrule(void)
{
	struct alert_rule r;

	memset(&r, 0, sizeof(r));
	r.enabled = true;
	r.silenced = false;
	r.silenced_until = 0;
	return (r);
}

ATF_TC(active_when_enabled);
ATF_TC_HEAD(active_when_enabled, tc)
{
	atf_tc_set_md_var(tc, "descr", "an enabled, unsilenced rule is active");
}
ATF_TC_BODY(active_when_enabled, tc)
{
	struct alert_rule r = mkrule();

	ATF_CHECK(alert_rule_active(&r, time(NULL)));
	ATF_CHECK(alert_rule_active(NULL, time(NULL)) == false);

	r.enabled = false;
	ATF_CHECK(alert_rule_active(&r, time(NULL)) == false);
}

ATF_TC(manual_silence_stays);
ATF_TC_HEAD(manual_silence_stays, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a manual silence (silenced_until == 0) never auto-expires");
}
ATF_TC_BODY(manual_silence_stays, tc)
{
	struct alert_rule r = mkrule();

	r.silenced = true;
	r.silenced_until = 0;
	ATF_CHECK(alert_rule_active(&r, time(NULL)) == false);
	/* still silenced after the check */
	ATF_CHECK(r.silenced);
}

ATF_TC(timed_silence_future);
ATF_TC_HEAD(timed_silence_future, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a timed silence still in the future keeps the rule inactive");
}
ATF_TC_BODY(timed_silence_future, tc)
{
	struct alert_rule r = mkrule();
	time_t now = time(NULL);

	r.silenced = true;
	r.silenced_until = now + 3600;
	ATF_CHECK(alert_rule_active(&r, now) == false);
	ATF_CHECK(r.silenced);	/* not cleared yet */
}

ATF_TC(timed_silence_expired);
ATF_TC_HEAD(timed_silence_expired, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a timed silence past its deadline auto-unsilences and reactivates "
	    "(the bug: alert_silence used to ignore `until`, silencing forever)");
}
ATF_TC_BODY(timed_silence_expired, tc)
{
	struct alert_rule r = mkrule();
	time_t now = time(NULL);

	r.silenced = true;
	r.silenced_until = now - 1;	/* already expired */
	ATF_CHECK(alert_rule_active(&r, now));
	/* the rule is un-silenced in place */
	ATF_CHECK(r.silenced == false);
	ATF_CHECK_EQ((time_t)0, r.silenced_until);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, active_when_enabled);
	ATF_TP_ADD_TC(tp, manual_silence_stays);
	ATF_TP_ADD_TC(tp, timed_silence_future);
	ATF_TP_ADD_TC(tp, timed_silence_expired);

	return (atf_no_error());
}

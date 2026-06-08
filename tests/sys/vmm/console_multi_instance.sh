#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Test: bhyve console multi-instance coexistence (T8.D).
# Case 1 exercises the singleton path via build + -h smoke. Cases 2-4
# are expected_failure until T0.D + T9 (jail(8) allow.fbuf registration)
# land; atf_check -s exit:0 makes the missing-parameter error visible to
# the framework while the body itself still returns 0, which atf_expect_fail
# then matches as a passing expected failure. When T0.D + T9 land, jail
# succeeds, atf_check passes, and the bodies return 0, flipping the
# expected_failure into a real failure that we re-baseline at that time.
#

# Source utils.subr when atf_get_srcdir is set (i.e. real test run);
# during atf-sh introspection atf_get_srcdir is unset, so the include
# is guarded to avoid spurious `set -u` failures.
if [ -n "${atf_get_srcdir:-}" ]; then
	. "${atf_get_srcdir}/utils.subr"
fi

atf_test_case sh_console_bhyve_singleton_unchanged
sh_console_bhyve_singleton_unchanged_head()
{
	atf_set "descr" "Build bhyve and verify -h smoke (singleton unchanged)"
}
sh_console_bhyve_singleton_unchanged_body()
{
	set -eu
	bhyve_src=$(atf_get_srcdir)/../../usr.sbin/bhyve
	bhyve_bin=
	if [ -f "${bhyve_src}/Makefile" ]; then
		atf_check -s exit:0 -o ignore -e ignore \
		    sh -c "cd '${bhyve_src}' && make bhyve 2>&1 | tail -1"
		bhyve_bin="${bhyve_src}/bhyve"
	else
		bhyve_bin=$(command -v bhyve || echo "")
		atf_check -s exit:0 -o ignore -e ignore \
		    test -n "${bhyve_bin}"
	fi
	# Smoke: -h must be recognized; help must mention "bhyve".
	atf_check -s exit:0 -o match:"bhyve" -e ignore \
	    sh -c "'${bhyve_bin}' -h 2>&1 | head -3"
}

# expected_failure: allow.fbuf_not_registered until T0.D + T9 land.
atf_test_case sh_console_two_jails_concurrent
sh_console_two_jails_concurrent_head()
{
	atf_set "descr" "Two jails with allow.fbuf=1 run concurrently"
}
sh_console_two_jails_concurrent_body()
{
	set -eu
	atf_expect_fail "allow.fbuf_not_registered (T0.D + T9 pending)"
	atf_check -s exit:0 -o ignore -e ignore \
	    jail -c name=j1 persist allow.fbuf=1
}

# expected_failure: allow.fbuf_not_registered.
atf_test_case sh_console_jail_and_bhyve_concurrent
sh_console_jail_and_bhyve_concurrent_head()
{
	atf_set "descr" "One jail + bhyve share the console subsystem"
}
sh_console_jail_and_bhyve_concurrent_body()
{
	set -eu
	atf_expect_fail "allow.fbuf_not_registered (T0.D + T9 pending)"
	# Green path: jail + bhyve run together 5s; red path: jail fails.
	atf_check -s exit:0 -o ignore -e ignore \
	    jail -c name=j1 persist allow.fbuf=1
}

# expected_failure: allow.fbuf_not_registered until T9 lands.
atf_test_case sh_console_destroyed_jail_console_freed
sh_console_destroyed_jail_console_freed_head()
{
	atf_set "descr" "Killing a jail frees its console slot for re-use"
}
sh_console_destroyed_jail_console_freed_body()
{
	set -eu
	atf_expect_fail "allow.fbuf_not_registered (T0.D + T9 pending)"
	# Start, kill, restart with same name; allow.fbuf=1 must be accepted.
	atf_check -s exit:0 -o ignore -e ignore \
	    jail -c name=j1 persist allow.fbuf=1
}

# atf-sh's main() invokes atf_init_test_cases as the registration hook;
# all atf_add_test_case calls must live inside it for kyua to enumerate
# the test cases.
atf_init_test_cases()
{
	atf_add_test_case sh_console_bhyve_singleton_unchanged
	atf_add_test_case sh_console_two_jails_concurrent
	atf_add_test_case sh_console_jail_and_bhyve_concurrent
	atf_add_test_case sh_console_destroyed_jail_console_freed
}

#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# Regression wrapper for the runtime perf/stress harnesses
# (tools/ocifbsd-perf.sh, tools/ocifbsd-stress.sh — .plan tasks 1.23–1.26).
#
# These are the same harnesses run by hand on the lab VM, invoked here with
# tiny parameters so the suite exercises the concurrent-lifecycle and
# leak-detection paths on every root-capable test run.  They need jail(2) and a
# /rescue static binary for the rootfs, so both cases skip when not root.

# Resolve the ocifbsd binary the same way lifecycle_test does.
ocifbsd_bin()
{
	local srcdir bin
	srcdir=$(atf_get_srcdir)
	bin="${srcdir}/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ -x "${bin}" ]; then echo "${bin}"; return 0; fi
	command -v ocifbsd 2>/dev/null && return 0
	return 1
}

# Resolve a harness under usr.sbin/ocifbsd/tools relative to the test srcdir.
harness()
{
	local srcdir p
	srcdir=$(atf_get_srcdir)
	p="${srcdir}/../../../usr.sbin/ocifbsd/tools/$1"
	[ -f "${p}" ] && { echo "${p}"; return 0; }
	return 1
}

preflight()
{
	[ "$(id -u)" = "0" ] || atf_skip "requires root (jail(2))"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep required for rootfs"
}

atf_test_case perf_harness_runs
perf_harness_runs_head()
{
	atf_set "descr" "ocifbsd-perf.sh times a few lifecycles and emits valid JSON"
	atf_set "require.user" "root"
}
perf_harness_runs_body()
{
	local bin h
	preflight
	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	h=$(harness ocifbsd-perf.sh) || atf_skip "ocifbsd-perf.sh not found"

	# 2 iterations is enough to exercise create/start/state/kill/delete timing.
	atf_check -s exit:0 -e ignore -o save:perf.json \
	    sh "${h}" -n 2 -b "${bin}"
	# Report must name the harness and carry the phase percentiles.
	grep -q '"harness": "ocifbsd-perf"' perf.json || \
	    atf_fail "perf report missing harness tag: $(cat perf.json)"
	for k in create start state kill delete end_to_end; do
		grep -q "\"${k}\"" perf.json || \
		    atf_fail "perf report missing phase ${k}: $(cat perf.json)"
	done
}

atf_test_case stress_no_leak
stress_no_leak_head()
{
	atf_set "descr" "ocifbsd-stress.sh brings up concurrent jails and tears down with no leak"
	atf_set "require.user" "root"
}
stress_no_leak_body()
{
	local bin h
	preflight
	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	h=$(harness ocifbsd-stress.sh) || atf_skip "ocifbsd-stress.sh not found"

	# Small: 3 concurrent x 2 rounds. Exit 0 means every round returned to the
	# jail baseline; a leak makes the harness exit non-zero.
	atf_check -s exit:0 -e ignore -o save:stress.json \
	    sh "${h}" -c 3 -r 2 -b "${bin}"
	grep -q '"clean": true' stress.json || \
	    atf_fail "stress reported a leak or launch failure: $(cat stress.json)"
	grep -q '"launch_failures": 0' stress.json || \
	    atf_fail "stress had launch failures: $(cat stress.json)"
}

atf_init_test_cases()
{
	atf_add_test_case perf_harness_runs
	atf_add_test_case stress_no_leak
}

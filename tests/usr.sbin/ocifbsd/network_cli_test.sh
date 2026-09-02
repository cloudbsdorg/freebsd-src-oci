#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# Integration test for the named-network management CLI
# (`ocifbsd network create|ls|inspect|rm` — .plan tasks 3.5–3.7). Exercises the
# real network resource library, which builds an if_bridge, so it needs root
# and skips otherwise.

ocifbsd_bin()
{
	local srcdir bin
	srcdir=$(atf_get_srcdir)
	bin="${srcdir}/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ -x "${bin}" ]; then echo "${bin}"; return 0; fi
	command -v ocifbsd 2>/dev/null && return 0
	return 1
}

atf_test_case network_crud cleanup
network_crud_head()
{
	atf_set "descr" "network create/ls/inspect/rm builds and destroys a bridge with no leak"
	atf_set "require.user" "root"
}
network_crud_body()
{
	local bin id

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"

	# create — stdout must be exactly the 32-hex network id, nothing else
	# (a regression once leaked ifconfig chatter onto this line).
	atf_check -s exit:0 -e empty -o save:create.out \
	    "${bin}" network create --subnet 172.31.9.0/24 \
	    --gateway 172.31.9.1 atftest0
	id=$(tr -d ' \t\r\n' < create.out)
	if ! expr "${id}" : '[0-9a-f]\{32\}$' >/dev/null 2>&1; then
		atf_fail "create did not print a clean 32-hex id (got: ${id})"
	fi

	# the bridge must actually exist
	atf_check -s exit:0 -o match:"ocifbsdatftest0" \
	    ifconfig ocifbsdatftest0

	# ls must show the network with its parsed fields
	atf_check -s exit:0 -o match:"atftest0" -o match:"ocifbsdatftest0" \
	    "${bin}" network ls

	# inspect by name yields JSON carrying the subnet/gateway we set
	atf_check -s exit:0 -o match:"172.31.9.0/24" -o match:"172.31.9.1" \
	    "${bin}" network inspect atftest0

	# rm by name; the bridge must be gone afterwards
	atf_check -s exit:0 -o ignore "${bin}" network rm atftest0
	if ifconfig ocifbsdatftest0 >/dev/null 2>&1; then
		atf_fail "bridge ocifbsdatftest0 leaked after network rm"
	fi

	# ls no longer lists it
	atf_check -s exit:0 -o not-match:"atftest0" "${bin}" network ls
}
network_crud_cleanup()
{
	# Best-effort teardown if an assertion aborted mid-test. Scoped to this
	# test's own objects: destroy its bridge and remove only the state file
	# naming atftest0, never other networks on the host.
	ifconfig ocifbsdatftest0 destroy 2>/dev/null
	for f in /var/run/ocifbsd/networks/*.json; do
		[ -f "${f}" ] || continue
		if grep -q '"name": "atftest0"' "${f}" 2>/dev/null; then
			rm -f "${f}"
		fi
	done
	return 0
}

atf_init_test_cases()
{
	atf_add_test_case network_crud
}

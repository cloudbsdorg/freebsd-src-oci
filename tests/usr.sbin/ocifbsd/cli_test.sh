#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 CloudBSD
#
# CLI smoke tests for the ocifbsd binary using atf_check.
# Requires a built binary at ../../../usr.sbin/ocifbsd/ocifbsd
# relative to the test source directory (in-tree build).

atf_test_case help_lists_commands
help_lists_commands_head()
{
	atf_set "descr" "ocifbsd --help lists lifecycle commands"
}
help_lists_commands_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	# usage is printed on stderr
	atf_check -s exit:0 -e match:"create" -e match:"start" \
	    -e match:"kill" -e match:"delete" -e match:"state" \
	    "${bin}" --help
}

atf_test_case version_prints
version_prints_head()
{
	atf_set "descr" "ocifbsd -V prints version string"
}
version_prints_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s exit:0 -o match:"0\\.1\\.0" "${bin}" -V
}

atf_test_case unknown_command_fails
unknown_command_fails_head()
{
	atf_set "descr" "unknown subcommand exits non-zero"
}
unknown_command_fails_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s not-exit:0 -e ignore "${bin}" not-a-real-command
}

atf_init_test_cases()
{
	atf_add_test_case help_lists_commands
	atf_add_test_case version_prints
	atf_add_test_case unknown_command_fails
}

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
	    -e match:"pull" -e match:"images" \
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

atf_test_case pull_dry_run
pull_dry_run_head()
{
	atf_set "descr" "pull --dry-run parses an OCI reference"
}
pull_dry_run_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		# try MAKEOBJDIRPREFIX layout via sibling
		if [ -n "${MAKEOBJDIRPREFIX}" ]; then
			:
		fi
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s exit:0 \
	    -o match:"registry=ghcr.io" \
	    -o match:"repository=cloudbsd/ocifbsd" \
	    -o match:"tag=latest" \
	    -o match:"store_path=" \
	    "${bin}" pull --dry-run ghcr.io/cloudbsd/ocifbsd:latest
}

atf_test_case images_empty_ok
images_empty_ok_head()
{
	atf_set "descr" "images succeeds even if store is empty/missing"
}
images_empty_ok_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s exit:0 -o ignore -e ignore "${bin}" images
}

atf_init_test_cases()
{
	atf_add_test_case help_lists_commands
	atf_add_test_case version_prints
	atf_add_test_case unknown_command_fails
	atf_add_test_case pull_dry_run
	atf_add_test_case images_empty_ok
}

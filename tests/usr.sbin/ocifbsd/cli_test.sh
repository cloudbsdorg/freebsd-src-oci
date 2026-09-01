#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
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
	    -e match:"pull" -e match:"images" -e match:"rmi" \
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

atf_test_case pull_invalid_ref
pull_invalid_ref_head()
{
	atf_set "descr" "pull rejects empty/invalid references"
}
pull_invalid_ref_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	# empty string after parse_reference should fail
	atf_check -s not-exit:0 -e ignore "${bin}" pull --dry-run ""
}

atf_test_case pull_real_unreachable
pull_real_unreachable_head()
{
	atf_set "descr" "pull without dry-run fails cleanly on unreachable host"
}
pull_real_unreachable_body()
{
	local bin store

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore
	mkdir -p "${store}"
	# 127.0.0.1:1 — nothing listening; must not hang forever (curl timeout default)
	# Use a host that fails DNS quickly
	export OCIFBSD_DATA_DIR="${store}"
	atf_check -s not-exit:0 -o ignore -e ignore \
	    env OCIFBSD_DATA_DIR="${store}" \
	    "${bin}" pull not-a-real-registry.invalid/ocifbsd/test:latest
}

atf_test_case rmi_missing_fails
rmi_missing_fails_head()
{
	atf_set "descr" "rmi fails when image is not in the local store"
}
rmi_missing_fails_body()
{
	local bin store

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore-rmi
	mkdir -p "${store}"
	atf_check -s not-exit:0 -e ignore \
	    env OCIFBSD_DATA_DIR="${store}" \
	    "${bin}" rmi ghcr.io/cloudbsd/does-not-exist:latest
}

atf_test_case rmi_removes_store
rmi_removes_store_head()
{
	atf_set "descr" "rmi deletes a local image store directory"
}
rmi_removes_store_body()
{
	local bin store img

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore-rmi2
	img="${store}/ghcr.io/cloudbsd/demo/latest"
	mkdir -p "${img}/rootfs"
	echo '{}' > "${img}/config.json"
	# rmi checks container state to refuse removing an in-use image; give it
	# a readable, test-owned state dir (the default /var/run/ocifbsd is
	# root-only, and rmi now fails closed when it cannot read state).
	mkdir -p "$(pwd)/state"
	atf_check -s exit:0 -o match:"deleted=" \
	    env OCIFBSD_DATA_DIR="${store}" OCIFBSD_STATE_DIR="$(pwd)/state" \
	    "${bin}" rmi ghcr.io/cloudbsd/demo:latest
	if [ -d "${img}" ]; then
		atf_fail "image store still present after rmi"
	fi
}

atf_test_case create_image_missing_fails
create_image_missing_fails_head()
{
	atf_set "descr" "create --image fails when store is not ready"
}
create_image_missing_fails_body()
{
	local bin store

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore-create
	mkdir -p "${store}"
	atf_check -s not-exit:0 -e match:"image not ready" \
	    env OCIFBSD_DATA_DIR="${store}" \
	    "${bin}" create --image ghcr.io/cloudbsd/missing:latest
}

atf_test_case pull_dry_run_official
pull_dry_run_official_head()
{
	atf_set "descr" "pull --dry-run maps official image to library/"
}
pull_dry_run_official_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s exit:0 \
	    -o match:"registry=docker.io" \
	    -o match:"repository=library/hello-world" \
	    -o match:"tag=latest" \
	    "${bin}" pull --dry-run hello-world:latest
}

atf_test_case create_missing_bundle_fails
create_missing_bundle_fails_head()
{
	atf_set "descr" "create without args or missing bundle fails"
}
create_missing_bundle_fails_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s not-exit:0 -e ignore "${bin}" create
	atf_check -s not-exit:0 -e ignore \
	    "${bin}" create /nonexistent/ocifbsd-bundle-$$
}

atf_test_case run_image_missing_fails
run_image_missing_fails_head()
{
	atf_set "descr" "run --image fails when store is not ready"
}
run_image_missing_fails_body()
{
	local bin store

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore-run
	mkdir -p "${store}"
	atf_check -s not-exit:0 -e match:"image not ready" \
	    env OCIFBSD_DATA_DIR="${store}" \
	    "${bin}" run --image ghcr.io/cloudbsd/missing:latest
}

atf_test_case start_missing_fails
start_missing_fails_head()
{
	atf_set "descr" "start/state/kill/delete fail on missing container"
}
start_missing_fails_body()
{
	local bin id

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	id="0000000000000000000000000000000000000000000000000000000000000000"
	atf_check -s not-exit:0 -e ignore "${bin}" start "${id}"
	atf_check -s not-exit:0 -e ignore "${bin}" state "${id}"
	atf_check -s not-exit:0 -e ignore "${bin}" kill "${id}"
	atf_check -s not-exit:0 -e ignore "${bin}" delete "${id}"
}

atf_test_case list_empty_ok
list_empty_ok_head()
{
	atf_set "descr" "list succeeds with empty container set"
}
list_empty_ok_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	# Run against a private, test-owned state directory. The real
	# /var/run/ocifbsd is intentionally root:ocifbsd 0750 and is not
	# readable by an unprivileged test user; OCIFBSD_STATE_DIR redirects
	# it so this smoke test exercises argument handling in isolation.
	atf_check -s exit:0 -o ignore -e ignore \
	    env OCIFBSD_STATE_DIR="${PWD}/state" "${bin}" list
}

atf_test_case images_respects_data_dir
images_respects_data_dir_head()
{
	atf_set "descr" "images lists real image roots under OCIFBSD_DATA_DIR"
}
images_respects_data_dir_body()
{
	local bin store img

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store=$(pwd)/imgstore-list
	img="${store}/ghcr.io/demo/latest"
	# A usable image root has config.json + rootfs/. A bare directory
	# with neither must NOT be listed.
	mkdir -p "${img}/rootfs"
	echo '{}' > "${img}/config.json"
	mkdir -p "${store}/ghcr.io/empty/notanimage"
	atf_check -s exit:0 -o match:"ghcr.io/demo" -o match:"latest" \
	    env OCIFBSD_DATA_DIR="${store}" "${bin}" images
	atf_check -s exit:0 -o not-match:"notanimage" \
	    env OCIFBSD_DATA_DIR="${store}" "${bin}" images
}

atf_test_case pull_no_args_fails
pull_no_args_fails_head()
{
	atf_set "descr" "pull without reference fails"
}
pull_no_args_fails_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	atf_check -s not-exit:0 -e ignore "${bin}" pull
	atf_check -s not-exit:0 -e ignore "${bin}" rmi
}

atf_test_case lock_file_created
lock_file_created_head()
{
	atf_set "descr" "a lifecycle op creates a per-container lock file (0640)"
}
lock_file_created_body()
{
	local bin

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	mkdir -p "${PWD}/state"
	# 'stop' on a missing id still takes and releases the per-container
	# lock before reporting not-found; the lock file is left as a marker.
	atf_check -s not-exit:0 -e ignore \
	    env OCIFBSD_STATE_DIR="${PWD}/state" "${bin}" stop lockcheck-id
	atf_check -s exit:0 test -f "${PWD}/state/lockcheck-id.lock"
	# root/ocifbsd-group only: no world/other access bits.
	mode=$(stat -f "%Lp" "${PWD}/state/lockcheck-id.lock")
	atf_check_equal "640" "${mode}"
}

atf_test_case lock_excludes_concurrent
lock_excludes_concurrent_head()
{
	atf_set "descr" "a held lock blocks a concurrent lifecycle op"
}
lock_excludes_concurrent_body()
{
	local bin start end rc

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	if ! command -v lockf >/dev/null 2>&1; then
		atf_skip "lockf(1) not available to hold the lock"
	fi
	mkdir -p "${PWD}/state"
	# lockf(1) and ocifbsd both use flock(2) on the same path, so holding
	# the lock externally must make ocifbsd's blocking acquire wait. The
	# holder keeps the lock well past the timeout window (5s > 0.5s start
	# offset + 2s timeout) so the block is unambiguous, not a boundary race.
	lockf -k "${PWD}/state/contend-id.lock" sh -c 'sleep 5' &
	holder=$!
	sleep 1
	start=$(date +%s)
	# 2s wall clock while the holder still holds the lock: must time out.
	timeout 2 env OCIFBSD_STATE_DIR="${PWD}/state" \
	    "${bin}" stop contend-id >/dev/null 2>&1
	rc=$?
	end=$(date +%s)
	wait "${holder}" 2>/dev/null || true
	atf_check_equal "124" "${rc}"		# 124 == timed out => blocked
	if [ $((end - start)) -lt 2 ]; then
		atf_fail "ocifbsd did not block on the held lock"
	fi
	# After release it completes promptly (not-found, exit != 0).
	atf_check -s not-exit:0 -e ignore \
	    env OCIFBSD_STATE_DIR="${PWD}/state" "${bin}" stop contend-id
}

atf_test_case rmi_symlink_escape_safe
rmi_symlink_escape_safe_head()
{
	atf_set "descr" "rmi does not follow a symlink out of the image store"
}
rmi_symlink_escape_safe_body()
{
	local bin store victim

	bin="$(atf_get_srcdir)/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ ! -x "${bin}" ]; then
		atf_skip "ocifbsd binary not built at ${bin}"
	fi
	store="${PWD}/data"
	# A valid store for ref evil.io/x:latest (config.json + rootfs/).
	mkdir -p "${store}/evil.io/x/latest/rootfs/sub"
	echo '{}' > "${store}/evil.io/x/latest/config.json"
	echo hi > "${store}/evil.io/x/latest/rootfs/file.txt"
	# An external victim and symlinks inside the store pointing at it.
	mkdir -p "${PWD}/victim"
	echo precious > "${PWD}/victim/keep.txt"
	ln -s "${PWD}/victim" "${store}/evil.io/x/latest/rootfs/escape_dir"
	ln -s "${PWD}/victim/keep.txt" \
	    "${store}/evil.io/x/latest/rootfs/escape_file"
	atf_check -s exit:0 -o ignore -e ignore \
	    env OCIFBSD_DATA_DIR="${store}" OCIFBSD_STATE_DIR="${PWD}/state" \
	    "${bin}" rmi evil.io/x:latest
	# The store tree is gone...
	if [ -e "${store}/evil.io/x/latest" ]; then
		atf_fail "image store was not removed"
	fi
	# ...but the symlink targets outside the store are untouched.
	atf_check -s exit:0 test -f "${PWD}/victim/keep.txt"
	atf_check -s exit:0 -o match:"precious" cat "${PWD}/victim/keep.txt"
}

atf_init_test_cases()
{
	atf_add_test_case help_lists_commands
	atf_add_test_case version_prints
	atf_add_test_case unknown_command_fails
	atf_add_test_case pull_dry_run
	atf_add_test_case pull_dry_run_official
	atf_add_test_case images_empty_ok
	atf_add_test_case images_respects_data_dir
	atf_add_test_case pull_invalid_ref
	atf_add_test_case pull_no_args_fails
	atf_add_test_case pull_real_unreachable
	atf_add_test_case rmi_missing_fails
	atf_add_test_case rmi_removes_store
	atf_add_test_case create_image_missing_fails
	atf_add_test_case create_missing_bundle_fails
	atf_add_test_case run_image_missing_fails
	atf_add_test_case start_missing_fails
	atf_add_test_case list_empty_ok
	atf_add_test_case lock_file_created
	atf_add_test_case lock_excludes_concurrent
	atf_add_test_case rmi_symlink_escape_safe
}

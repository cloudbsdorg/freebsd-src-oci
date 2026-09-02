#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# Integration test: create → start → state → kill → delete using a
# minimal OCI bundle and a static /rescue binary as the jail rootfs.
# Requires root (jail_create / jail_remove).

# Resolve ocifbsd binary: prefer sibling objdir layout under MAKEOBJDIRPREFIX
ocifbsd_bin()
{
	local srcdir bin

	srcdir=$(atf_get_srcdir)
	# From tests/usr.sbin/ocifbsd → usr.sbin/ocifbsd/ocifbsd (src or obj)
	bin="${srcdir}/../../../usr.sbin/ocifbsd/ocifbsd"
	if [ -x "${bin}" ]; then
		echo "${bin}"
		return 0
	fi
	# Fall back to PATH
	if command -v ocifbsd >/dev/null 2>&1; then
		command -v ocifbsd
		return 0
	fi
	return 1
}

make_bundle()
{
	local bundle rootfs

	bundle=$(pwd)/bundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin" "${rootfs}/tmp"
	# /rescue/* are static on FreeBSD — ideal for a minimal jail rootfs
	if [ ! -x /rescue/sleep ]; then
		atf_skip "/rescue/sleep not available"
	fi
	cp /rescue/sleep "${rootfs}/bin/sleep"
	chmod 755 "${rootfs}/bin/sleep"

	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "hostname": "ocifbsd-life",
  "process": {
    "terminal": false,
    "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ],
    "env": [ "PATH=/bin", "TERM=xterm" ],
    "cwd": "/"
  },
  "root": {
    "path": "rootfs",
    "readonly": false
  }
}
EOF
	echo "${bundle}"
}

atf_test_case create_start_kill_delete cleanup
create_start_kill_delete_head()
{
	atf_set "descr" "create/start/state/kill/delete lifecycle on FreeBSD jail"
	atf_set "require.user" "root"
}
create_start_kill_delete_body()
{
	local bin bundle cid name

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	bundle=$(make_bundle)
	name="life$$"

	# create (prints container id on stdout)
	atf_check -s exit:0 -e ignore -o save:create.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < create.out)
	if ! expr "${cid}" : '[0-9a-f]\{64\}$' >/dev/null 2>&1; then
		atf_fail "create did not print a 64-hex id (got: ${cid})"
	fi

	# state after create should mention created or the id
	atf_check -s exit:0 -e ignore -o save:state1.out \
	    "${bin}" state "${cid}"
	if ! grep -qiE "created|${cid}" state1.out; then
		atf_fail "state after create unexpected: $(cat state1.out)"
	fi

	# start (may print id on stdout)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	# running: jail should exist
	atf_check -s exit:0 -o match:"ocifbsd-" jls -n name

	# kill (SIGTERM default)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" kill "${cid}"

	# delete
	atf_check -s exit:0 -e ignore -o ignore \
	    "${bin}" delete --force "${cid}" || \
	    atf_check -s exit:0 -e ignore -o ignore "${bin}" delete "${cid}"

	# gone from jls
	if jls -n name 2>/dev/null | grep -q "ocifbsd-"; then
		# may still list other ocifbsd jails; ensure ours is gone
		if jls -n name | grep -q "${cid}"; then
			atf_fail "jail still present after delete"
		fi
	fi
}
create_start_kill_delete_cleanup()
{
	local bin cid

	bin=$(ocifbsd_bin) || return 0
	# Best-effort cleanup of leftover test jails/containers
	for cid in $(jls -n name 2>/dev/null | sed -n 's/.*name=ocifbsd-\([0-9a-f]*\).*/\1/p'); do
		"${bin}" delete "ocifbsd-${cid}" --force 2>/dev/null || true
		"${bin}" delete "${cid}" --force 2>/dev/null || true
	done
	# remove by name pattern
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		jail -r "${j}" 2>/dev/null || true
	done
}

atf_test_case create_rejects_missing_bundle
create_rejects_missing_bundle_head()
{
	atf_set "descr" "create fails on missing bundle path"
	atf_set "require.user" "root"
}
create_rejects_missing_bundle_body()
{
	local bin

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	atf_check -s not-exit:0 -e ignore \
	    "${bin}" create /nonexistent/ocifbsd-bundle-$$ 
}

atf_test_case create_start_with_nullfs cleanup
create_start_with_nullfs_head()
{
	atf_set "descr" "nullfs mount applied on start and cleaned on delete"
	atf_set "require.user" "root"
}
create_start_with_nullfs_body()
{
	local bin bundle cid name hostsrc

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	bundle=$(make_bundle)
	name="mnt$$"
	hostsrc=$(pwd)/hostsrc
	mkdir -p "${hostsrc}" "${bundle}/rootfs/data"
	echo hello > "${hostsrc}/marker"

	# Rewrite config with a nullfs mount into /data
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "hostname": "ocifbsd-mnt",
  "process": {
    "terminal": false,
    "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ],
    "env": [ "PATH=/bin" ],
    "cwd": "/"
  },
  "root": { "path": "rootfs", "readonly": false },
  "mounts": [
    {
      "destination": "/data",
      "type": "nullfs",
      "source": "${hostsrc}",
      "options": [ "ro" ]
    }
  ]
}
EOF

	atf_check -s exit:0 -e ignore -o save:create.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < create.out)

	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	# Mount should be visible on the host under the rootfs path
	if ! mount | grep -q "${bundle}/rootfs/data"; then
		atf_fail "nullfs mount not present after start"
	fi
	if [ ! -f "${bundle}/rootfs/data/marker" ]; then
		atf_fail "marker not visible through nullfs mount"
	fi

	atf_check -s exit:0 -e ignore -o ignore "${bin}" kill "${cid}"
	atf_check -s exit:0 -e ignore -o ignore \
	    "${bin}" delete --force "${cid}" || \
	    atf_check -s exit:0 -e ignore -o ignore "${bin}" delete "${cid}"

	# Mount should be gone after delete
	if mount | grep -q "${bundle}/rootfs/data"; then
		atf_fail "nullfs mount still present after delete"
	fi
}
create_start_with_nullfs_cleanup()
{
	local bin mp

	bin=$(ocifbsd_bin) || return 0
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		"${bin}" delete --force "${j#ocifbsd-}" 2>/dev/null || true
		jail -r "${j}" 2>/dev/null || true
	done
	# leftover mounts from failed runs (any nullfs under work dir)
	mount -p 2>/dev/null | awk '$2 ~ /rootfs\/data$/ { print $2 }' |
	    while read -r mp; do
		umount -f "${mp}" 2>/dev/null || true
	done
	# also try the atf work directory path if still mounted
	if [ -d "$(pwd)/bundle/rootfs/data" ]; then
		umount -f "$(pwd)/bundle/rootfs/data" 2>/dev/null || true
	fi
}

atf_test_case rmi_refuses_in_use
rmi_refuses_in_use_head()
{
	atf_set "descr" "rmi refuses to remove an image a container references"
	atf_set "require.user" "root"
}
rmi_refuses_in_use_body()
{
	local bin data store ref cid

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	if [ ! -x /rescue/sh ]; then
		atf_skip "/rescue/sh not available"
	fi
	data=$(pwd)/data
	ref="local.test/inuse:latest"
	store="${data}/local.test/inuse/latest"
	mkdir -p "${store}/rootfs/bin"
	cp /rescue/sh "${store}/rootfs/bin/sh"
	cat > "${store}/config.json" <<-'JSON'
	{ "ociVersion": "1.0.0",
	  "root": { "path": "rootfs", "readonly": true },
	  "process": { "args": ["/bin/sh"] } }
	JSON

	export OCIFBSD_STATE_DIR="$(pwd)/state"
	export OCIFBSD_DATA_DIR="${data}"
	mkdir -p "${OCIFBSD_STATE_DIR}"

	cid=$("${bin}" create --image "${ref}") ||
	    atf_fail "create --image failed"

	# In use -> refused, and the image store must survive the refusal.
	atf_check -s not-exit:0 -e match:"in use" \
	    "${bin}" rmi "${ref}"
	atf_check -s exit:0 test -d "${store}"

	# --force removes it even while referenced.
	atf_check -s exit:0 -o ignore "${bin}" rmi --force "${ref}"
	atf_check -s exit:0 test '!' -d "${store}"

	"${bin}" delete "${cid}" >/dev/null 2>&1 || true
}

atf_test_case readonly_nosuid_root cleanup
readonly_nosuid_root_head()
{
	atf_set "descr" "root.readonly + noNewPrivileges are enforced: the jail root is a read-only, nosuid nullfs and a write inside is blocked"
	atf_set "require.user" "root"
}
readonly_nosuid_root_body()
{
	local bin bundle rootfs cid name jname

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep not available"
	bundle=$(pwd)/robundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin"
	cp /rescue/sleep "${rootfs}/bin/sleep"; chmod 755 "${rootfs}/bin/sleep"
	cp /rescue/sh "${rootfs}/bin/sh"; chmod 755 "${rootfs}/bin/sh"
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": { "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ], "cwd": "/",
    "noNewPrivileges": true },
  "root": { "path": "rootfs", "readonly": true }
}
EOF
	name="rolife$$"
	atf_check -s exit:0 -e ignore -o save:c.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < c.out)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	# The jail root must be a read-only, nosuid nullfs overlay.
	atf_check -s exit:0 -o match:"read-only" \
	    sh -c "mount | grep '\.jailroot'"
	atf_check -s exit:0 -o match:"nosuid" \
	    sh -c "mount | grep '\.jailroot'"

	# A write to the container root must be blocked (read-only fs). Report
	# BLOCKED/WROTE and always exit 0 so the assertion keys off the output,
	# not the shell's redirect-failure exit status.
	jname=$(jls -q name | grep '^ocifbsd-' | head -1)
	atf_check -s exit:0 -e ignore -o match:"BLOCKED" -o not-match:"WROTE" \
	    jexec "${jname}" /bin/sh -c \
	    'if echo x > /rotest 2>/dev/null; then echo WROTE; else echo BLOCKED; fi'

	# Delete tears the overlay down: no jailroot mount is left behind.
	atf_check -s exit:0 -e ignore -o ignore "${bin}" delete --force "${cid}"
	if mount | grep -q '\.jailroot'; then
		atf_fail "jailroot nullfs still mounted after delete"
	fi
}
readonly_nosuid_root_cleanup()
{
	local m j
	for m in $(mount | grep '\.jailroot' | awk '{print $3}'); do
		umount -f "${m}" 2>/dev/null || true
	done
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		jail -r "${j}" 2>/dev/null || true
	done
}

atf_init_test_cases()
{
	atf_add_test_case create_start_kill_delete
	atf_add_test_case create_rejects_missing_bundle
	atf_add_test_case create_start_with_nullfs
	atf_add_test_case rmi_refuses_in_use
	atf_add_test_case readonly_nosuid_root
}

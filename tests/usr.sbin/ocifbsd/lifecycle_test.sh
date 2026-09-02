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

	# state after create must follow the OCI runtime `state` schema:
	# ociVersion, id, status (created), and bundle.
	atf_check -s exit:0 -e ignore -o save:state1.out \
	    "${bin}" state "${cid}"
	for field in ociVersion '"id"' created bundle; do
		if ! grep -q "${field}" state1.out; then
			atf_fail "state missing OCI field ${field}: $(cat state1.out)"
		fi
	done

	# start (may print id on stdout)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	# running: jail should exist
	atf_check -s exit:0 -o match:"ocifbsd-" jls -n name

	# state after start must report running and carry the process pid.
	atf_check -s exit:0 -e ignore -o save:state2.out \
	    "${bin}" state "${cid}"
	for field in running '"pid"' ociVersion; do
		if ! grep -q "${field}" state2.out; then
			atf_fail "running state missing ${field}: $(cat state2.out)"
		fi
	done

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

	# The jail root must be a read-only, nosuid nullfs overlay (match this
	# container's own mountpoint, not any stray .jailroot from another run).
	atf_check -s exit:0 -o match:"read-only" \
	    sh -c "mount | grep '${cid}.jailroot'"
	atf_check -s exit:0 -o match:"nosuid" \
	    sh -c "mount | grep '${cid}.jailroot'"

	# A write to the container root must be blocked (read-only fs). Report
	# BLOCKED/WROTE and always exit 0 so the assertion keys off the output,
	# not the shell's redirect-failure exit status.
	jname=$(jls -q name | grep '^ocifbsd-' | head -1)
	atf_check -s exit:0 -e ignore -o match:"BLOCKED" -o not-match:"WROTE" \
	    jexec "${jname}" /bin/sh -c \
	    'if echo x > /rotest 2>/dev/null; then echo WROTE; else echo BLOCKED; fi'

	# Delete tears the overlay down: no jailroot mount is left behind.
	atf_check -s exit:0 -e ignore -o ignore "${bin}" delete --force "${cid}"
	if mount | grep -q "${cid}.jailroot"; then
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

atf_test_case rctl_limits_applied_and_cleaned cleanup
rctl_limits_applied_and_cleaned_head()
{
	atf_set "descr" "a bundle with linux.resources gets RCTL rules on " \
	    "start and they are removed on delete (enforced when RACCT is on, " \
	    "gracefully skipped when it is off)"
	atf_set "require.user" "root"
}
rctl_limits_applied_and_cleaned_body()
{
	local bin bundle rootfs cid name jname

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep not available"
	bundle=$(pwd)/rctlbundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin"
	cp /rescue/sleep "${rootfs}/bin/sleep"; chmod 755 "${rootfs}/bin/sleep"
	# 128 MiB memoryuse limit and a 50%-of-one-core CPU limit via the OCI
	# linux.resources.memory.limit and cpu.quota/period fields.
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": { "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ], "cwd": "/" },
  "root": { "path": "rootfs", "readonly": false },
  "linux": { "resources": {
    "memory": { "limit": 134217728 },
    "cpu": { "quota": 50000, "period": 100000 } } }
}
EOF
	name="rctllife$$"
	atf_check -s exit:0 -e ignore -o save:c.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < c.out)

	if [ "$(sysctl -n kern.racct.enable 2>/dev/null)" = "1" ]; then
		# RACCT/RCTL enabled: the rule must be applied on start and
		# gone after delete.
		atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"
		jname=$(jls -q name | grep '^ocifbsd-' | head -1)
		# rctl(8) lists a subject's rules as `rctl <subject>` (the -l
		# form takes a different subject syntax and does not match here).
		atf_check -s exit:0 -o match:"memoryuse" \
		    sh -c "rctl jail:${jname} 2>/dev/null"
		# cpu.quota 50000 / period 100000 -> pcpu:...=50 (not the raw
		# microsecond quota).
		atf_check -s exit:0 -o match:"pcpu:deny=50" \
		    sh -c "rctl jail:${jname} 2>/dev/null"
		atf_check -s exit:0 -e ignore -o ignore \
		    "${bin}" delete --force "${cid}"
		# No rule may survive the container.
		atf_check -s exit:0 -o not-match:"memoryuse" \
		    sh -c "rctl jail:${jname} 2>/dev/null || true"
	else
		# RACCT/RCTL disabled: start must still succeed and warn rather
		# than fail, so containers run on hosts without accounting.
		atf_check -s exit:0 -o match:"RACCT/RCTL is unavailable" \
		    -o match:"limits not applied" \
		    sh -c "${bin} start ${cid} 2>&1"
		atf_check -s exit:0 -e ignore -o ignore \
		    "${bin}" delete --force "${cid}"
	fi
}
rctl_limits_applied_and_cleaned_cleanup()
{
	local j
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		rctl -r "jail:${j}" 2>/dev/null || true
		jail -r "${j}" 2>/dev/null || true
	done
}

atf_test_case vnet_epair_wired_and_cleaned cleanup
vnet_epair_wired_and_cleaned_head()
{
	atf_set "descr" "a freebsd.vnet container gets an epair moved into its " \
	    "jail with the configured IP, and the host epair is destroyed on " \
	    "delete (no leak)"
	atf_set "require.user" "root"
}
vnet_epair_wired_and_cleaned_body()
{
	local bin bundle rootfs cid name jname before after

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep not available"
	[ -x /rescue/ifconfig ] || atf_skip "/rescue/ifconfig not available"

	before=$(ifconfig -l | tr ' ' '\n' | grep -c '^epair')

	bundle=$(pwd)/vnetbundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin" "${rootfs}/sbin"
	cp /rescue/sleep "${rootfs}/bin/sleep"; chmod 755 "${rootfs}/bin/sleep"
	# A minimal image ships ifconfig so the runtime can set the address.
	cp /rescue/ifconfig "${rootfs}/sbin/ifconfig"
	chmod 755 "${rootfs}/sbin/ifconfig"
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": { "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ], "cwd": "/" },
  "root": { "path": "rootfs", "readonly": false },
  "freebsd": { "vnet": true, "ip4": [ "192.0.2.20/24" ] }
}
EOF
	name="vnetlife$$"
	atf_check -s exit:0 -e ignore -o save:c.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < c.out)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	jname=$(jls -q name | grep '^ocifbsd-' | head -1)
	# The jail must have an epair interface, carrying the configured IP.
	atf_check -s exit:0 -o match:"epair" \
	    sh -c "jexec ${jname} ifconfig -l"
	atf_check -s exit:0 -o match:"192.0.2.20" \
	    sh -c "jexec ${jname} ifconfig"

	atf_check -s exit:0 -e ignore -o ignore "${bin}" delete --force "${cid}"

	# The host-side epair must be gone: no net increase over the baseline.
	after=$(ifconfig -l | tr ' ' '\n' | grep -c '^epair')
	if [ "${after}" -gt "${before}" ]; then
		atf_fail "epair leaked: before=${before} after=${after}"
	fi
}
vnet_epair_wired_and_cleaned_cleanup()
{
	local j e
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		jail -r "${j}" 2>/dev/null || true
	done
	# Destroy any ocifbsd-created epairs left by a failed run.
	for e in $(ifconfig -l | tr ' ' '\n' | grep '^epair'); do
		ifconfig "${e}" destroy 2>/dev/null || true
	done
	rm -f /var/run/ocifbsd/*.epair 2>/dev/null || true
}

atf_test_case vnet_bridge_attach cleanup
vnet_bridge_attach_head()
{
	atf_set "descr" "a freebsd.bridge container has its host epair added as " \
	    "a member of the named bridge, removed on delete"
	atf_set "require.user" "root"
}
vnet_bridge_attach_body()
{
	local bin bundle rootfs cid name

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep not available"
	[ -x /rescue/ifconfig ] || atf_skip "/rescue/ifconfig not available"

	bundle=$(pwd)/brbundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin" "${rootfs}/sbin"
	cp /rescue/sleep "${rootfs}/bin/sleep"; chmod 755 "${rootfs}/bin/sleep"
	cp /rescue/ifconfig "${rootfs}/sbin/ifconfig"
	chmod 755 "${rootfs}/sbin/ifconfig"
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": { "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ], "cwd": "/" },
  "root": { "path": "rootfs", "readonly": false },
  "freebsd": { "vnet": true, "ip4": [ "192.0.2.30/24" ],
    "bridge": "ocibr0" }
}
EOF
	name="brlife$$"
	atf_check -s exit:0 -e ignore -o save:c.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < c.out)
	atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"

	# The bridge exists (created by the runtime) and has an epair member.
	atf_check -s exit:0 -o match:"member: epair" \
	    sh -c "ifconfig ocibr0"

	atf_check -s exit:0 -e ignore -o ignore "${bin}" delete --force "${cid}"
	# The epair member is gone after delete (destroying it left the bridge).
	atf_check -s exit:0 -o not-match:"member: epair" \
	    sh -c "ifconfig ocibr0 2>/dev/null || true"
}
vnet_bridge_attach_cleanup()
{
	local j e
	for j in $(jls -q name 2>/dev/null | grep '^ocifbsd-'); do
		jail -r "${j}" 2>/dev/null || true
	done
	for e in $(ifconfig -l | tr ' ' '\n' | grep '^epair'); do
		ifconfig "${e}" destroy 2>/dev/null || true
	done
	ifconfig ocibr0 destroy 2>/dev/null || true
	rm -f /var/run/ocifbsd/*.epair 2>/dev/null || true
}

atf_test_case mac_label_applied_or_warned cleanup
mac_label_applied_or_warned_head()
{
	atf_set "descr" "freebsd.macLabel is applied to the container init when " \
	    "its MAC policy is loaded, and warned as unenforceable otherwise"
	atf_set "require.user" "root"
}
mac_label_applied_or_warned_body()
{
	local bin bundle rootfs cid name jname

	bin=$(ocifbsd_bin) || atf_skip "ocifbsd binary not found"
	[ -x /rescue/sleep ] || atf_skip "/rescue/sleep not available"
	bundle=$(pwd)/macbundle
	rootfs=${bundle}/rootfs
	mkdir -p "${rootfs}/bin"
	cp /rescue/sleep "${rootfs}/bin/sleep"; chmod 755 "${rootfs}/bin/sleep"
	cat > "${bundle}/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": { "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "120" ], "cwd": "/" },
  "root": { "path": "rootfs", "readonly": false },
  "freebsd": { "macLabel": "biba/high" }
}
EOF
	name="maclife$$"
	atf_check -s exit:0 -e ignore -o save:c.out \
	    "${bin}" create --name "${name}" "${bundle}"
	cid=$(tr -d ' \t\r\n' < c.out)

	if [ "$(sysctl -n security.mac.biba.enabled 2>/dev/null)" = "1" ]; then
		# Policy loaded: the init process must carry the biba label.
		atf_check -s exit:0 -e ignore -o ignore "${bin}" start "${cid}"
		jname=$(jls -q name | grep '^ocifbsd-' | head -1)
		atf_check -s exit:0 -o match:"biba/high" \
		    sh -c "ps -J ${jname} -o label,command | grep sleep"
	else
		# No policy: start warns it will not be enforced, still runs.
		atf_check -s exit:0 -o match:"will not be enforced" \
		    sh -c "${bin} start ${cid} 2>&1"
	fi
	atf_check -s exit:0 -e ignore -o ignore "${bin}" delete --force "${cid}"
}
mac_label_applied_or_warned_cleanup()
{
	local j
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
	atf_add_test_case rctl_limits_applied_and_cleaned
	atf_add_test_case vnet_epair_wired_and_cleaned
	atf_add_test_case vnet_bridge_attach
	atf_add_test_case mac_label_applied_or_warned
}

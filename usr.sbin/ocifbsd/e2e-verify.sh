#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
#
# e2e-verify.sh — full end-to-end verification of the ocifbsd runtime on a
# live FreeBSD host. Exercises the container lifecycle, the hostname default,
# state-liveness reconciliation, network configuration (VNET isolation and
# non-VNET address application), the running-container safety guard, the
# root/admin-group permission model, and on-disk file modes.
#
# Requires root (jails) and the official FreeBSD OCI base image already in
# the store (docker.io/library/freebsd:15.1). Run as: sudo sh e2e-verify.sh
#
# Exits 0 only if every check passes.

set -u

OCI="${OCI:-$(dirname "$0")/ocifbsd}"
IMAGE="${OCI_E2E_IMAGE:-docker.io/library/freebsd:15.1}"
DATA="${OCIFBSD_DATA_DIR:-/var/lib/ocifbsd}"
TESTUSER="${OCI_E2E_USER:-ocitester}"
GROUP="ocifbsd"
IMGDIR="${DATA}/$(echo "$IMAGE" | sed 's/:/\//')"
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "  PASS $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1 [cond: $2]"; fi; }

if [ "$(id -u)" -ne 0 ]; then
	echo "e2e-verify: must run as root" >&2
	exit 2
fi
if [ ! -x "$OCI" ]; then
	echo "e2e-verify: ocifbsd binary not found at $OCI" >&2
	exit 2
fi
if [ ! -d "$IMGDIR/rootfs" ]; then
	echo "e2e-verify: base image not in store ($IMGDIR); pull/load it first" >&2
	exit 2
fi

echo "== ocifbsd end-to-end verification =="

# ---- 0. build sanity / version -------------------------------------------
"$OCI" --version >/dev/null 2>&1
check "ocifbsd --version succeeds" '"$OCI" --version >/dev/null 2>&1'

# Patch the image init to a long-lived loop so containers stay running long
# enough to exec into; restore on exit no matter how we leave.
CFG="$IMGDIR/config.json"
cp "$CFG" "/tmp/e2e-cfg-backup.$$"
# Rewrite the image's process args to a long-lived init so containers stay up
# long enough to exec into. Portable (sed, no python) so this runs on a base
# FreeBSD install; it replaces whatever single "args" array `ocifbsd load`
# produced with a sleep loop.
sed -i '' -E \
    's#("args": )\[[^]]*\]#\1[ "/bin/sh", "-c", "while true; do sleep 3600; done" ]#' \
    "$CFG"

cleanup() {
	[ -n "${CID:-}" ] && "$OCI" delete --force "$CID" >/dev/null 2>&1
	[ -n "${RID:-}" ] && "$OCI" delete --force "$RID" >/dev/null 2>&1
	cp "/tmp/e2e-cfg-backup.$$" "$CFG" 2>/dev/null
	rm -f "/tmp/e2e-cfg-backup.$$"
	pw groupmod "$GROUP" -d "$TESTUSER" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ---- 1. lifecycle + hostname default -------------------------------------
echo "-- lifecycle & hostname"
CID=$("$OCI" run --name e2e-web --image "$IMAGE" 2>/dev/null)
check "run returns a 64-hex id" 'echo "$CID" | grep -Eq "^[0-9a-f]{64}$"'
check "container reports running" '"$OCI" state "$CID" 2>/dev/null | grep -q running'
HN=$("$OCI" exec "$CID" /bin/hostname 2>/dev/null)
check "hostname defaults to the name (e2e-web)" '[ "$HN" = "e2e-web" ]'
check "exec sees the shared host kernel" '"$OCI" exec "$CID" /usr/bin/uname -s 2>/dev/null | grep -q FreeBSD'

# ---- 2. state-liveness reconciliation ------------------------------------
echo "-- state reconciliation"
JID=$(jls -v 2>/dev/null | awk -v n="ocifbsd-$(echo "$CID" | cut -c1-12)" '$0 ~ n {print $1}' | head -1)
if [ -n "$JID" ]; then
	jail -r "$JID" >/dev/null 2>&1   # kill the jail behind ocifbsd's back
	check "a container whose jail vanished reports stopped" \
	    '"$OCI" state "$CID" 2>/dev/null | grep -q stopped'
else
	bad "could not find jid to test reconciliation"
fi
"$OCI" delete --force "$CID" >/dev/null 2>&1

# ---- 3. network config: VNET isolation applied ---------------------------
echo "-- network config (VNET)"
CID=$("$OCI" create --name e2e-vnet --image "$IMAGE" 2>/dev/null)
check "network set --vnet on applies to the created container" \
    '"$OCI" network set e2e-vnet --vnet on 2>&1 | grep -q "applied to jail"'
"$OCI" start "$CID" >/dev/null 2>&1
IFACES=$("$OCI" exec "$CID" /sbin/ifconfig -l 2>/dev/null)
check "VNET container sees only its own lo0 (isolated stack)" '[ "$IFACES" = "lo0" ]'
check "host, by contrast, has more than lo0" '[ "$(ifconfig -l | wc -w)" -gt 1 ]'
"$OCI" delete --force "$CID" >/dev/null 2>&1

# ---- 4. network config: non-VNET address applied as a jail param ---------
echo "-- network config (non-VNET address)"
CID=$("$OCI" create --name e2e-ip --image "$IMAGE" 2>/dev/null)
# 127.0.0.1 is always present on the host loopback, so restricting a
# non-VNET jail to it is safe and does not touch real host addresses.
# Two loopback addresses: verifies both survive as one ip4.addr array param
# (a repeated option name would keep only the last).
"$OCI" network set e2e-ip --ip4 127.0.0.1/8 --ip4 127.0.0.2/8 >/dev/null 2>&1
"$OCI" start "$CID" >/dev/null 2>&1
JNAME="ocifbsd-$(echo "$CID" | cut -c1-12)"
IP=$(jls -j "$JNAME" ip4.addr 2>/dev/null)
check "non-VNET jail carries the first configured ip4 address" \
    'echo "$IP" | grep -q 127.0.0.1'
check "non-VNET jail carries BOTH configured ip4 addresses" \
    'echo "$IP" | grep -q 127.0.0.2'
"$OCI" delete --force "$CID" >/dev/null 2>&1

# ---- 5. running-container guard ------------------------------------------
echo "-- running-container guard"
RID=$("$OCI" run --name e2e-run --image "$IMAGE" 2>/dev/null)
OUT=$("$OCI" network set e2e-run --dns 9.9.9.9 2>&1)
check "modifying a running container defers to a restart" \
    'echo "$OUT" | grep -qi "restart the container"'
check "the running container is not disrupted" '"$OCI" state "$RID" 2>/dev/null | grep -q running'
"$OCI" delete --force "$RID" >/dev/null 2>&1; RID=

# ---- 6. permission model -------------------------------------------------
echo "-- permissions (unprivileged $TESTUSER)"
CID=$("$OCI" create --name e2e-perm --image "$IMAGE" 2>/dev/null)
"$OCI" network set e2e-perm --vnet off >/dev/null 2>&1

# Ensure the test user is NOT in the admin group first.
pw groupmod "$GROUP" -d "$TESTUSER" 2>/dev/null || true
check "non-group user is DENIED network list" \
    '! su -m "$TESTUSER" -c "$OCI network list e2e-perm" >/dev/null 2>&1'
check "non-group user is DENIED network set" \
    '! su -m "$TESTUSER" -c "$OCI network set e2e-perm --vnet on" >/dev/null 2>&1'
check "non-group user cannot read the raw netcfg file" \
    '! su -m "$TESTUSER" -c "cat $DATA/networks/$CID.json" >/dev/null 2>&1'

# Now add them to the admin group and confirm access is granted.
pw groupmod "$GROUP" -m "$TESTUSER" 2>/dev/null
check "admin-group member is ALLOWED network list" \
    'su -m "$TESTUSER" -c "$OCI network list e2e-perm" >/dev/null 2>&1'
pw groupmod "$GROUP" -d "$TESTUSER" 2>/dev/null

# ---- 7. on-disk modes ----------------------------------------------------
echo "-- file modes"
check "state dir is 0750" '[ "$(stat -f %Lp /var/run/ocifbsd)" = "750" ]'
check "netcfg file is 0640" '[ "$(stat -f %Lp "$DATA/networks/$CID.json")" = "640" ]'
"$OCI" delete --force "$CID" >/dev/null 2>&1; CID=

echo
echo "== summary: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]

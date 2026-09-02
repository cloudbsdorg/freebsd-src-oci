#!/bin/sh
#
# ocifbsd-supervise — container restart policy for ocifbsd.
#
# ocifbsd's own lifecycle has no restart verb and cannot restart a stopped
# container (start works only from 'created', and delete wipes the per-name
# netcfg). This supervisor implements a restart *policy* on top of that: it
# reads a manifest of containers that should be running and, on boot and every
# INTERVAL seconds, (re)creates and starts any that are not — reapplying the
# stored VNET address each time. It is the piece that makes a crashed jail come
# back and makes the workload survive a node reboot.
#
# Manifest lines (whitespace-separated), '#' comments and blanks ignored:
#     <name> <image> <ip4cidr> [policy]
# policy: always (default) | on-failure | no
#
CONF=${OCIFBSD_RESTART_CONF:-/usr/local/etc/ocifbsd/restart.conf}
INTERVAL=${OCIFBSD_SUPERVISE_INTERVAL:-5}
GW=${OCIFBSD_POD_GW:-10.88.0.1}
BRIDGE=${OCIFBSD_POD_BRIDGE:-ocifbsdpodnet}
DNS=${OCIFBSD_POD_DNS:-1.1.1.1}

reconcile_one() {
	name=$1 image=$2 ip=$3 policy=${4:-always}

	line=$(ocifbsd list 2>/dev/null | awk -v n="$name" '$2==n {print $1"|"$NF; exit}')
	id=${line%%|*}
	state=${line##*|}
	[ "$state" = "running" ] && return 0	# healthy — leave it alone

	case "$policy" in
	no)
		return 0
		;;
	on-failure)
		# only restart if the last run exited non-zero
		ec=$(ocifbsd inspect "$id" 2>/dev/null |
		    grep -o '"exit_code"[^,]*' | grep -oE '[-0-9]+$')
		[ "${ec:-0}" = "0" ] && return 0
		;;
	esac

	# (re)create + start with the correct pod-network address. delete first
	# because a stopped/failed container cannot be restarted in place, and
	# because setting netcfg needs a freshly created container.
	[ -n "$id" ] && ocifbsd delete --force "$id" >/dev/null 2>&1
	ocifbsd create --name "$name" --image "$image" >/dev/null 2>&1 || return 1
	ocifbsd network set "$name" --vnet on --ip4 "$ip" \
	    --gateway4 "$GW" --bridge "$BRIDGE" --dns "$DNS" >/dev/null 2>&1
	ocifbsd start "$name" >/dev/null 2>&1
	logger -t ocifbsd-supervise "restart-policy=$policy: (re)started $name ($image @ $ip)"
}

ensure_fabric() {
	# The pod gateway IP on the bridge is what lets the node route to its
	# containers (and the containers reach their default gateway). It does
	# not survive a reboot — the bridge is recreated when the first
	# container starts, but comes up without the gateway address — so
	# reassign it here every loop (idempotent; only when missing).
	ifconfig "$BRIDGE" >/dev/null 2>&1 || return 0
	ifconfig "$BRIDGE" inet 2>/dev/null | grep -q "inet ${GW} " ||
	    ifconfig "$BRIDGE" inet "${GW}/24" alias >/dev/null 2>&1
}

while :; do
	if [ -f "$CONF" ]; then
		while read -r name image ip policy _rest; do
			case "$name" in ''|\#*) continue ;; esac
			reconcile_one "$name" "$image" "$ip" "$policy"
		done < "$CONF"
		ensure_fabric
	fi
	sleep "$INTERVAL"
done

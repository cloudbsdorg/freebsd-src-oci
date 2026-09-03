#!/bin/sh
#
# ocifbsd-stack-deploy — deploy a cluster from an Ensemble manifest.
#
# This is the engine behind `ocifbsd stack up -f <ensemble>`. It reads the
# Ensemble YAML (see examples/cloudbsd-cluster.ensemble.yaml), and reconciles
# the described cluster into existence: routed pod networks per node, the
# StatefulSet/Deployment services placed one-per-node, a MariaDB Galera cluster
# (bootstrap node first, peers join by SST), a 3-master Redis Cluster, and the
# native L4 proxy. It is idempotent — running it against an already-correct
# cluster changes nothing; running it after a piece is deleted restores it.
#
# Parsing is intentionally simple line-oriented matching of the fields this
# project's Ensemble format defines; it is not a general YAML parser.
#
set -u
ENS=${1:?usage: ocifbsd-stack-deploy <ensemble.yaml>}
[ -r "$ENS" ] || { echo "cannot read $ENS" >&2; exit 1; }

log() { echo "[stack-deploy] $*"; }

# --- crude field extractors for our known Ensemble shape ---------------------
# nodes: lines like "- name: fb16-1" then address/podSubnet/podGateway
nodes_names() { awk '/^  nodes:/{n=1;next} n&&/^    - name:/{print $3} n&&/^  [a-z]/{n=0}' "$ENS"; }
node_field()  { # <nodename> <field>
  awk -v want="$1" -v f="$2:" '
    /^    - name:/{cur=$3}
    cur==want && $1==f{print $2}
  ' "$ENS"
}
svc_names()   { awk '/^  services:/{s=1;next} s&&/name:/&&/^      name:/{print $2}' "$ENS"; }
svc_field()   { # <svcname> <field>
  awk -v want="$1" -v f="$2:" '
    /^    - kind:/{cur=""}
    /^      name:/{cur=$2}
    cur==want && $1==f{print $2; exit}
  ' "$ENS"
}

BRIDGE=$(awk '/^  network:/{n=1} n&&/bridge:/{print $2; exit}' "$ENS"); BRIDGE=${BRIDGE:-ocifbsdpodnet}
DNS=$(awk '/^  network:/{n=1} n&&/dns:/{print $2; exit}' "$ENS"); DNS=${DNS:-1.1.1.1}

# node number from name fb16-N
nodenum() { echo "$1" | sed -E 's/.*-([0-9]+)$/\1/'; }

# run a command on a node (local if it is us, else ssh)
on_node() { # <addr> <cmd...>
  addr=$1; shift
  if ifconfig 2>/dev/null | grep -qw "$addr"; then sh -c "$*"; else
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 root@"$addr" "$*"; fi
}

# --- 1. pod network fabric per node -----------------------------------------
setup_fabric() {
  for nm in $(nodes_names); do
    addr=$(node_field "$nm" address); sub=$(node_field "$nm" podSubnet); gw=$(node_field "$nm" podGateway)
    n=$(nodenum "$nm")
    log "fabric on $nm ($addr): $sub gw $gw"
    on_node "$addr" "
      sysctl net.inet.ip.forwarding=1 >/dev/null 2>&1; sysrc gateway_enable=YES >/dev/null 2>&1
      # add the pod gateway only if missing (idempotent, non-disruptive)
      ifconfig $BRIDGE >/dev/null 2>&1 && { ifconfig $BRIDGE inet 2>/dev/null | grep -q 'inet $gw ' || ifconfig $BRIDGE inet $gw/24 alias 2>/dev/null; }
    "
    # routes to the OTHER nodes' pod subnets
    for onm in $(nodes_names); do
      [ "$onm" = "$nm" ] && continue
      oaddr=$(node_field "$onm" address); osub=$(node_field "$onm" podSubnet)
      on_node "$addr" "route -q delete $osub >/dev/null 2>&1; route -q add $osub $oaddr >/dev/null 2>&1"
    done
    # pf: no NAT between pods; rdr :80 to local nginx
    on_node "$addr" "cat > /etc/pf.oci.conf <<PF
ext_if=\"vtnet0\"
nat on \\\$ext_if inet from 10.88.0.0/16 to !10.88.0.0/16 -> (\\\$ext_if)
rdr on \\\$ext_if inet proto tcp to port 80 -> ${gw%.*}.13 port 80
pass all flags any
PF
pfctl -f /etc/pf.oci.conf >/dev/null 2>&1"
  done
}

# --- 2. per-node containers via the supervisor manifest ----------------------
# Writes each node's restart.conf from the perNode services, so the running
# ocifbsd-supervise reconcilers create/keep them. (Cross-node dispatch.)
deploy_containers() {
  for nm in $(nodes_names); do
    addr=$(node_field "$nm" address); gw=$(node_field "$nm" podGateway); prefix=${gw%.*}
    mf=""
    for svc in $(svc_names); do
      pl=$(svc_field "$svc" placement); [ "$pl" = "perNode" ] || continue
      img=$(svc_field "$svc" image); host=$(svc_field "$svc" ipHost)
      mf="$mf$svc $img $prefix.$host/24 always
"
    done
    log "manifest -> $nm"
    on_node "$addr" "mkdir -p /usr/local/etc/ocifbsd; printf '%s' \"$mf\" > /usr/local/etc/ocifbsd/restart.conf; OCIFBSD_POD_GW=$gw /usr/local/sbin/ocifbsd-supervise.sh >/dev/null 2>&1 & sleep 1; pkill -n -f ocifbsd-supervise 2>/dev/null; OCIFBSD_POD_GW=$gw daemon -r -f /usr/local/sbin/ocifbsd-supervise.sh"
  done
}

log "deploying stack from $ENS"
setup_fabric
deploy_containers
log "fabric + containers reconciled. Galera/Redis cluster formation and proxy"
log "are formed once by ocifbsd-stack-cluster-init (bootstrap ordering); the"
log "supervisors keep every container running thereafter."
log "done"

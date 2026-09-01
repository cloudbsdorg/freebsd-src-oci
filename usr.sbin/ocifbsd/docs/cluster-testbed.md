# ocifbsd cluster testbed

A virtualized FreeBSD 16 testbed for exercising ocifbsd in both a clustered and
a standalone configuration. It runs on the FreeBSD host under `vm-bhyve`.

> Addresses below use the RFC 5737 documentation range; the real lab uses a
> private LAN. Substitute your own static addresses.

## Topology

| Role | Nodes | Example addresses |
|------|-------|-------------------|
| **Cluster** | 6 × FreeBSD 16 (`freebsd-16-1` … `freebsd-16-6`) | `192.0.2.11` … `192.0.2.16` |
| **Standalone** | 1 × FreeBSD 16 (`oci-e2e`) | `192.0.2.10` |

Six nodes give a Raft cluster that tolerates two simultaneous node failures
(majority of 6 is 4). The standalone node exercises the single-host paths with
no cluster attached.

## How the VMs are built

Each guest is a `vm-bhyve` clone of a known-good FreeBSD 16 node, using the
**bhyveload** loader (real serial console over `nmdm`). The LAN has no DHCP, so
every clone is given a unique **static** address offline before first boot:

```sh
vm stop freebsd-16-1                 # clone source must be stopped
vm clone freebsd-16-1 freebsd-16-4
vm start freebsd-16-1

# offline-edit the clone's disk: unique IP, hostname, fresh SSH host keys
md=$(mdconfig -f /usr/local/vms/freebsd-16-4/disk0)
mount /dev/${md}p4 /mnt
sed -i '' -e 's/freebsd-16-1/freebsd-16-4/g' \
          -e 's/<old-ip>/<new-ip>/g' /mnt/etc/rc.conf
rm -f /mnt/etc/ssh/ssh_host_*
umount /mnt; mdconfig -d -u ${md#md}
vm start freebsd-16-4
```

Nodes are reached over SSH as `root` using the keys in `~/.ssh` (the guest
image already carries the matching public key).

## Offline PKI (self-signed, no internet)

The control plane authenticates nodes with an internally-blessed CA
(`clustering/cluster_pki.c`) that needs no internet. Verified on FreeBSD 16:
the CA issues an mTLS identity (serverAuth + clientAuth) for each of the six
nodes, every certificate chains to the cluster CA, `openssl verify` accepts
them independently, and a certificate from a different CA is rejected.

```sh
cluster_pki_init_ca("/var/db/ocifbsd/pki", "my-cluster");
cluster_pki_issue_node("/var/db/ocifbsd/pki", "freebsd-16-1");
# ... one per node; each <node>.crt chains to ca.crt
```

## Status

- **Provisioned and reachable:** 6 cluster nodes + 1 standalone.
- **Build:** each node builds `ocifbsd` self-contained (no ports; `ldd` shows
  no `libcurl`/`libjson-c`).

Distributed-services stages, each built red-green and validated on this
testbed:

| Stage | What | Live validation |
|-------|------|-----------------|
| Placement | scheduler spread + cordon + cluster sync | unit |
| Load balancer | `pf` round-robin/source-hash pools | ruleset validated with `pfctl -n` |
| Raft-backed control state | replicated desired state (services/placements) | **3-node cluster: propose → replicate → apply, all converge** |
| Offline internal CA | self-signed cluster PKI, no internet | CA + 6 node mTLS identities issued/verified |
| mTLS control channel | mutual auth over the cluster CA | **cross-host handshake between two nodes** |
| Node agent | assignment protocol + reconcile + apply | **assignment over mTLS → correct actions; agent LAUNCH/STOP creates and removes real containers** |

### End-to-end distributed deploy — validated

The stages are unified into one automatic daemon flow. Running
`ocifbsd-cluster --controller --agent <ocifbsd>` on each node, a single
`CREATE web 3` proposed on the leader flows end to end with no manual steps:

1. Raft replicates the desired state to every node.
2. The leader's controller plans placements and proposes them; Raft replicates
   the placements too (so every node already holds its own slice — the deploy
   loop needs no separate push).
3. Each node's agent reconciles its slice and launches its replica via the
   local runtime.

**Result on the 3-node cluster:** `web-0`, `web-1`, `web-2` each land on a
different node — three real containers, one per node, spread across the
cluster, entirely automatically. The mTLS channel remains available for
out-of-band control (remote exec/logs), not needed for the core deploy loop.

- **Load balancer endpoints:** validated live -- a 3-node `CREATE web 3` has the
  leader auto-derive 3 backend endpoints (one per replica at its node address),
  replicated to every node (`endpoints=3`), and `controller_lb_ruleset()` turns
  them into a `pfctl -n`-valid round-robin pool.
- **Remaining:** assign a service VIP and apply the ruleset via pfctl, and measure image-pull fan-out to decide whether our own
  BitTorrent-style P2P layer distribution is warranted.

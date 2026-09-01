# Restricting an ocifbsd container

ocifbsd follows an **open-by-default** model: a bundle that asks for nothing
gets the permissive, working behavior, and every restriction below is
**opt-in** through a field in the OCI `config.json`. Nothing here changes unless
you add it, so existing bundles keep running unchanged.

Each section shows the `config.json` fragment that turns the restriction on and
notes how ocifbsd enforces it on FreeBSD.

## Resource limits (works today)

OCI `process.rlimits` are applied to the container's init process with
`setrlimit(2)` — the direct FreeBSD equivalent of the POSIX process limits OCI
describes. No kernel RACCT/RCTL is required.

```json
{
  "process": {
    "args": ["/usr/bin/myapp"],
    "rlimits": [
      { "type": "RLIMIT_NOFILE", "soft": 1024, "hard": 1024 },
      { "type": "RLIMIT_NPROC",  "soft": 128,  "hard": 256 },
      { "type": "RLIMIT_AS",     "soft": 536870912, "hard": 536870912 }
    ]
  },
  "root": { "path": "rootfs" }
}
```

Recognized types map to FreeBSD resources: `RLIMIT_CPU`, `RLIMIT_FSIZE`,
`RLIMIT_DATA`, `RLIMIT_STACK`, `RLIMIT_CORE`, `RLIMIT_RSS`, `RLIMIT_MEMLOCK`,
`RLIMIT_NPROC`, `RLIMIT_NOFILE`, and `RLIMIT_AS` (alias `RLIMIT_VMEM`). An
unrecognized type is skipped with a warning rather than failing the container —
open by default. Verified: setting `RLIMIT_NOFILE` to 64 makes `ulimit -n`
report 64 inside the container.

For **jail-wide** accounting limits (as opposed to per-process rlimits), use the
FreeBSD extension `freebsd.rctl_rules`, which maps to `rctl(4)` and requires
`kern.racct.enable=1` in the host kernel.

## No new privileges (parsed; enforcement in progress)

`process.noNewPrivileges` is parsed. The intended FreeBSD enforcement is to
present the container root `nosuid` so set-user-ID / set-group-ID binaries
cannot raise privilege.

```json
{ "process": { "args": ["/usr/bin/myapp"], "noNewPrivileges": true },
  "root": { "path": "rootfs" } }
```

Status: FreeBSD `nullfs` refuses to mount a path over itself ("Resource deadlock
avoided"), so a self-overlay does not work. The correct mechanism is to apply
`nosuid` on the mount that *establishes* the container root (mounting the rootfs
source onto a distinct jail-path mountpoint). That rootfs-mount change is the
tracked follow-up; until it lands, `noNewPrivileges` is recorded but not
enforced.

## Read-only paths / read-only root (parsed; enforcement in progress)

`root.readonly` and `linux.readonlyPaths` are parsed.

```json
{
  "process": { "args": ["/usr/bin/myapp"] },
  "root": { "path": "rootfs", "readonly": true },
  "linux": { "readonlyPaths": ["/etc", "/usr/local/etc"] }
}
```

Status: same FreeBSD `nullfs` self-overlay limitation as above. Making an
existing subtree read-only in place has no direct `nullfs` equivalent; the
read-only root is enforced by mounting the rootfs read-only at establishment
(the same rootfs-mount follow-up). Read-only *subpaths* need a distinct
read-only source and are tracked with it.

## Masked paths (works via empty overlay)

`linux.maskedPaths` hides a directory's contents by overlaying an empty,
read-only directory. Because the overlay source differs from the target, this
is a normal `nullfs` mount and works today.

```json
{
  "process": { "args": ["/usr/bin/myapp"] },
  "root": { "path": "rootfs" },
  "linux": { "maskedPaths": ["/var/secret"] }
}
```

Only existing directories are masked; a masked path that does not exist in the
rootfs is skipped.

## Network isolation (works today, via the FreeBSD extension)

Not an OCI security-context field, but the most effective isolation on FreeBSD:
run the container in its own VNET so it has an independent network stack.

```json
{ "process": { "args": ["/usr/bin/myapp"] },
  "root": { "path": "rootfs" },
  "freebsd": { "vnet": true, "ip4": ["192.0.2.10"] } }
```

## Cluster authentication (works today, opt-in)

The gossip/Raft control channel authenticates every datagram with HMAC-SHA256
when a shared cluster key is configured. Without a key the cluster runs
unauthenticated (open default) and warns at startup — set a key so a host that
can merely reach the port cannot inject Raft commands (which schedule and run
containers) into the cluster.

Prefer a key file (not visible in `ps(1)` or the environment); all nodes must
share the same key:

```sh
# once, on a trusted host — generate a random key and distribute it
openssl rand -hex 32 > /etc/ocifbsd/cluster.key
chmod 600 /etc/ocifbsd/cluster.key

# start each node pointing at the shared key
OCIFBSD_CLUSTER_KEYFILE=/etc/ocifbsd/cluster.key ocifbsd-cluster -n node1 run
```

`OCIFBSD_CLUSTER_KEY=<secret>` is also honored as a fallback. Nodes with
mismatched keys cannot exchange messages, so the whole cluster must use one key.

## Summary

| Restriction | Field | FreeBSD status |
|-------------|-------|----------------|
| Resource limits | `process.rlimits` | Enforced (`setrlimit`) |
| Jail-wide limits | `freebsd.rctl_rules` | Enforced (`rctl`, needs RACCT) |
| Masked paths | `linux.maskedPaths` | Enforced (empty overlay) |
| Network isolation | `freebsd.vnet` | Enforced (VNET) |
| No new privileges | `process.noNewPrivileges` | Parsed; needs rootfs-mount mode |
| Read-only root/paths | `root.readonly`, `linux.readonlyPaths` | Parsed; needs rootfs-mount mode |

Leave a field out and that restriction is simply off — the default is open.

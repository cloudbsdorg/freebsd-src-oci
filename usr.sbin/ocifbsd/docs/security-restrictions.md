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

## No new privileges (works today)

`process.noNewPrivileges` is enforced: ocifbsd establishes the container root as
a `nosuid` nullfs mount, so set-user-ID / set-group-ID binaries in the image
cannot raise privilege.

```json
{ "process": { "args": ["/usr/bin/myapp"], "noNewPrivileges": true },
  "root": { "path": "rootfs" } }
```

## Read-only root (works today) / read-only paths (parsed)

`root.readonly` is enforced: the container root is established as a **read-only**
nullfs mount (a distinct `$STATE_DIR/<id>.jailroot` mountpoint used as the jail
root — FreeBSD nullfs cannot overlay a path on itself), so writes anywhere in the
root fail with `EROFS`. Verified: a container with `root.readonly` runs and a
write inside returns "Read-only file system".

```json
{
  "process": { "args": ["/usr/bin/myapp"] },
  "root": { "path": "rootfs", "readonly": true },
  "linux": { "readonlyPaths": ["/etc"] }
}
```

`root.readonly` and `noNewPrivileges` may be combined (the root is then mounted
`ro,nosuid`). Per-path `linux.readonlyPaths` (making one existing subtree
read-only while the rest stays writable) is still parsed-only — it needs a
distinct read-only source per path and is tracked separately.

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
| Jail-wide limits (OCI) | `linux.resources` | Enforced (`rctl`, needs `kern.racct.enable=1`; else warns) |
| Jail-wide limits (native) | `freebsd.rctl_rules` | Enforced (`rctl`, needs RACCT) |
| MAC label | `freebsd.macLabel` | Enforced (`mac_set_proc` on init, needs a loaded labeling policy; else warns) |
| Network isolation | `freebsd.vnet` | Enforced (VNET: epair moved into the jail, IP configured, torn down on delete) |
| External reachability | `freebsd.bridge` | Enforced (host epair added to the named `if_bridge(4)`) |
| No new privileges | `process.noNewPrivileges` | Enforced (nosuid nullfs root) |
| Read-only root | `root.readonly` | Enforced (read-only nullfs root) |
| Read-only sub-paths | `linux.readonlyPaths` | Parsed only — warns as unenforced (per-path overlay pending) |
| Masked paths | `linux.maskedPaths` | Parsed only — warns as unenforced |

Leave a field out and that restriction is simply off — the default is open.

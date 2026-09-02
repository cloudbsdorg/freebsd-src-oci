---
name: oci-failover-test
description: Safely simulate a node/system failure in the ocifbsd cluster by powering the bhyve VM off and on, instead of stopping individual containers.
---

# Simulating a node failure in the ocifbsd cluster

When you want to prove failover, self-healing, or proxy short-circuit behavior by
taking a cluster node "down," **power off the whole bhyve VM** — do not stop or
delete the ocifbsd containers inside it.

## Why: the container lifecycle is a trap

ocifbsd's container lifecycle does **not** round-trip cleanly:

- `ocifbsd start` only works on a container in the **`created`** state. A
  **`stopped`** container cannot be started again — there is **no `restart`
  verb**.
- To bring a stopped container back you must `delete` it and recreate it, and
  **`delete` wipes the per-name network configuration** (VNET, ip4, gateway,
  bridge, dns). The recreated container comes up with **no IP** and the node
  drops out of the cluster.
- Rebuilding then requires the exact original order, because `network set`
  needs the container to already exist but the netcfg is only applied at
  **start**:

  ```sh
  ocifbsd create  --name <svc> --image local/<svc>:latest
  ocifbsd network set <svc> --vnet on --ip4 <ip>/24 --gateway4 10.88.0.1 \
      --bridge ocifbsdpodnet --dns 1.1.1.1
  ocifbsd start   <svc>
  ```

  (`run` = create+start, so it starts **before** you can set netcfg — the IP is
  lost. Never use `run` when the container needs a fixed pod IP.)

Stopping containers to fake a failure therefore turns a 10-second test into a
fragile manual rebuild.

## Do this instead

Power the VM off (a real "system down") and back on — clean, and reversible in
one command:

```sh
# take the node down
vm stop  freebsd-16-3          # or: sudo vm stop freebsd-16-3
# ... observe: proxy failover, cluster map marks it down, self-healing ...
# bring it back
vm start freebsd-16-3
```

The cluster VMs run under vm-bhyve on **freedev007** (`freebsd-16-1..6`
→ 192.168.1.241..). `fb16-1` (.241) holds the shared **Redis + MariaDB**, so
for a survivable web-tier test take down `fb16-2` or `fb16-3`, never `fb16-1`.

## Known cluster-node netcfg (podnet 10.88.0.0/24, bridge `ocifbsdpodnet`)

| container | ip4          | gateway4   | dns     |
|-----------|--------------|------------|---------|
| wordpress | 10.88.0.12/24| 10.88.0.1  | 1.1.1.1 |
| nginx     | 10.88.0.13/24| 10.88.0.1  | 1.1.1.1 |

Use the create → network set → start sequence above only as a **recovery**
procedure if containers were already stopped/deleted — not as a test method.

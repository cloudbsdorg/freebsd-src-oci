<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# ocifbsd vs podman — container lifecycle benchmark

Task 5.7. A same-host comparison of container lifecycle latency between
`ocifbsd` and `podman` on FreeBSD, so the numbers reflect tooling overhead
rather than differences in hardware or OS.

## Method

- **Host:** one FreeBSD 15.1-STABLE machine ran both runtimes (the `ocifbsd`
  binary, though built on FreeBSD 16, runs unmodified on 15.1). Running both on
  the same host is what makes the comparison fair.
- **podman:** 5.x with the `ocijail` 0.6.0 OCI runtime and the ZFS graph
  driver — i.e. podman also backs containers with FreeBSD jails, so this
  measures the tooling around the same kernel primitive.
- **Workload:** a minimal image whose rootfs is a single static `/rescue`
  binary; the process is `sleep`. Networking is disabled on both sides
  (`podman --network none`; `ocifbsd` with no VNET attachment) so the numbers
  isolate the create/start/stop/delete path, not bridge/epair setup.
- **Timing:** `CLOCK_MONOTONIC` in milliseconds around each phase, 20 full
  lifecycles. ocifbsd was driven by `tools/ocifbsd-perf.sh`; podman by an
  equivalent create/start/stop/rm loop.

## Results (p50, milliseconds, n=20)

| Phase            | ocifbsd | podman |
|------------------|--------:|-------:|
| create           | **3**   | 58     |
| start            | 103     | **34** |
| stop / kill      | **2**   | 51     |
| delete / rm      | **2**   | 34     |
| **end-to-end**   | **~113**| ~177   |

## Reading the numbers

- **End-to-end, ocifbsd is ~1.6× faster** (≈113 ms vs ≈177 ms for the full
  create→start→stop→delete cycle).
- **Phase distribution differs.** ocifbsd does almost nothing at `create`
  (3 ms — it writes state and defers the work) and pays the jail bring-up at
  `start` (103 ms). podman front-loads `create` (58 ms: image graph, config,
  CNI/network scaffolding) and has a lighter `start` (34 ms). Summed across the
  lifecycle, ocifbsd's leaner path wins.
- **Teardown is much cheaper on ocifbsd** (kill 2 ms + delete 2 ms vs stop
  51 ms + rm 34 ms), because it does not maintain podman's container graph and
  storage-layer bookkeeping.
- The dominant cost on both sides is the jail bring-up itself, which is a
  kernel operation neither runtime can avoid; the difference is entirely in the
  surrounding tooling, where ocifbsd's native, minimal design shows.

## Caveats

Micro-benchmark: single host, warm caches, no image pull in the timed loop, no
networking. It measures lifecycle latency, not throughput, memory, or
steady-state behavior. The point is order-of-magnitude tooling overhead, and on
that axis `ocifbsd` is competitive with — and end-to-end faster than — podman
for the same jail-backed container on FreeBSD.

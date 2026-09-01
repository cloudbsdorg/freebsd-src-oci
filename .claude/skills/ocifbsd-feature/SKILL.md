---
name: ocifbsd-feature
description: >-
  Build (or extend) an ocifbsd/CloudBSD feature end-to-end to this project's
  definition of done. Invoke whenever implementing anything in usr.sbin/ocifbsd
  — a new module, a bug fix, a distributed-services stage, an optimization —
  or when the user says "make sure all tasks are complete", "keep going",
  "red green", "full e2e", or asks to continue the .plan work. Drives the
  standing loop: red-green tests, self-contained build, full kyua suite, live
  validation on the FreeBSD 16 vm-bhyve cluster, stress + optimization, docs,
  website, and a REVYTECH-attributed commit — then repeats until the stack is
  empty.
---

# ocifbsd-feature — implement to the project's definition of done

<!-- Copyright (c) 2026 REVYTECH, Inc. -->

This skill encodes how features are finished on the `feature/oci-bootstrap`
branch. "Done" is broader than "it compiles": follow every step below, in order,
for each unit of work, and don't stop until the requested stack is empty.

## 0. Ground rules (always)

- **Attribution:** every file created carries `Copyright (c) 2026 REVYTECH, Inc.`
  and `This software was developed by REVYTECH, Inc.` Never introduce FreeBSD
  Foundation / Klara / CloudBSD as a copyright holder; if you find that
  boilerplate in an ocifbsd-authored file, replace it with REVYTECH. Vendored
  `contrib/` (json-c, libcurl) keeps its upstream license untouched.
- **No external product branding:** do not name features after Kubernetes/k8s or
  Docker. The config-conversion format is **Ensemble**. Keep only interop
  constants (the `docker.io` registry hostnames, OCI/Docker media types).
- **Self-contained:** the tree builds with **zero ports**. Never add a port
  dependency or a `-I/usr/local`/`-L/usr/local`/`-lcurl`/`-ljson-c` flag. New
  base libraries go through `LIBADD` (raw `-lssl`/`-lcrypto` are rejected by the
  src build).
- **Redaction:** anything that could be published (repo docs, website, man
  pages) uses RFC 5737 documentation IPs (`192.0.2.0/24`, `198.51.100.0/24`,
  `203.0.113.0/24`), never the real lab LAN.
- **JSON a human reads is pretty-printed by default** (compact only for
  JSONL/streaming, behind an escape hatch).
- Commit messages end with the two standard trailers (Co-Authored-By +
  Claude-Session). Branch off `main` is already done — commit to
  `feature/oci-bootstrap` and push.

## 1. Red-green (test first)

Write the failing test **before** the implementation and watch it fail:

- C unit tests are ATF and `#include` the module `.c` directly (see
  `registries_test.c`, `cluster_pki_test.c`). Register each in **both**
  `tests/usr.sbin/ocifbsd/Makefile` `ATF_TESTS_C` and the checked-in
  `Kyuafile`. Per-test `LIBADD.<name>+=`/`CFLAGS.<name>+=` go **before**
  `.include <bsd.test.mk>`.
- Build just the new test → confirm RED (a link error for the not-yet-written
  function, or a failing assertion). Then implement → GREEN.
- Cover the happy path, edge cases, rejection of bad input, and — per the
  standing criterion — a **stress case** at scale.

## 2. Build + full suite (host)

```sh
cd usr.sbin/ocifbsd && make            # must be exit 0, WARNS=3 -Werror clean
# self-contained check:
ldd <ocifbsd binary> | grep -icE 'curl|json|/usr/local'   # must be 0
cd ../../tests/usr.sbin/ocifbsd && make && kyua test && kyua report
# require: "N total, ... 0 broken, 0 failed" (4 root-only skips are expected)
```

## 3. Live validation on the FreeBSD 16 cluster

The vm-bhyve testbed is part of the definition of done: a 6-node cluster plus a
standalone (see `usr.sbin/ocifbsd/docs/cluster-testbed.md`). Reach nodes over
SSH as `root` with `~/.ssh` keys. Sync source with `tar | ssh 'cd
/root/ocifbsd-tree/usr.sbin && tar xzf -'` (extract at `usr.sbin`, not inside
`ocifbsd/` — a double-`ocifbsd/` path is the classic mistake).

- Build on a node (self-contained), run `e2e-verify.sh` (expect 19/19), and the
  **live scenario** for this feature (e.g. a 3-node Raft propose→apply, a
  cross-host mTLS handshake, an offline PKI issue+verify). Prefer a small C
  harness that exercises the real functions on the target.
- Need more nodes? Clone a stopped node with `vm clone`, then offline-edit the
  raw disk (`mdconfig -f .../disk0`, mount `p4`, set a unique static IP +
  hostname in `/etc/rc.conf`, `rm /etc/ssh/ssh_host_*`), and boot.

## 4. Optimization + stress

Exercise the feature under load and confirm it stays correct and fast. Do an
optimization pass (algorithms, I/O, data path). For bulk image-layer transfer
across a cluster, P2P (our own, BitTorrent-style over the mTLS channel; layers
are already sha256-addressed) is allowed **only if measured necessary** —
instrument first, don't build it speculatively.

## 5. Code review

Fold in a review: use the local `grok` CLI via the `grok-analyze` skill when
available, plus an independent own-review for correctness, security, and
algorithmic efficiency (weigh Gang-of-Four patterns and Knuth). Verify every
finding against the source before acting.

## 6. Docs + website

- Update README, the relevant man page(s) (`mandoc -T lint` clean), and
  `OCI-STATUS.md`.
- Refresh the website when user-facing behavior changed: regenerate the
  man-page site with `usr.sbin/ocifbsd/docs/mansite/` and, for the landing
  site, the separate `~/git/ocifbsd-www` repo; deploy with its `deploy.sh`
  (nginx docroots `/usr/local/www/oci` and `/usr/local/www/ocifbsd-man`).
  Technical diagrams: hand-authored theme-aware SVG; imagery: the `agy` CLI.

## 7. Commit, push, repeat

Commit each coherent unit with a descriptive body and the standard trailers;
`git push origin feature/oci-bootstrap`. Then take the next item off the stack
and return to step 1. Only stop when the requested work is genuinely complete —
report status plainly, including anything that failed or was skipped.

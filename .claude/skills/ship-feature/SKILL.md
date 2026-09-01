---
name: ship-feature
description: >-
  Build or extend a feature end-to-end to a rigorous "definition of done" in any
  codebase, adapting to that project's conventions. Invoke when implementing a
  feature, fixing a bug, doing an optimization, or continuing an iterative loop —
  e.g. the user says "make sure all tasks are complete", "keep going", "don't
  stop", "red green", "full e2e", "ship it", or asks to keep working through a
  plan/backlog. Drives the loop: discover conventions, red-green tests,
  build + full suite, live/integration validation, stress + optimization,
  review, docs, and a conventions-compliant commit — repeating until the stack
  is empty.
---

# ship-feature — implement to a rigorous definition of done

<!-- Copyright (c) 2026 REVYTECH, Inc. -->

"Done" is broader than "it compiles." Follow these steps, in order, for each
unit of work, and don't stop until the requested stack is empty. The steps are
project-agnostic; **step 0 is how you make them fit the project at hand.**

## 0. Discover and honor the project's conventions (first, every time)

Before writing code, learn how this project does things and match it — do not
impose your own defaults:

- **Build & test commands:** how it builds and how it runs tests (Makefile,
  `cargo`, `npm`, `go test`, `bmake`/kyua, CI config). Find the test framework
  and the "all tests pass" signal.
- **Attribution / license header:** the exact copyright/SPDX header new files
  must carry, and the ownership rule (who is the copyright holder; whether to
  replace or append on edit). Never invent an owner; copy the project's.
- **Coding standards & lint:** formatting, naming, warning level (treat
  `-Werror`/strict lint as a hard gate), and any commit-message conventions or
  required trailers.
- **Dependency policy:** e.g. a "vendored / self-contained / no new runtime
  deps" rule, an approved-license list, or a "no copyleft" constraint. Respect
  it; add dependencies only the project's way.
- **Branding / naming constraints:** products or trademarks the project avoids
  naming its own features after; the neutral names it uses instead. Keep genuine
  interop/protocol identifiers (they are not branding).
- **Redaction:** never publish secrets, internal hostnames, or real IPs in
  anything that ships (docs, website, examples). Use the project's placeholders,
  or standard documentation ranges (RFC 5737 `192.0.2.0/24`,
  `198.51.100.0/24`, `203.0.113.0/24`).
- **Output ergonomics:** honor stated preferences (e.g. human-facing JSON
  pretty-printed by default, compact only for machine/streaming output).
- **Integration environment:** how the project is validated for real — VMs,
  containers, a staging cluster, hardware, a device farm — and how to reach it.

If a convention isn't discoverable and the choice is consequential, ask; other-
wise pick the obvious project-consistent option and note it.

## 1. Red-green (test first)

Write the failing test **before** the implementation and watch it fail (RED —
a link/compile error for the not-yet-written function, or a failing assertion),
then implement until it passes (GREEN). Register the test wherever the project's
harness discovers tests. Cover the happy path, edge cases, rejection of bad
input, and a **stress case at scale**.

## 2. Build + full suite

Build clean at the project's strict warning level, then run the **entire** test
suite (not just the new test). Require zero failures; account for any
legitimately skipped tests (e.g. root-only). If the project has a "self-
contained / no-new-deps" rule, verify it here (e.g. inspect the linked
libraries).

## 3. Live / integration validation

Unit-green is not enough. Exercise the feature in the project's real
environment — the VM/cluster/container/staging target, not just the build host.
Run the project's e2e/integration checks and the **specific live scenario** for
this change (prefer a small harness that drives the real functions/binaries on
the target). Report exactly what ran and its result.

## 4. Stress + optimization

Exercise the feature under load; confirm it stays correct and fast. Do an
optimization pass (algorithms, I/O, the data path). **Optimize with
measurement** — profile or instrument before adding heavy machinery (caches,
custom transports, P2P); don't build it speculatively.

## 5. Code review

Fold in a review before finalizing: an independent own-review for correctness,
security, and algorithmic efficiency, plus any tool/second-model the project
uses (verify every finding against the source before acting on it).

## 6. Docs + user-facing surface

Update whatever the change touches: README, man pages / API docs (lint them),
status/changelog, and any website or UI. Keep examples runnable and redacted.

## 7. Commit, push, repeat

Commit each coherent unit with a descriptive body that explains the *why*, in
the project's message style with any required trailers; push to the working
branch. Then take the next item off the stack and return to step 1. Stop only
when the work is genuinely complete — then report status plainly, including
anything that failed or was skipped.

---

## Appendix: this checkout (ocifbsd / CloudBSD)

Concrete values so the skill is immediately usable here; treat as an example of
step 0 for other projects.

- **Build/test:** `cd usr.sbin/ocifbsd && make` (bmake, `WARNS=3 -Werror`);
  tests `cd tests/usr.sbin/ocifbsd && make && kyua test && kyua report` (expect
  `0 failed`; 4 root-only skips are normal). Self-contained gate:
  `ldd <binary> | grep -icE 'curl|json|/usr/local'` must be `0` (json-c and
  libcurl are vendored under `contrib/`; **zero ports**).
- **Tests:** ATF; C unit tests `#include` the module `.c` directly and are
  registered in **both** `tests/usr.sbin/ocifbsd/Makefile` `ATF_TESTS_C` and the
  checked-in `Kyuafile`; per-test `LIBADD.<t>+=`/`CFLAGS.<t>+=` go **before**
  `.include <bsd.test.mk>`.
- **Attribution:** `Copyright (c) 2026 REVYTECH, Inc.` + `This software was
  developed by REVYTECH, Inc.` Replace any FreeBSD Foundation / Klara / CloudBSD
  ownership in ocifbsd-authored files; leave `contrib/` upstream licenses alone.
- **Branding:** no Kubernetes/k8s or Docker naming for our features (the
  config-conversion format is **Ensemble**); keep interop constants (`docker.io`
  hosts, OCI/Docker media types).
- **Integration:** a `vm-bhyve` testbed of 6 FreeBSD 16 cluster nodes + a
  standalone (see `usr.sbin/ocifbsd/docs/cluster-testbed.md`), reached over SSH
  as `root` with `~/.ssh` keys; sync source via `tar | ssh 'cd
  /root/ocifbsd-tree/usr.sbin && tar xzf -'` (extract at `usr.sbin`). e2e:
  `e2e-verify.sh` (19/19) plus the live scenario.
- **Website:** man-page site under `usr.sbin/ocifbsd/docs/mansite/`; landing
  site in the separate `~/git/ocifbsd-www` repo (`deploy.sh` → nginx docroots
  `/usr/local/www/oci` and `/usr/local/www/ocifbsd-man`). Diagrams: theme-aware
  SVG; imagery: the `agy` CLI.
- **Review:** the local `grok` CLI via the `grok-analyze` skill when available.
- **Commit trailers:** `Co-Authored-By: …` and `Claude-Session: …`; push to
  `feature/oci-bootstrap`.

# ocifbsd — dependencies & licensing

> Audit date: 2026-09-01. Purpose: confirm the build carries **no GPL/GNU
> copyleft** dependencies (BSD-first policy) and record what a self-contained
> release build needs.

## Toolchain

Built with the FreeBSD base **clang/lld** toolchain and **bmake** — all
BSD/permissive. No GNU toolchain requirement.

## Linked libraries

### In FreeBSD base (no external download needed)

| Library | Provides | License |
|---------|----------|---------|
| libjail, libutil, libnv | jails, helpers, name/value | BSD-2 |
| libzfs | ZFS image store | CDDL (base) |
| libcrypto, libssl | TLS, digests (cert/) | Apache-2.0 (base OpenSSL) |
| libmd | SHA-256 (image, clustering) | BSD-2 |
| libarchive | layer/tar extraction (image) | BSD-2 |
| libz | compression | Zlib |
| libm, libpthread (libthr) | math, threads | BSD-2 |
| libpam | PAM module (pam/) | BSD-3 style |

None of these are GPL. CDDL (libzfs) and Apache-2.0 (OpenSSL) are the base
system's own choices and ship with FreeBSD.

### Ports (NOT in base, NOT yet vendored in-repo)

| Library | Used by | License | Notes |
|---------|---------|---------|-------|
| **libcurl** (curl 8.x) | image (registry pull/push), cert/acme, logd/forward, api | **curl license** (MIT-derived, permissive) | needs response-header access (Replay-Nonce/Location) for ACME |
| **json-c** (0.19) | most subsystems (JSON encode/parse) | **MIT** | small, easily vendorable |

Both port dependencies are **permissive (no copyleft)** — so there is no
license conflict with a BSD base. The only issue is *self-containment*: a clean
checkout on a stock FreeBSD needs these two ports installed
(`pkg install curl json-c`) before it will build the networked/JSON subsystems.

## Self-contained release build — plan

To make `make release` build from the repo alone (a success criterion):

1. **json-c (MIT):** vendor into `contrib/` and build in-tree. Small and
   self-contained; the cleanest path.
2. **libcurl:** two options —
   - **libfetch (base):** migrate registry pull/push and logd forwarding to
     base `libfetch` (BSD). ACME is the hard case: it needs Replay-Nonce and
     Location response headers, which libfetch exposes less directly, so ACME
     may keep libcurl or grow a small base-HTTP shim.
   - **vendor libcurl (permissive):** larger tree but zero behavior change.

Until then the two ports are documented build requirements; there is **no GPL
exposure** either way.

## Verification

A cleanroom build (a fresh FreeBSD 16 VM with only the repo + the two
documented ports) is used to confirm the tree builds from scratch; see the
release/cleanroom notes.

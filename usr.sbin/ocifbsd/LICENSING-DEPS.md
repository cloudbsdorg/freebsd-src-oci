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

### Vendored in-tree (no port needed)

| Library | Used by | License | Notes |
|---------|---------|---------|-------|
| **json-c** (0.18) | most subsystems (JSON encode/parse) | **MIT** | vendored under `contrib/json-c` as a private static lib; `ldd` shows no `libjson-c`. Built with json-c's own cmake-generated `config.h` (ENABLE_THREADING off, HAVE_XLOCALE_H on), `-fPIC` (PIE binaries), `-DNDEBUG`. |

### Ports still required to build

| Library | Used by | License | Notes |
|---------|---------|---------|-------|
| **libcurl** (curl 8.x) | image (registry pull/push), cert/acme, logd/forward, api | **curl license** (MIT-derived, permissive) | needs response-header access (Replay-Nonce/Location) for ACME; pulls a large transitive tree (nghttp2, idn2, ssh2, …). Candidate to replace with base `libfetch` or vendor. |

Both are **permissive (no copyleft)** — no license conflict with a BSD base.
json-c is now vendored, so a clean checkout needs only `pkg install curl`
(and that dependency is tracked for removal). Verified self-contained on json-c
on the 15.1 build host and a FreeBSD 16 VM: builds with no json-c port, `ldd`
shows no `libjson-c`, suite 160/160, e2e 19/19.

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

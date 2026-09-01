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

### Vendored in-tree (no port needed) — the tree needs ZERO ports to build

| Library | Used by | License | Notes |
|---------|---------|---------|-------|
| **json-c** (0.18) | most subsystems (JSON encode/parse) | **MIT** | vendored under `contrib/json-c` as a private static lib; `ldd` shows no `libjson-c`. Built with json-c's own cmake-generated `config.h` (ENABLE_THREADING off, HAVE_XLOCALE_H on), `-fPIC` (PIE binaries), `-DNDEBUG`. |
| **libcurl** (curl 8.21.0) | image (registry pull/push), cert/acme, logd/forward | **curl license** (MIT-derived, permissive) | vendored under `contrib/curl` as a private static lib (`libocifbsd_curl.a`); `ldd` shows no `libcurl`. Source is the exact port distfile (SHA256 `aa1b66a7…`). Configured minimally with curl's own `./configure`: HTTP/HTTPS only, TLS via **base** OpenSSL (`--with-openssl=/usr`), base CA bundle (`/etc/ssl/cert.pem`), and every third-party backend disabled (no nghttp2/nghttp3/ngtcp2, idn2/psl/rtmp, brotli/zstd/zlib, libssh2/libssh, gnutls/wolfssl/mbedtls, gssapi, and all non-HTTP protocols). The generated `curl_config.h` is committed under `lib/`. See `curl.inc.mk`. |

Both are **permissive (no copyleft)** — no license conflict with a BSD base,
and there is **no GPL anywhere** in the tree.

**The build now requires no ports at all** (json-c and libcurl were the last
two, both vendored). Verified self-contained on the 15.1 build host and a
FreeBSD 16.0-CURRENT VM: the whole tree builds with no ports, `ldd` on the
resulting `ocifbsd` shows only FreeBSD base libraries (no `libcurl`, no
`libjson-c`, nothing under `/usr/local`) **even when both ports are installed**,
curl's symbols are statically embedded, and the runtime is green — host suite
156/160 (4 root-only tests skipped as non-root), e2e 19/19 on FreeBSD 16, and a
standalone TLS check on FreeBSD 16 completes a full HTTPS handshake with
custom-CA verification (the ACME/registry code path) against base OpenSSL.

## Self-contained release build — DONE

`make release` builds from the repo alone (the success criterion is met):

1. **json-c (MIT):** vendored into `contrib/json-c`, built in-tree. ✅
2. **libcurl (curl license):** vendored into `contrib/curl`, built in-tree.
   The "vendor libcurl (permissive)" option was chosen over migrating to base
   `libfetch` because it preserves battle-tested TLS/HTTP with **zero behavior
   change** — important since the ACME (`cert/acme.c`) and registry
   (`image/pull.c`,`image/push.c`) flows depend on response-header capture
   (Replay-Nonce/Location/WWW-Authenticate) and streaming uploads. ✅

There is **no GPL exposure** and **no port requirement**.

## Verification

A cleanroom-style build was confirmed on a fresh FreeBSD 16.0-CURRENT VM with
**no `/usr/src`** and only the repo: the whole tree builds, the resulting
`ocifbsd` links only base libraries (`ldd` shows no `libcurl`/`libjson-c`),
and e2e-verify passes 19/19. Building without `/usr/src` also surfaced and
fixed a portability bug — five leaf-module Makefiles carried a bare
`.include <src.opts.mk>` that aborts when src.opts.mk is not installed; those
includes were unused and were removed.

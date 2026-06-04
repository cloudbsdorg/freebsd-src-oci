# tools/cross-build/ - Cross-Build Helpers for Non-FreeBSD Hosts

> **TL;DR**: If you are on a FreeBSD host, you don't need this directory.
> Just run `bmake -C usr.sbin/ocifbsd` and you're done. This directory
> exists only for developers on **macOS** or **Linux** hosts who want to
> cross-build to FreeBSD amd64.

## Why this exists

`ocifbsd` is a FreeBSD-native runtime. Its tier-1 build target is FreeBSD
itself. The C code uses FreeBSD-specific syscalls (`jail(2)`, `capsicum(4)`,
`rctl(8)`, `zfs(8)`) and FreeBSD-specific headers (`<sys/jail.h>`,
`<sys/capsicum.h>`, etc.) that do not exist on macOS or Linux.

The FreeBSD source tree provides `tools/build/make.py` — a build orchestrator
that can cross-build the FreeBSD userland (libraries) from a non-FreeBSD host
by pointing `XCC`/`XLD`/`XAS` at a cross-toolchain. We use that to build the
libraries `ocifbsd` depends on (`jail`, `util`, `zfs`, `crypto`, `pthread`,
`m`, `json-c`), then `bmake` to link `ocifbsd` against them.

This is a **cross-build**: the host is macOS/Linux, the target binaries are
FreeBSD amd64. The resulting `ocifbsd` binary will not run on macOS/Linux —
it must be deployed to a FreeBSD host (or VM) to run.

## When to use this

Use the cross-build path if:

- You develop on macOS or Linux and don't want to do all your work inside a
  FreeBSD VM.
- You want to iterate on the C code from your native editor (clangd, LSP)
  on macOS/Linux.
- You're doing a quick syntax/type check before pushing to a CI runner that
  does the full native build.

Do **not** use the cross-build path if:

- You are on a FreeBSD host. Use native `bmake` instead — it's faster,
  simpler, and tests the actual runtime.
- You're preparing a release. Releases must be built natively on FreeBSD.

## What's in this directory

| File                  | Purpose                                                  |
| --------------------- | -------------------------------------------------------- |
| `macos.sh`            | Bootstrap a clean macOS host for cross-build             |
| `README.md`           | This file                                                |
| `linux.sh`            | (TODO) Bootstrap a clean Linux host for cross-build      |

## Quick start (macOS host)

```sh
# 1. Clone the repo
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
git checkout feature/oci-bootstrap

# 2. Bootstrap the toolchain (idempotent)
./tools/cross-build/macos.sh --install --yes

# 3. Source the generated env file
. /tmp/ocifbsd-cross-build-env

# 4. Cross-build
make -C usr.sbin/ocifbsd cross-build
```

The output is a FreeBSD amd64 `ocifbsd` binary at `usr.sbin/ocifbsd/ocifbsd`.
Deploy it to a FreeBSD host to run:

```sh
scp usr.sbin/ocifbsd/ocifbsd freebsd-host:/usr/local/sbin/
```

## What `macos.sh` does

Five steps, each idempotent:

1. **Xcode Command Line Tools** — `xcode-select --install` if missing.
2. **Homebrew** — install from `https://brew.sh` if missing.
3. **bmake** — `brew install bmake` if missing.
4. **LLVM toolchain** — `brew install llvm` (clang, lldb) + `brew install lld`
   (separate package since LLVM 19+).
5. **Python 3** — `brew install python@3.12` if missing (preinstalled on
   macOS 14+).

After verification, it writes a sourceable env file at
`/tmp/ocifbsd-cross-build-env` containing `XCC`, `XLD`, `XAS`, `XAR`, `XNM`,
`XOBJCOPY`, `XRANLIB`, `XSTRINGS`, `XSIZE`, `TARGET`, `TARGET_ARCH`, and
`MAKEOBJDIRPREFIX` — all the variables `tools/build/make.py` needs.

## Where "FreeBSD native" stays primary

Every cross-build helper in this directory explicitly says it's a cross-build
helper, not the primary build path. The Makefile in `usr.sbin/ocifbsd/` lists
the `cross-build` target in a clearly-marked "Cross-build (macOS/Linux hosts)"
section of its `help` output, separated from the native build targets.

If the rest of the organization sees any "darwin", "macos", or "macOS" string
on a primary build surface (default `make`, `make help`, `make info`),
that is a bug and should be reported.

## Reporting issues

If the cross-build breaks:

1. Run `./tools/cross-build/macos.sh --check` and capture the output.
2. Run `make -C usr.sbin/ocifbsd cross-build 2>&1 | tee /tmp/cross-build.log`.
3. File an issue with both outputs attached.

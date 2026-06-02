# Contributing to ocifbsd

Welcome! `ocifbsd` is a native OCI runtime for FreeBSD. This guide
covers the development workflow, make targets, and conventions.

## Quick Start

```bash
# One-command setup (macOS host)
bmake -C usr.sbin/ocifbsd dev

# Or step by step:
bmake -C usr.sbin/ocifbsd check-deps        # verify tools
bmake -C usr.sbin/ocifbsd darwin-bootstrap  # macOS: install toolchain
. /tmp/ocifbsd-cross-build-env              # activate cross-build env
bmake -C usr.sbin/ocifbsd darwin-build      # cross-build

# Verify your changes
bmake -C usr.sbin/ocifbsd verify
```

## Development Workflow

### 1. Branch from main

```bash
git checkout main
git pull
git checkout -b feature/my-change
```

### 2. Make your changes

Follow these conventions:

- **C code:** Match the existing FreeBSD style (see `style(9)`)
- **License headers:** Use traditional BSD-style (see existing files)
- **No AI slop:** Avoid `TODO`, `FIXME`, `XXX`, `HACK` markers
- **No seccomp:** Use `capsicum(4)` instead (FreeBSD-native)
- **No json-c:** Use `libxo` or vendor the dependency

### 3. Verify your changes

```bash
bmake -C usr.sbin/ocifbsd verify
bmake -C usr.sbin/ocifbsd audit
bmake -C usr.sbin/ocifbsd lint
bmake -C usr.sbin/ocifbsd smoke
```

### 4. Commit and push

```bash
git add .
git commit -m "Clear, descriptive message"
git push origin feature/my-change
```

### 5. Open a PR

The CI workflow will run all inspection targets automatically.

## Make Targets Reference

### Inspection (work on macOS or FreeBSD)

| Target | What it does |
|--------|-------------|
| `help` | Show all targets |
| `info` | Build environment (host, target, tools) |
| `audit` | AI slop markers (TODO/FIXME/HACK + hotspots) |
| `lint` | License headers, seccomp, json-c refs |
| `smoke` | Shell/Makefile syntax + license check |
| `sources` | File inventory with LOC |
| `size` | Tree size breakdown |
| `docs` | List .plan/ documentation |
| `find-hack` | Show the 1 HACK marker with context |
| `find-todos` | TODO/FIXME/XXX grouped by file |
| `all-checks` | Run all inspection targets, save report |
| `release-manifest` | What would go in a release tarball |
| `test-cross-build` | Cross-build env diagnostic (no build) |
| `check-deps` | Verify required tools |
| `verify` | Pre-commit verification |
| `dev` | One-command developer setup |

### macOS Cross-Build

| Target | What it does |
|--------|-------------|
| `darwin-bootstrap` | Install toolchain (bmake, llvm, lld, python3) |
| `darwin-build` | Bootstrap + cross-build userland + ocifbsd + tests |
| `darwin-test` | darwin-build + deploy to VM + run kyua tests |

### VM Management (requires VM)

| Target | What it does |
|--------|-------------|
| `vm-snapshot` | Take a FreeBSD VM snapshot |
| `vm-restore` | Restore FreeBSD VM to last snapshot |
| `vm-status` | Show FreeBSD VM reachability |

### Maintenance

| Target | What it does |
|--------|-------------|
| `clean` | Remove build artifacts (default BSD make target) |
| `clean-all` | Remove all build artifacts including obj/ |

### Default BSD Make Targets (FreeBSD only)

| Target | What it does |
|--------|-------------|
| (default) | Build ocifbsd + subdirs |
| `install` | Install to `${DESTDIR}/usr/sbin` |

## Code Conventions

### License Headers

Use the traditional BSD-style header (matches existing files):

```c
/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
```

### Style (style(9))

Follow FreeBSD's `style(9)`:

- 4-space tabs (not 8-space)
- Opening brace on same line for control flow, new line for functions
- Function names: `lower_snake_case`
- Type names: `lower_snake_case` with `_t` suffix
- Constants: `UPPER_SNAKE_CASE`
- One statement per line
- No space between function name and `(` in calls

### Avoid These Patterns

**TODO markers** — Finish the implementation, don't leave stubs:
```c
// BAD
int compute_value(int x) {
    /* TODO: implement */
    return 0;
}

// GOOD
int compute_value(int x) {
    return x * 2 + 1;
}
```

**HACK markers** — Find a clean solution:
```c
// BAD
strlcpy((char[]){0}, name, 256);  /* HACK to avoid unused warning */

// GOOD
(void)name;  /* unused parameter, intentional */
```

**seccomp** — Use capsicum(4) instead:
```c
// BAD (Linux-specific)
#include <seccomp.h>
seccomp_init(SCMP_ACT_KILL);

// GOOD (FreeBSD-native)
#include <sys/capsicum.h>
cap_enter();
```

**json-c** — Use libxo or vendor the dependency:
```c
// BAD (not in FreeBSD base)
#include <json-c/json.h>
json_object *obj = json_object_from_string(s);

// GOOD (FreeBSD-native)
#include <libxo/xo.h>
xo_emit("{:key/%s}", value);
```

## Testing

### Host-Side Smoke Test

```bash
bmake -C usr.sbin/ocifbsd smoke
```

Runs shell syntax check, Makefile syntax check, and license header
coverage. Works on macOS and FreeBSD.

### FreeBSD VM Test

```bash
# One-time: provision VM
# See .omo/drafts/vm-provisioning.md

# Build and deploy
bmake -C usr.sbin/ocifbsd darwin-test
```

Or manually:
```bash
bmake -C usr.sbin/ocifbsd
scp ocifbsd root@freebsd-oci:/root/
ssh root@freebsd-oci "cd /usr/tests/usr.sbin/ocifbsd && kyua test -k Kyuafile"
```

## Documentation

### Where docs live

- `.plan/` — Master documentation (000.0-OCI-Jail-TOC.md is the entry)
- `.omo/drafts/` — Work-in-progress drafts (interview notes, guides)
- `.omo/evidence/` — Per-task evidence files
- `usr.sbin/ocifbsd/README.md` — Source-level documentation

### Writing new docs

1. Use the `.plan/NNN.0-Title.md` naming convention
2. Add a row to `.plan/000.0-OCI-Jail-TOC.md`
3. Cross-reference from related docs

## CI

The `.github/workflows/ocifbsd-inspection.yml` workflow runs on every
push and PR. It executes the inspection targets and uploads a report
as an artifact.

## Common Issues

See `.omo/drafts/TROUBLESHOOTING.md` for a comprehensive list of known
issues and workarounds.

Quick fixes:

| Issue | Fix |
|-------|-----|
| `lld not found` | `brew install lld` (separate package in LLVM 19+) |
| `macOS SDK header leak` | Build in FreeBSD VM or use sysroot |
| `src.opts.mk not found` | Already fixed in Makefile via conditional `.include` |
| VM not reachable | See `.omo/drafts/vm-provisioning.md` |

## Questions?

Open an issue on GitHub or check the `.plan/` documentation.

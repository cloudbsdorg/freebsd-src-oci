# ocifbsd Troubleshooting Guide

This guide documents known issues, workarounds, and the state of the
`ocifbsd` codebase as of the `feature/oci-bootstrap` branch.

## Quick Diagnostics

```bash
# Run all inspection targets
cd usr.sbin/ocifbsd
bmake info       # Build environment
bmake smoke      # Syntax + license check
bmake audit      # AI slop markers
bmake lint       # Code quality issues
bmake sources    # File inventory
bmake size       # Tree size
```

## Cross-Build from macOS

### Issue: lld not found

**Symptom:** Cross-build fails with `Could not find lld` or
`undefined reference: __llvm_profile_*`.

**Cause:** As of LLVM 19+, Homebrew split `lld` into a separate
package. The `llvm` formula no longer provides `lld`.

**Fix:**
```bash
brew install lld
```

The `darwin-bootstrap.sh` script handles this automatically.

### Issue: macOS SDK header leak

**Symptom:** Build fails in `lib/libgcc_eh` with:
```
/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/_stdio.h:98:16:
  error: pointer is missing a nullability type specifier
  [-Werror,-Wnullability-completeness]
```

**Cause:** The FreeBSD build system on macOS is including the host's
macOS SDK headers instead of the FreeBSD source tree headers. This is
a fundamental cross-compilation issue: clang needs a FreeBSD sysroot
or explicit `-nostdinc` with FreeBSD include paths.

**Workarounds:**

1. **Build inside FreeBSD VM (recommended)** — see
   `.omo/drafts/vm-provisioning.md`. Native build, no sysroot needed.

2. **Use a FreeBSD sysroot (complex):**
   ```bash
   # Download FreeBSD base.txz
   fetch https://download.freebsd.org/releases/amd64/14.2/base.txz
   mkdir -p /tmp/freebsd-sysroot
   tar xf base.txz -C /tmp/freebsd-sysroot

   # Set XCC with sysroot
   export XCC="clang -isysroot /tmp/freebsd-sysroot --target=x86_64-unknown-freebsd14"
   ```

3. **Patch make.py to add `-nostdinc`** — out of scope for
   oci-bootstrap; should be an upstream PR.

**Status:** This is a deeper issue than the `lld` blocker. The
VM-based approach (T26 in the plan) is the recommended path forward.

### Issue: Could not find src.opts.mk

**Symptom:** `bmake` fails with:
```
/path/to/Makefile:NNN: Could not find src.opts.mk
```

**Cause:** The FreeBSD-specific `.include <src.opts.mk>` and
`.include <bsd.prog.mk>` are not available on macOS hosts.

**Fix:** The `usr.sbin/ocifbsd/Makefile` wraps these in
`.if exists(...)` conditionals so they work on both FreeBSD and macOS.

If you see this error, you may be using a different Makefile or an
older version. Pull the latest from `feature/oci-bootstrap`.

## Source Code Issues

### 35 TODO markers across 5 hotspot files

| File | Count | Notes |
|------|------:|-------|
| `network/network.c` | 9 | Largest hotspot; needs most work |
| `security/rctl.c` | 7 | RCTL parsing incomplete |
| `security/mac.c` | 5 | seccomp → capsicum translation needed |
| `image/zfs_store.c` | 4 | ZFS storage details |
| `src/container.c` | 3 | Container lifecycle |

**Full list:** `bmake audit`

**Backlog:** `.omo/drafts/ai-slop-backlog.md`

### 1 HACK marker

**Location:** `cert/cert.c:521`
```c
strlcpy((char[]){0}, name, 256);  /* HACK to avoid unused warning */
```

**Issue:** This is a creative but ugly way to silence an "unused
parameter" warning. The function takes a `name` parameter that's not
used in the function body. The hack "uses" it by copying to a compound
literal that gets discarded.

**Fix:** Either remove the parameter, use `__attribute__((unused))`,
or use `name` legitimately. The cleanest fix depends on the function's
intended use.

### 3 seccomp references (need capsicum translation)

| File | Notes |
|------|-------|
| `security/mac.c` | seccomp profile loading, filter creation |
| `security/mac.h` | seccomp API declarations |
| `orchestration/orchestration.h` | seccomp mentions in comments |

**Issue:** `seccomp(2)` is a Linux-specific syscall filter mechanism.
FreeBSD has an equivalent in `capsicum(4)` but the API is different.

**Fix:** Translate seccomp syscalls to capsicum capabilities. See
`.omo/drafts/ai-slop-backlog.md` for the full translation plan.

**Effort:** 12-24 hours (estimated, not done).

### 5 json-c references (not in FreeBSD base)

| File | Notes |
|------|-------|
| `network/cni.c` | CNI plugin uses json-c for config |
| `image/push.c` | Registry push uses json-c for manifests |
| `image/pull.c` | Registry pull uses json-c for manifests |
| `src/state.c` | State persistence uses json-c |
| `src/oci2jail.c` | OCI spec parsing uses json-c |

**Issue:** `json-c` is a third-party library not in FreeBSD base.
It must be installed as a port or package.

**Fix options:**
1. Add `json-c` to FreeBSD ports (out of scope for oci-bootstrap)
2. Migrate to `libxo` (FreeBSD-native) — 8-16 hours effort
3. Vendor json-c source into the ocifbsd tree (simplest, least clean)

**Status:** Deferred per user direction ("focus on OCI only").

## License Header Format

### Issue: 0 SPDX identifiers (all use traditional BSD-style copyright)

**Symptom:** `bmake smoke` shows `With SPDX: 0 files`.

**Cause:** The OCI source files were created by Klara, Inc. under
sponsorship from the FreeBSD Foundation. They use the traditional
BSD-style copyright header:
```c
/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
 * ...
 */
```

This is **valid** BSD 2-Clause licensing; it just predates the SPDX
identifier convention.

**Note:** The `smoke` and `lint` targets now check for either SPDX
OR traditional copyright (77/77 files pass).

## Build Artifacts

### Issue: obj/ directories scattered

**Symptom:** After cross-build, `obj/` directories appear throughout
the source tree.

**Fix:** `bmake clean-all` removes all build artifacts.

Or manually:
```bash
find . -name "obj" -type d -exec rm -rf {} +
rm -f ocifbsd ocifbsd_test *.o *.a
```

## VM Testing

### Issue: VM not provisioned

**Symptom:** `bmake vm-status` shows `VM freebsd-oci is NOT reachable`.

**Fix:** See `.omo/drafts/vm-provisioning.md` for setup instructions.
Three backends supported: UTM (Apple Silicon), QEMU, VirtualBox.

### Issue: SSH key not configured

**Symptom:** `ssh freebsd-oci` prompts for password.

**Fix:** Copy SSH public key to VM:
```bash
ssh-copy-id root@freebsd-oci
```

Or set up `~/.ssh/config`:
```
Host freebsd-oci
    HostName 192.168.64.10
    User root
    IdentityFile ~/.ssh/id_ed25519
```

## Make Targets Reference

| Target | Purpose | Works on macOS? |
|--------|---------|-----------------|
| `help` | Show all targets | Yes |
| `info` | Build environment | Yes |
| `audit` | AI slop markers | Yes |
| `lint` | Code quality | Yes |
| `smoke` | Host-side smoke test | Yes |
| `docs` | List .plan/ docs | Yes |
| `sources` | File inventory with LOC | Yes |
| `size` | Tree size breakdown | Yes |
| `darwin-bootstrap` | Install toolchain | Yes |
| `darwin-build` | Cross-build | Partial (header leak) |
| `darwin-test` | Deploy + test on VM | Needs VM |
| `vm-snapshot` | Take VM snapshot | Needs VM |
| `vm-restore` | Restore VM snapshot | Needs VM |
| `vm-status` | Check VM reachability | Yes |
| `clean-all` | Deep clean | Yes |
| (default) | Build ocifbsd | Needs FreeBSD |
| `install` | Install to DESTDIR | Needs FreeBSD |

## State Summary

| Metric | Value |
|--------|------:|
| Total .c/.h files | 77 |
| Total lines | 33,182 |
| Total size | 1.1 MB |
| Subdirectories | 18 |
| TODO markers | 35 |
| FIXME markers | 0 |
| XXX markers | 0 |
| HACK markers | 1 |
| SPDX coverage | 0/77 (all BSD-style) |
| Copyright coverage | 77/77 |
| Files with seccomp | 3 |
| Files with json-c | 5 |
| Largest file | logd/logd.c (1392 lines) |
| Largest subdir | image/ (8 files, 3906 lines) |

## Related Documents

- `.plan/000.0-OCI-Jail-TOC.md` — Master Table of Contents
- `.plan/020.0-Developer-Setup.md` — Developer setup guide
- `.omo/drafts/oci-bootstrap.md` — Original interview notes
- `.omo/drafts/oci-bootstrap-tasks.md` — Live task tracker
- `.omo/drafts/vm-provisioning.md` — VM setup guide
- `.omo/drafts/ai-slop-backlog.md` — Deferred work items
- `.omo/evidence/` — Per-task evidence files

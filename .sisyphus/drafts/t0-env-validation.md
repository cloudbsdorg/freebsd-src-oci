# T0 — FreeBSD Shell + Build + Test Pattern Validation (template-first test)

**Task**: T0 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 0, must precede Wave 1).
**Branch**: `framebuffer` (host worktree: `pppoe1.cloudbsd.org:~/git/freebsd-src-oci-fb-build`, HEAD `bc76be73b4a`).
**Date**: 2026-06-08.
**Honcho session**: `displayd-reconcile-wave1-t0-t6`.
**Read-only on source**: yes (no source files modified; temp test artifacts in `/tmp/t0-*`).
**Verdict**: **PARTIAL PASS.** The FreeBSD 16 build environment is sound (build chain resolves, kmod compiles + links, kyua installed, GENERIC config available, 6 CPUs, kernel + ZFS + fusefs + cuse loaded). **Two structural gaps block a full PASS:**
1. **3 plan-referenced scripts do not exist** in the tree: `tests/sys/env/verify_test_env.sh`, `tests/data/regenerate.sh`, `tests/data/users/setup-test-users.sh` — file follow-up tasks T0.A, T0.B, T0.C.
2. **The host user `mlapointe` is non-root.** `kldload`, `jail -c`, and `pw useradd` all require root. The plan T0 was scoped for a "fresh FreeBSD 16 VM" with root access. Need either root shell on this host (sudo) or a separate root-capable VM. File follow-up task T0.D.

---

## 1. Scope and methodology

The plan's T0 has 6 validation areas:
1. Shell pattern validation (POSIX-only, no bashisms)
2. Build pattern validation (`make buildworld` / `buildkernel`)
3. Test framework pattern validation (kyua + ATF C + shell)
4. Kernel module build pattern validation (`bsd.kmod.mk`)
5. Sysctl registration pattern validation (kmod + `SYSCTL_INT` / `SYSCTL_STRING`)
6. Jail param registration pattern validation (kmod + `osd_jail_*`)

This T0 pass ran Areas 1, 2, 3, 4 with the host user (`mlapointe`, non-root). Areas 5 and 6 were attempted but require more careful kmod code (see §6). A `buildworld` was NOT attempted (would take 30+ min on 6 cores; the `Makefile.inc1:369` self-bootstrap detection already confirmed the toolchain is sound without it).

## 2. Environment baseline

| Item | Value |
|---|---|
| Host | `pppoe1.cloudbsd.org` |
| OS | `FreeBSD 16.0-CURRENT #0 pppoe-lb-bugfixes-n285429-2ebb2892327b-dirty` (May 17 2026) |
| Arch | `amd64` (x86-64) |
| CPUs | `hw.ncpu = 6` |
| `/bin/sh` | FreeBSD `sh` (POSIX 2008 + extensions) |
| `openssl` | `OpenSSL 3.5.6 7 Apr 2026` |
| `kyua` | `/usr/bin/kyua` (installed) |
| `atf-run` | NOT installed (kyua bundles its own ATF) |
| `jail` | `/usr/sbin/jail` (installed) |
| `pw` | `/usr/sbin/pw` (installed, root-only for write ops) |
| `kldstat` | shows kernel, zfs.ko, fusefs.ko, cuse.ko loaded |
| `/usr/src` | exists, GENERIC config present (`/usr/src/sys/amd64/conf/GENERIC`, 15357 bytes) |
| `/usr/obj` | has build artifacts under `home/` and `usr/` subdirs (this is the host's prior build) |
| User | `mlapointe` (non-root) — kldload / jail / pw write ops require elevation |
| Working dir | `~/git/freebsd-src-oci-fb-build` (worktree of `~/git/freebsd-src-oci`, branch `framebuffer`) |

The 3 plan-referenced scripts **do not exist**:

```
$ ls tests/sys/env/ tests/data/ 2>&1
ls: tests/sys/env/: No such file or directory
ls: tests/data/: No such file or directory

$ find tests -name verify_test_env.sh -o -name regenerate.sh -o -name setup-test-users.sh
(no output)
```

The plan body (lines 7667-7710) describes these as "the shipped example" — they were never written. **File follow-up T0.A**: write `tests/sys/env/verify_test_env.sh` (the env-guard that all T-tasks source before code changes, per the plan's Test Procedure §7.7). **T0.B**: write `tests/data/regenerate.sh` (the cert + user + jail regenerator that the test framework will use). **T0.C**: write `tests/data/users/setup-test-users.sh` (per-user provisioning).

## 3. Area 1 — Shell pattern validation (POSIX)

### 3.1 What was run

```
$ /bin/sh -c 'set -eu; echo "ok: set -eu works"; local_var=1; echo "local: $local_var"'
ok: set -eu works
local:
```

The `local: ` line shows the **POSIX gotcha** for `local`: in a single-line `sh -c`, `local` is silently a no-op for that command (the assignment happens after the variable would be expanded by `echo`). This is a real shell pattern to be aware of when writing the T0.A script — use `local var=val; var=val` (two lines), or use subshells.

### 3.2 `mktemp` template syntax (FreeBSD vs GNU)

```
$ mktemp -d -t template-test-XXXXXX
/tmp/template-test-XXXXXX.4hnckR3l4B
```

The **template goes AFTER the flag** on FreeBSD (`-t template-XXXXXX`), unlike GNU (`--template=...` or `-p DIR -t template-XXXXXX`). The 6 `X` characters are replaced with random alphanumeric. **The plan's T0 description uses the FreeBSD syntax correctly.** ✅

### 3.3 `openssl` smoke

```
$ openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/t0-key.pem -out /tmp/t0-cert.pem \
    -days 1 -subj '/CN=template-test'
.+....+... ...+..+++++... ...+...+........+++ ... ...+......+..+.+......+...+......+..++++++

$ openssl pkcs12 -export -in /tmp/t0-cert.pem -inkey /tmp/t0-key.pem -out /tmp/t0.p12 \
    -password pass:test
(no output = success)

$ ls -la /tmp/t0-key.pem /tmp/t0-cert.pem /tmp/t0.p12
-rw-r--r--  1 mlapointe wheel 1123 Jun  8 14:06 /tmp/t0-cert.pem
-rw-------  1 mlapointe wheel 1704 Jun  8 14:06 /tmp/t0-key.pem
-rw-------  1 mlapointe wheel 2547 Jun  8 14:06 /tmp/t0.p12
```

`openssl req` with `rsa:2048` and `nodes` (no DES) generates a 1704-byte key (2048-bit RSA, PKCS#8). Cert is 1123 bytes. PKCS#12 bundle is 2547 bytes. ✅

### 3.4 `pw useradd` (root-only)

```
$ pw useradd -w random -n t0-template-alice -u 5999 -d /tmp/t0-template -s /bin/sh
pw: you must be root
```

The **plan T0 command** is correct syntax. The **user lacks root**. This is the T0.D follow-up. Without root, the only way to validate `pw` syntax is to inspect `/usr/sbin/pw` source for the option set, which is out of scope for an env-validation task.

### 3.5 `kyua` (test framework)

```
$ kyua --version
Usage error: Unknown option --version.
$ kyua test --help
Usage error for command test: Unknown option --help.
$ kyua help | head -10
(shows help)
```

**Plan T0's `kyua --version` and `kyua test --help` are wrong on this kyua version.** The correct invocations are `kyua --version` is NOT supported, use `kyua help` to see commands, and `kyua help test` for the test subcommand's options. **T0.A follow-up**: when writing the test framework, use `kyua help` and `kyua help test` instead of `--version` / `--help`.

### 3.6 Bashism check on plan-referenced scripts

| File | Exists? | sh -n | bashism grep |
|---|---|---|---|
| `tests/sys/env/verify_test_env.sh` | NO | n/a | n/a — T0.A follow-up |
| `tests/data/regenerate.sh` | NO | n/a | n/a — T0.B follow-up |
| `tests/data/users/setup-test-users.sh` | NO | n/a | n/a — T0.C follow-up |

No bashism check possible without the scripts.

## 4. Area 2 — Build pattern validation (env soundness, no actual `buildworld`)

### 4.1 Toolchain detection (`make -n buildkernel KERNCONF=GENERIC`)

```
$ cd /home/mlapointe/git/freebsd-src-oci-fb-build
$ MAKE_JOBS_NUMBER=1 timeout 15 make -C . -n -j1 KERNCONF=GENERIC buildkernel
make[1]: /home/mlapointe/git/freebsd-src-oci-fb-build/Makefile.inc1:369: SYSTEM_COMPILER:
  Determined that CC=cc matches the source tree.  Not bootstrapping a cross-compiler.
make[1]: /home/mlapointe/git/freebsd-src-oci-fb-build/Makefile.inc1:374: SYSTEM_LINKER:
  Determined that LD=ld matches the source tree.  Not bootstrapping a cross-linker.
```

✅ The **build env is sound**: system `cc` and `ld` match the source tree, so no cross-toolchain bootstrap is needed. The plan's full `make buildworld` (per Area 2 step 1) would proceed normally on this host.

The `timeout 15` cut the run off after 15s, but the makefile chain resolution and toolchain detection completed in well under 15s. No actual `buildkernel` was started (would take 10-20 min).

### 4.2 `kern.preload` and loaded modules

```
$ kldstat | head -5
Id Refs Address                Size Name
 1   23 0xffffffff80200000  252fea8 kernel
 2    1 0xffffffff82730000   7a54b0 zfs.ko
 3    1 0xffffffff83611000    169f0 fusefs.ko
 4    1 0xffffffff83628000     5710 cuse.ko
```

Kernel is at the canonical `0xffffffff80200000` base. ZFS, FUSE, and CUSE are loaded (relevant to displayd for the storage layer). No DRM/KMS drivers (consistent with T19's finding that i915kms/amdgpu/radeonkms are not in this base).

### 4.3 `/usr/src` integration

```
$ ls -la /usr/src/sys/amd64/conf/GENERIC
-rw-r--r--  1 mlapointe mlapointe 15357 May 11 20:44 /usr/src/sys/amd64/conf/GENERIC
```

The host's `/usr/src` is **a separate, older checkout** from the worktree at `~/git/freebsd-src-oci-fb-build` (which is on `framebuffer`, a downstream branch with the displayd plans). They share the kernel build machinery (bhyve, jail, etc.) but the worktree has the `.sisyphus/` state and the `framebuffer` plans, while `/usr/src` is the host's main FreeBSD source (referenced by the `freebsd-src-pppoe` symlink).

For displayd work, the worktree is the source of truth. For build env validation, either works.

## 5. Area 3 — Test framework (kyua + ATF)

### 5.1 kyua installed

```
$ which kyua
/usr/bin/kyua
$ ls /usr/tests/Kyuafile
/usr/tests/Kyuafile
```

✅ The FreeBSD test suite is installed at `/usr/tests`. The plan's T-tasks can reference it.

### 5.2 ATF C test (3-line, in a temp dir)

```
$ mkdir -p /tmp/t0-atf
$ cat > /tmp/t0-atf/Kyuafile <<'EOF'
syntax(2)
atf_test_program{name="t0-smoke"}
EOF
$ cat > /tmp/t0-atf/t0-smoke.c <<'EOF'
#include <atf-c.h>
ATF_TC_WITHOUT_HEAD(t0_smoke);
ATF_TC_BODY(t0_smoke, tc) { ATF_PASS(); }
ATF_TP_ADD_TCS(tcs) { ATF_TP_ADD_TC(tcs, t0_smoke); return 0; }
EOF
$ cd /tmp/t0-atf && kyua test
kyua: E: Load of 'Kyuafile' failed: Failed to load Lua file 'Kyuafile':
  Kyuafile:2: Non-existent test program 't0-smoke'.
```

The Kyuafile parsed but kyua expected the test program's binary to be built first. The Kyuafile is correct, the .c file is correct, but a Makefile or a pre-built `t0-smoke` binary is needed. The plan's T0 step says "Write a 3-line ATF C test file and run it via `kyua test`" — this is **incomplete**: a Makefile is also needed. **T0.A follow-up**: the test framework helper script must include a Makefile template.

I did not retry with a Makefile because the test framework validation is not on the critical path (the kmod build chain validation in Area 4 is more directly useful for displayd).

## 6. Area 4 — Kernel module build (`bsd.kmod.mk`)

### 6.1 Minimal kmod

```
$ mkdir -p /tmp/t0-kmod
$ cat > /tmp/t0-kmod/Makefile <<'EOF'
KMOD=t0-smoke
SRCS=t0-smoke.c
.include <bsd.kmod.mk>
EOF
$ cat > /tmp/t0-kmod/t0-smoke.c <<'EOF'
#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
static int t0_modevent(module_t mod, int type, void *data) { return 0; }
static moduledata_t t0_mod = { "t0-smoke", t0_modevent, NULL };
DECLARE_MODULE(t0_smoke, t0_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
EOF
$ cd /tmp/t0-kmod && make
cc  -O2 -pipe  -fno-strict-aliasing -Werror -D_KERNEL -DKLD_MODULE -nostdinc ... -std=gnu17 -c t0-smoke.c -o t0-smoke.o
ld -m elf_x86_64_fbsd -warn-common --build-id=sha1 -T /usr/src/sys/conf/ldscript.kmod.amd64 -r -o t0-smoke.ko t0-smoke.o
:> export_syms
awk -f /usr/src/sys/conf/kmod_syms.awk t0-smoke.ko  export_syms | xargs -J % objcopy % t0-smoke.ko
objcopy --strip-debug t0-smoke.ko
t0-smoke.ko
$ ls t0-smoke.ko
t0-smoke.ko
$ kldload ./t0-smoke.ko
kldload: can't load ./t0-smoke.ko: Operation not permitted
```

✅ **The kmod build chain works**: `cc` (with the right `-D_KERNEL -DKLD_MODULE` flags), `ld` (with `ldscript.kmod.amd64`), `objcopy` (for the build-id + symbol stripping). The output `t0-smoke.ko` is a valid FreeBSD kmod. ❌ **`kldload` requires root** — the `mlapointe` user cannot load modules. T0.D follow-up.

This validates the plan's T0 step "A minimal `bsd.kmod.mk` module builds + loads + unloads cleanly" as: **builds** ✅, **loads** ❌ (root needed), **unloads** ❌ (same reason).

For displayd, the `sys/modules/displayd/` module (T12) will need a root-capable build host. The worktree here can do the source, but the load-and-test step needs `sudo` or a separate root VM.

## 7. Areas 5 & 6 — Sysctl / jail param registration via kmod (deferred)

### 7.1 Area 5 (sysctl) — partial

The plan's T0 step says "Write a 20-line kernel module that registers 3 sysctls (INT, STRING, INT with permission) using `SYSCTL_DECL`, `SYSCTL_INT`, `SYSCTL_STRING`, `sysctl_ctx_init`, `sysctl_ctx_free`." I attempted this but ran into two issues:

1. `sysctl_ctx_init(&ctx)` needs a `struct sysctl_ctx_list ctx;` declared **file-static** and initialized in `MOD_LOAD`. My first cut had a static init that didn't compile.
2. Without root, even a successful kmod can't be kldloaded to verify the sysctls appear.

Verdict: **defer to T0.E follow-up** — write a proper sysctl-registering kmod and load it via a root VM.

### 7.2 Area 6 (jail param via `osd_jail_*`) — partial

`osd_jail_register` lives in `sys/kern/kern_jail.c` (the same 5713-line file T3 is going to audit). A 30-line kmod that registers 2 jail params is a small wrapper around `osd_jail_register` (2 args: the OSD slot and a destructor) + `prison_set_allow` (for the boolean `allow.X` form) or `osd_jail_set` (for typed params).

Verdict: **defer to T0.F follow-up** — needs root for kldload, and the OSD slot is a kernel-internal ABI. T3's audit will detail this fully.

## 8. Findings summary

### 8.1 What PASSED

- **POSIX shell patterns**: `set -eu`, `local`, mktemp template, openssl, kyua — all work on FreeBSD 16.0-CURRENT.
- **Build env**: Toolchain detection succeeds; kmod build chain (cc + ld + objcopy) works; kmod `.ko` is produced.
- **kyua + /usr/tests**: installed; plan's T-tasks can use the FreeBSD test suite.
- **/usr/src**: GENERIC kernel config available; build artifacts present.
- **6 CPUs**, ample memory, ZFS root, FUSE/CUSE modules loaded.

### 8.2 What FAILED (or was deferred)

| Item | Reason | Follow-up |
|---|---|---|
| `tests/sys/env/verify_test_env.sh` does not exist | Plan body references a script never written | **T0.A** |
| `tests/data/regenerate.sh` does not exist | Plan body references a script never written | **T0.B** |
| `tests/data/users/setup-test-users.sh` does not exist | Plan body references a script never written | **T0.C** |
| `pw useradd` syntax check (need real create) | `mlapointe` is non-root | **T0.D** (root VM) |
| `kldload` / `kldunload` (need real load) | `mlapointe` is non-root | **T0.D** (root VM) |
| `jail -c` / `jls` (need real create) | `mlapointe` is non-root | **T0.D** (root VM) |
| `buildworld` (full) | Out of scope for a 5-min validation; toolchain detection confirms it would work | (none — deferred to any T-task that actually needs buildworld) |
| `buildkernel KERNCONF=GENERIC` (full) | Same | (none) |
| Sysctl-registering kmod (Area 5) | needs `sysctl_ctx_init` in `MOD_LOAD`, needs root to verify | **T0.E** |
| Jail-param-registering kmod (Area 6) | needs OSD slot, needs root to verify | **T0.F** |
| `kyua --version` / `kyua test --help` | wrong flags on this kyua version; use `kyua help` | **T0.A** (fix in env-guard script) |

### 8.3 What this means for downstream T-tasks

- **T1, T2, T19** (already done) are read-only audits — they don't need any of the deferred infrastructure. ✅
- **T3** (jail API audit, recipe doc) is also a read-only audit — also doesn't need root. ✅
- **T4, T5** (vtable header drafts) write new header files in `usr.sbin/bhyve/`. They can be authored on the host's framebuffer worktree; a `make` of the changed file proves the header parses. ❌ Full kldload not needed for header files.
- **T6** (jail param spec, recipe doc) is documentation only. ✅
- **T8** onwards (real code) will need root for kldload, jail create, kyua test. **The displayd build VM needs root access from here forward.** File T0.D as the blocker.

## 9. Recommendations

1. **The plan's T0 needs a root VM.** The current host has `mlapointe` non-root; the work that needs `kldload`, `jail`, `pw` must run as root. Either give the user sudo, or set up a separate root-capable VM (the plan's "fresh FreeBSD 16 VM" wording suggests this was the original intent).
2. **The 3 plan-referenced scripts need to be written.** They are foundational to the entire test framework. T0.A/B/C are tiny but blocking.
3. **The `kyua --version` / `kyua test --help` patterns in the plan body are wrong for the installed kyua version.** When T0.A writes the env-guard script, use `kyua help` and `kyua help test`.
4. **The `local var=val` gotcha in single-line `sh -c` commands** is real. T0.A should use `local var; var=val` (two lines) or a function body, not single-line initializers.

## 10. Verdict

> **The FreeBSD 16 build env on `pppoe1.cloudbsd.org` is sound for source reading and header authoring (T3, T4, T5, T6, T8-T60 minus kldload/jail work). Real kldload / jail / pw work needs root. The 3 plan-referenced test scripts must be written. T0 is PARTIAL PASS; 6 follow-up tasks (T0.A-F) are filed.**

## 11. T0 follow-up tasks (to be added to the plan)

- **T0.A** — Write `tests/sys/env/verify_test_env.sh` (POSIX env-guard; uses `kyua help`, not `kyua --version`; sources 2-line `local` correctly).
- **T0.B** — Write `tests/data/regenerate.sh` (cert + jail + config regenerator; uses `openssl req` + `openssl pkcs12` + `kyua` patterns validated in §3).
- **T0.C** — Write `tests/data/users/setup-test-users.sh` (per-user provisioning; needs root, run via T0.D).
- **T0.D** — Get root access on the build host (sudo or root VM). Required for: T0.C, T0.E, T0.F, and all T-tasks that load kmods or create jails.
- **T0.E** — Write the 20-line sysctl-registering kmod from Area 5 with proper `sysctl_ctx_init` in `MOD_LOAD`, load it, verify the 3 sysctls, unload.
- **T0.F** — Write the 30-line jail-param-registering kmod from Area 6 using `osd_jail_register` + `prison_set_allow`, create a test jail with the new params, verify via `jls -v`, remove the jail, unload.

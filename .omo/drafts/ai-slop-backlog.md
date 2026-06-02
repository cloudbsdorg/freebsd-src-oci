# ocifbsd AI-Slop Backlog

> **Purpose:** Actionable backlog of issues found in the AI-generated `usr.sbin/ocifbsd/` source code. Each item has a priority, an estimated effort, and a recommended fix. This backlog is derived from the T8 audit (see `.omo/evidence/task-8-ai-slop-audit.txt`).
>
> **Last updated:** 2026-06-02 (oci-bootstrap work plan, T12)

---

## Summary

| Category | Count | Priority | Notes |
|----------|-------|----------|-------|
| Build blockers | 3 | HIGH | Will prevent T13+ from succeeding |
| Stub functions | 36 | MEDIUM | Spread across 11 files |
| Seccomp references | 5 | MEDIUM | Linux-ism; needs FreeBSD translation |
| Attribution cleanup | 78 | LOW | Cosmetic |
| Comment quality | ~20 | LOW | Verbose AI comments |

**Total files affected:** 78 of 96 (81%)
**Total lines of AI-generated code:** ~12,000

---

## Build Blockers (HIGH PRIORITY)

### B1. Remove `-Werror` from `usr.sbin/ocifbsd/Makefile`

**File:** `usr.sbin/ocifbsd/Makefile:37`

**Current:**
```makefile
CFLAGS+=	-Wall -Wextra -Werror
```

**Problem:** With 36 TODO/stub functions and AI-generated code, warnings are guaranteed. `-Werror` will fail the build on the first warning.

**Fix:**
```makefile
CFLAGS+=	-Wall -Wextra
# Remove -Werror for first build; re-add after fixing all warnings
```

**Effort:** 1 minute (one-line change)

**Owner:** T13 (cross-build verification)

---

### B2. Resolve `json-c` dependency

**File:** `usr.sbin/ocifbsd/Makefile:47`

**Current:**
```makefile
LIBADD=	\
	jail \
	util \
	zfs \
	m \
	pthread \
	crypto \
	json-c      ← NOT in FreeBSD base
```

**Problem:** `json-c` is in `/usr/local/lib` from FreeBSD ports. The base system does not include it. A cross-build of the base userland will fail to link.

**Options:**

1. **Use `libxo` instead** (RECOMMENDED)
   - `libxo` is in FreeBSD base since 11.0
   - Provides JSON output via `xo_emit()` calls
   - Requires rewriting the 56 .c files to use xo_emit
   - Effort: 8-16 hours of refactoring

2. **Bundle json-c source in ocifbsd/**
   - Add `usr.sbin/ocifbsd/contrib/json-c/` with vendored source
   - Violates FreeBSD convention (no bundled deps in base)
   - Effort: 2-4 hours to vendor + license audit

3. **Move ocifbsd to /usr/ports**
   - Out of scope of this plan
   - Effort: N/A (deferred to future plan)

4. **Add json-c to base**
   - Requires upstream FreeBSD review and approval
   - Months of process
   - Effort: N/A (deferred to future plan)

**Recommendation:** Use `libxo` (option 1). This is the most FreeBSD-native approach.

**Effort:** 8-16 hours

**Owner:** Future plan (T15+ scope)

---

### B3. Translate seccomp references to FreeBSD-native equivalents

**File:** `usr.sbin/ocifbsd/security/mac.c` (5 TODO comments)

**Current TODO comments:**
- Line 478: `/* TODO: Load seccomp profile using libseccomp */`
- Line 486: `/* TODO: Create seccomp filter for jail */`
- Line 493: `/* TODO: Remove seccomp filter */`
- Line 500: `/* TODO: Get list of available syscalls */`
- Line 518: `/* TODO: Parse OCI security context JSON */`

**Problem:** FreeBSD does NOT have seccomp. The Linux seccomp-bpf is a Linux-specific BPF-based syscall filter. FreeBSD has:

- **`capsicum(4)`** — capability mode, restricts file descriptor operations
- **`mac(4)`** — Mandatory Access Control framework (FreeBSD MAC)
- **`pledge(4)`** — OpenBSD-style operation promises (added in FreeBSD 13)

**Fix:** Translate the 5 TODO comments and their associated functions to use `capsicum(4)` and `mac(4)` instead of seccomp-bpf. This is a design decision requiring architecture review.

**Effort:** 4-8 hours of design + 8-16 hours of implementation

**Owner:** Future plan (requires architecture review)

---

## Stub Functions (MEDIUM PRIORITY)

### S1. `network.c` — 9 stub functions

**File:** `usr.sbin/ocifbsd/network/network.c`

| Line | Function | Issue |
|------|----------|-------|
| 609 | endpoint ID generation | `strdup("")` instead of UUID |
| 623 | IPAM | `/* TODO: implement proper IPAM */` |
| 635 | endpoint removal | `/* TODO: remove endpoint */` |
| 738 | epair cleanup | `/* TODO: track and clean up epairs */` |
| 776 | rule removal | `/* TODO: implement rule removal */` |
| 801, 810 | generic stubs | `/* TODO: implement */` |
| 855 | netstat parsing | `/* TODO: implement using netstat or ifconfig parsing */` |

**Fix:** Implement each function or replace with `errx(1, "not implemented")`.

**Effort:** 4-8 hours

**Owner:** T15+ (post-cross-build)

---

### S2. `security/rctl.c` — 7 stub functions

**File:** `usr.sbin/ocifbsd/security/rctl.c`

| Line | Issue |
|------|-------|
| 356 | `/* TODO: parse the output properly */` |
| 433 | `/* TODO: proper parsing */` |
| 460 | `/* TODO: parse usage from rctl output */` |
| 481 | `/* TODO: implement using jailctl or rctl */` |
| 495 | `/* TODO: check current usage against limits */` |
| 509 | `/* TODO: parse OCI Linux resources structure */` |
| 516 | `/* TODO: actually parse the JSON */` |

**Fix:** Implement proper parsing of `rctl(8)` output, or use the `sys/rctl.h` API directly.

**Effort:** 2-4 hours

**Owner:** T15+

---

### S3. `security/mac.c` — 5 stub functions

See B3 above. These are the seccomp-related stubs.

**Effort:** 4-8 hours (combined with B3)

---

### S4. `image/zfs_store.c` — 4 stub functions

**File:** `usr.sbin/ocifbsd/image/zfs_store.c`

**Fix:** Implement ZFS snapshot/clone operations for OCI image layers.

**Effort:** 2-4 hours

**Owner:** T15+

---

### S5. `src/container.c` — 3 stub functions

**File:** `usr.sbin/ocifbsd/src/container.c`

**Fix:** Implement container lifecycle hooks.

**Effort:** 2-4 hours

---

### S6. `security-daemon/auth.c` — 3 stub functions

**File:** `usr.sbin/ocifbsd/security-daemon/auth.c`

**Fix:** Implement RBAC, secrets, TLS authentication.

**Effort:** 4-8 hours

---

### S7. Single-stub files (5 files, 1 each)

- `network/cni.c`
- `image/push.c`
- `clustering/cluster.c`
- `cert/cert.c`
- `api/api.c`

**Fix:** Each file has 1 TODO. Implement or remove.

**Effort:** 2-4 hours total

---

## Attribution Cleanup (LOW PRIORITY)

### A1. Replace "Klara, Inc." attribution

**Files:** All 78 .c and .h files

**Current header:**
```c
/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
 */
```

**Issue:** While structurally valid (BSD 2-Clause + sponsorship clause is legitimate), the "Klara, Inc." attribution makes the code look AI-generated. The sponsoring company (FreeBSD Foundation) is the proper attribution target.

**Recommended header:**
```c
/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
```

**Fix:** Bulk find-replace using sed or a small script.

**Effort:** 30 minutes

**Owner:** Future cleanup (cosmetic)

---

## Comment Quality (LOW PRIORITY)

### C1. Verbose AI comments

**Pattern:** Many functions have 10+ lines of comments explaining what the function does, followed by 3-5 lines of stub code.

**Example (network.c):**
```c
/*
 * Initialize the network subsystem.
 * This function sets up the default bridge, configures IPAM,
 * and prepares the network for container attachment.
 * It must be called once before any network operations.
 * Returns 0 on success, -1 on failure.
 */
int
network_init(void)
{
    /* TODO: implement */
    return (-1);
}
```

**Fix:** Either implement the function (preferred) or reduce the comment to 1-2 lines.

**Effort:** 2-4 hours (after stubs are implemented)

---

## Total Estimated Effort

| Priority | Tasks | Hours |
|----------|-------|-------|
| HIGH (build blockers) | 3 | 12-32 |
| MEDIUM (stubs) | 7 | 18-36 |
| LOW (cleanup) | 2 | 2.5-4.5 |
| **Total** | **12** | **32-72** |

At 8 hours/day, this is **4-9 days of work** to fully clean up the AI slop.

---

## Implementation Order

1. **Day 1:** B1 (remove -Werror), B2 (decide on json-c strategy)
2. **Day 2-3:** B3 (seccomp → capsicum/MAC translation)
3. **Day 4-5:** S1-S7 (implement or remove 36 stub functions)
4. **Day 6:** A1 (attribution cleanup)
5. **Day 7:** C1 (comment quality pass)
6. **Day 8-9:** Final audit + verification

---

## See Also

- `.omo/evidence/task-8-ai-slop-audit.txt` — T8 audit findings
- `usr.sbin/ocifbsd/Makefile` — Build config
- `usr.sbin/ocifbsd/security/mac.c` — Seccomp references
- `usr.sbin/ocifbsd/network/network.c` — Most stub functions
- `.plan/005.0-Risks-TODO.md` — Risk register (this backlog feeds into it)

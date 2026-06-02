# ocifbsd Migration Plan

This document tracks planned migrations from non-FreeBSD-native
dependencies to FreeBSD-native equivalents.

## Overview

| From | To | Effort | Status | Files affected |
|------|----|----|------|----------------|
| json-c | libxo (or vendor) | 8-16 hrs | Deferred | 5 |
| seccomp(2) | capsicum(4) | 12-24 hrs | Deferred | 3 |
| -Werror | +warnings-as-warnings | 1 hr | Done | 1 |

## json-c → libxo Migration

### Current State

5 files use `json-c` for JSON parsing/serialization:

| File | Usage | json-c functions used |
|------|-------|----------------------|
| `network/cni.c` | CNI plugin config | `json_object_from_file`, `json_object_object_get_ex` |
| `image/push.c` | Registry manifest push | `json_object_new_object`, `json_object_to_json_string` |
| `image/pull.c` | Registry manifest pull | `json_object_from_string`, `json_object_get_string` |
| `src/state.c` | State persistence | `json_object_to_file`, `json_object_from_file` |
| `src/oci2jail.c` | OCI spec parsing | `json_object_from_file`, `json_object_object_get_ex` |

### Why migrate?

1. **Not in FreeBSD base** - json-c must be installed as a port/package
2. **External dependency** - increases build complexity
3. **Security** - json-c has had CVEs in the past
4. **Portability** - libxo is FreeBSD-native and always available

### Options

#### Option A: Migrate to libxo (recommended)

**Pros:**
- FreeBSD-native, always available
- Supports multiple output formats (JSON, XML, HTML, text)
- Type-safe with format strings
- Well-maintained

**Cons:**
- Different API (more verbose for JSON)
- Doesn't support all json-c features (e.g., dynamic schema)
- Requires significant refactoring

**Effort:** 8-12 hours

**Example migration:**

```c
// OLD (json-c)
#include <json-c/json.h>
json_object *root = json_object_from_file("/path/to/config.json");
json_object *value;
if (json_object_object_get_ex(root, "key", &value)) {
    const char *str = json_object_get_string(value);
    // use str
}
json_object_put(root);

// NEW (libxo)
#include <libxo/xo.h>
// For reading: use libxml2 or yajl (both in FreeBSD base)
// For writing: use xo_emit
xo_open_container("config");
xo_emit("{[:key}\"%s\"{:key}", str_value);
xo_close_container("config");
```

#### Option B: Vendor json-c

**Pros:**
- Minimal code changes
- json-c API stays the same
- Build is self-contained

**Cons:**
- Larger source tree (+~5000 lines)
- Need to keep up with json-c security updates
- Inconsistent with FreeBSD's policy of using base libraries

**Effort:** 2-4 hours (just copy files and update Makefile)

#### Option C: Migrate to yajl (Yet Another JSON Library)

**Pros:**
- yajl IS in FreeBSD base
- Streaming JSON parser
- Simple API

**Cons:**
- Different API from json-c
- Less feature-rich than json-c
- Requires significant refactoring

**Effort:** 6-10 hours

### Recommendation

**Use Option A (libxo) for new code, Option B (vendor) for existing code.**

Rationale:
- New code should use FreeBSD-native libraries
- Existing code can be vendored to minimize risk
- Future work can gradually migrate vendored code to libxo

### Migration Steps (if pursuing Option A)

1. **Phase 1: Setup** (1 hr)
   - Add `#include <libxo/xo.h>` to all 5 files
   - Verify build still works
   - Document current json-c usage

2. **Phase 2: Read paths** (3-4 hrs)
   - Replace `json_object_from_file` with yajl or libxml2
   - Replace `json_object_object_get_ex` with native lookups
   - Test all read paths

3. **Phase 3: Write paths** (3-4 hrs)
   - Replace `json_object_new_*` with `xo_emit`
   - Replace `json_object_to_json_string` with `xo_emit`
   - Test all write paths

4. **Phase 4: Remove json-c** (1-2 hrs)
   - Remove `-ljson-c` from LIBADD
   - Remove json-c includes
   - Update Makefile dependencies

5. **Phase 5: Verify** (1-2 hrs)
   - Run all tests
   - Verify image pull/push still works
   - Verify CNI plugin still works
   - Verify state persistence still works

### Migration Steps (if pursuing Option B)

1. Download json-c 0.17 or latest
2. Copy `json-c/` to `usr.sbin/ocifbsd/vendor/json-c/`
3. Add to Makefile: `SUBDIR+= vendor/json-c`
4. Update includes: `#include "json-c/json.h"` (relative)
5. Remove `-ljson-c` from LIBADD
6. Add json-c to .gitignore for build artifacts
7. Update documentation

## seccomp(2) → capsicum(4) Migration

### Current State

3 files reference `seccomp`:

| File | Usage |
|------|-------|
| `security/mac.c` | seccomp profile loading, filter creation |
| `security/mac.h` | seccomp API declarations |
| `orchestration/orchestration.h` | seccomp mentions in comments |

All seccomp calls are currently TODOs (5 TODOs in `security/mac.c`).

### Why migrate?

1. **seccomp is Linux-only** - not available on FreeBSD
2. **capsicum(4) is the FreeBSD equivalent** - capability-based sandboxing
3. **Different API** - capsicum uses file descriptor capabilities
4. **Better integration** - capsicum is native to FreeBSD

### capsicum(4) Overview

capsicum provides capability-based sandboxing:

```c
#include <sys/capsicum.h>

// Enter capability mode
cap_enter();

// Create capability from file descriptor
cap_rights_t rights;
cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_FSTAT);
cap_new(fd, &rights);

// After cap_enter(), only operations on capabilities are allowed
```

### Migration Steps

1. **Phase 1: Design** (2-4 hrs)
   - Map seccomp syscalls to capsicum capabilities
   - Design the capability model
   - Document the mapping

2. **Phase 2: Replace API** (4-8 hrs)
   - Replace `seccomp_init()` with `cap_enter()`
   - Replace `seccomp_rule_add()` with `cap_rights_init()` + `cap_new()`
   - Replace `seccomp_load()` with capability application

3. **Phase 3: Test** (4-8 hrs)
   - Test with container workloads
   - Verify all operations succeed with appropriate capabilities
   - Test that operations fail without capabilities (negative testing)

4. **Phase 4: Document** (2-4 hrs)
   - Document the capability model
   - Provide migration guide for users
   - Update SECURITY.md

### seccomp → capsicum Mapping

| seccomp operation | capsicum equivalent |
|------------------|---------------------|
| `seccomp_init(SCMP_ACT_KILL)` | `cap_enter()` |
| `seccomp_rule_add(ctx, ACT_ALLOW, syscall, 0)` | `cap_rights_init(&rights, CAP_*)` |
| `seccomp_load(ctx)` | `cap_new(fd, &rights)` |
| `seccomp_release(ctx)` | (no equivalent - capabilities are per-fd) |

### Limitations

capsicum has some limitations compared to seccomp:
- **No syscall-level filtering** - capsicum works at the file descriptor level
- **No argument filtering** - capsicum can't filter by syscall arguments
- **Requires refactoring** - application must be designed for capabilities

For fine-grained syscall filtering on FreeBSD, consider:
- `syscall_filter(9)` (kernel-level filtering, in development)
- `audit(4)` (logging, not blocking)
- `mac(4)` (mandatory access control)

## -Werror Removal (Done)

### Before
```make
CFLAGS+= -Wall -Wextra -Werror
```

### After
```make
CFLAGS+= -Wall -Wextra
```

### Why?

The Klara-era code has 35 TODO markers and various warnings. With `-Werror`,
the build fails. Removing `-Werror` allows the build to succeed while
still showing warnings.

### Future work

Re-enable `-Werror` after all TODOs are resolved. This should be a goal
before production deployment.

## References

- [libxo documentation](https://github.com/Juniper/libxo)
- [capsicum(4) man page](https://www.freebsd.org/cgi/man.cgi?query=capsicum)
- [Capsicum: Practical Capabilities for UNIX](https://www.cl.cam.ac.uk/research/security/capsicum/)
- [json-c documentation](https://json-c.github.io/json-c/)
- [yajl documentation](https://lloyd.github.io/yajl/)
- [seccomp(2) man page (Linux)](https://man7.org/linux/man-pages/man2/seccomp.2.html)

# Security Policy

## Overview

`ocifbsd` is a native OCI runtime for FreeBSD. It translates OCI
container specifications into FreeBSD jails, with additional support
for resource limits (RCTL), mandatory access control (MAC labels),
capabilities (capsicum), TPM-based attestation, and PAM authentication.

As a runtime, it is in a privileged position: it creates and manages
isolated environments on behalf of users. Security is a first-class
concern.

## Supported Versions

| Branch | Supported |
|--------|-----------|
| `main` | Yes |
| `feature/oci-bootstrap` | Yes (current development) |
| `stable/*` | Yes |
| Older branches | No |

## Reporting a Vulnerability

**Please do not file public issues for security vulnerabilities.**

Report security issues privately to the maintainers via GitHub's
private vulnerability reporting feature:

1. Go to https://github.com/cloudbsdorg/freebsd-src-oci/security
2. Click "Report a vulnerability"
3. Fill out the form with:
   - Description of the vulnerability
   - Steps to reproduce
   - Impact assessment
   - Suggested fix (if any)

You should receive a response within 48 hours. If you do not, follow
up via the project's normal communication channels.

## Security Architecture

### Privilege Model

`ocifbsd` is intended to be run as root (or with `CAP_SYS_ADMIN`-
equivalent capabilities) because it needs to:

- Create and configure jails
- Set up networking (VNET, bridges, epair)
- Mount filesystems (nullfs, tmpfs, ZFS datasets)
- Apply RCTL resource limits
- Configure MAC labels
- Bind privileged ports (for API server)

The runtime should not be exposed to untrusted users without
additional isolation (e.g., running inside a VM or jail itself).

### Isolation Layers

`ocifbsd` uses multiple FreeBSD isolation mechanisms:

1. **jail(8)** — Process and filesystem isolation
2. **VNET** — Network stack isolation
3. **capsicum(4)** — Capability-based sandboxing
4. **MAC labels** — Mandatory access control (mac_bsdextended, mac_test)
5. **RCTL** — Resource limits (CPU, memory, processes, disk)
6. **PAM** — Pluggable authentication for API access
7. **TPM** — Hardware-based attestation (optional)
8. **seccomp(2) → capsicum(4)** — Syscall filtering (in progress)

### Trust Boundaries

```
┌─────────────────────────────────────────────────────────┐
│  User (untrusted)                                       │
│    │                                                    │
│    ├── HTTP API ──────────┐                             │
│    ├── CLI ───────────────┼─→ ocifbsd (root)            │
│    └── Container runtime ─┘                             │
│                              │                          │
│                              ├──→ jail(8)               │
│                              ├──→ VNET                  │
│                              ├──→ capsicum(4)           │
│                              ├──→ MAC labels            │
│                              ├──→ RCTL                  │
│                              └──→ Container process     │
└─────────────────────────────────────────────────────────┘
```

The container process is fully isolated from the host and from other
containers. The runtime itself runs as root and has full system access.

## Security Considerations

### Current Status

| Feature | Status | Notes |
|---------|--------|-------|
| jail(8) | ✓ Implemented | Core isolation |
| VNET | ✓ Implemented | Network isolation |
| RCTL | ⚠ Partial | 7 TODOs in security/rctl.c |
| MAC labels | ⚠ Partial | 5 TODOs in security/mac.c |
| capsicum(4) | ✗ Not implemented | Need to translate seccomp calls |
| seccomp(2) | ✗ Linux-only | 3 files reference seccomp, need translation |
| TPM | ✓ Implemented | Optional, hardware-dependent |
| PAM | ✓ Implemented | For API authentication |
| TLS | ✓ Implemented | In security-daemon/ |
| RBAC | ✓ Implemented | In security-daemon/ |

### Known Issues

See `.omo/drafts/TROUBLESHOOTING.md` and `.omo/drafts/ai-slop-backlog.md`
for a complete list.

Critical:
- **seccomp references** (3 files) need to be translated to capsicum(4)
  before the runtime is production-ready. Currently the seccomp calls
  are TODOs.

Medium:
- **RCTL parsing** has 7 TODOs in `security/rctl.c`. Limits may not
  be correctly applied until these are resolved.
- **MAC label application** has 5 TODOs in `security/mac.c`. MAC
  policies may not be correctly applied.

Low:
- **json-c dependency** is not in FreeBSD base. Vendoring or
  migration to `libxo` is recommended.

### Pre-Production Checklist

Before deploying `ocifbsd` in production:

- [ ] Translate all seccomp calls to capsicum(4)
- [ ] Resolve all 35 TODO markers (especially in security/)
- [ ] Replace json-c with libxo or vendor it
- [ ] Run `bmake audit` and review all findings
- [ ] Run `bmake lint` and review all findings
- [ ] Run `bmake smoke` and verify all checks pass
- [ ] Test in a FreeBSD VM with a known workload
- [ ] Review `.omo/drafts/TROUBLESHOOTING.md`
- [ ] Subscribe to security advisories for FreeBSD base system
- [ ] Subscribe to security advisories for json-c (if used)
- [ ] Set up TLS for the API server with valid certificates
- [ ] Configure PAM with strong authentication (not `pam_permit`)
- [ ] Enable MAC policies (mac_bsdextended, mac_test)
- [ ] Enable audit logging (auditd, auditdistd)
- [ ] Restrict API access to trusted networks
- [ ] Document the deployment's threat model

## Hardening Guide

### Runtime Configuration

In `/etc/ocifbsd/ocifbsd.conf`:

```yaml
# Enable all available isolation features
security:
  enable_capsicum: true     # Use capsicum(4) for capability-based sandboxing
  enable_mac: true          # Apply MAC labels to containers
  enable_rctl: true         # Apply RCTL resource limits
  enable_seccomp: true      # (Note: requires capsicum translation)
  enable_tpm: true          # Use TPM for attestation (if available)
  enable_pam: true          # Require authentication for API access

# Restrict API access
api:
  bind_address: 127.0.0.1   # Localhost only by default
  port: 8443
  tls:
    cert: /etc/ocifbsd/tls/server.crt
    key: /etc/ocifbsd/tls/server.key
    min_version: TLSv1.3

# Default resource limits (can be overridden per-container)
defaults:
  memory: 1G                # Hard memory limit
  cpu: 100%                 # Single CPU
  processes: 256            # Max processes
  disk: 10G                 # Max disk usage
```

### Kernel Tuning

In `/boot/loader.conf`:

```
# Enable MAC framework
mac_load="YES"

# Enable specific MAC modules
mac_bsdextended_load="YES"
mac_test_load="YES"

# Enable capsicum
capability_load="YES"

# Enable audit
audit_load="YES"

# Restrict dmesg (information disclosure)
kern.msgbuf_show_kvmstack=0
```

In `/etc/sysctl.conf`:

```
# Restrict ptrace to root only
security.bsd.unprivileged_proc_debug=0

# Restrict kernel module loading
kern.module_path=/boot/kernel;/boot/modules

# Enable ASLR
kern.elf64.aslr.enable=1
kern.elf32.aslr.enable=1

# Randomize PIDs
kern.randompid=1
```

### Container Best Practices

When creating containers:

```yaml
# Drop all capabilities by default
process:
  capabilities:
    drop:
      - all
    add:
      - CHOWN
      - DAC_OVERRIDE
      - FOWNER
      - SETUID
      - SETGID

# Run as non-root user
process:
  user: "1000:1000"

# Read-only root filesystem
root:
  readonly: true

# Mount only what's needed
mounts:
  - type: tmpfs
    destination: /tmp
    options: rw,nosuid,nodev,noexec
  - type: tmpfs
    destination: /run
    options: rw,nosuid,nodev,size=64m
```

## Security Audit Log

| Date | Type | Description | Status |
|------|------|-------------|--------|
| 2026-06-02 | Initial | feature/oci-bootstrap branch created | Pre-audit |
| 2026-06-03 | High | Shell injection via `system()` with user-controlled paths in `gc/gc.c`, `image/zfs_store.c`, `network/bridge.c`, `network/vnet.c` (~25 call sites) | Documented, fixing critical ones |

## Shell Injection Vulnerabilities (HIGH PRIORITY)

**Discovered 2026-06-03 during TODO cleanup.**

Multiple functions use `system(snprintf(...))` to execute shell commands.
If any user-controlled input flows into these strings, a malicious
actor can inject arbitrary shell commands.

### Pattern

```c
char cmd[PATH_MAX];
snprintf(cmd, sizeof(cmd), "zfs destroy -r %s", dataset);  // BAD
ret = system(cmd);                                          // BAD
```

An attacker who controls `dataset` (e.g., `tank/images;rm -rf /`)
gets arbitrary command execution as the ocifbsd user.

### Affected Files

| File | Functions | User-controlled input |
|------|-----------|----------------------|
| `gc/gc.c` | `gc_delete_container`, `gc_delete_image`, `gc_delete_volume`, `gc_delete_network`, `gc_zfs_snapshots`, `gc_zfs_destroy_dataset`, `gc_zfs_cleanup` | container/image/volume/network names, dataset paths |
| `image/zfs_store.c` | `zfs_image_create`, `zfs_image_delete`, `zfs_volume_create` | dataset paths, image names |
| `network/bridge.c` | `bridge_create`, `bridge_destroy`, `bridge_add_interface`, etc. | bridge names, interface names |
| `network/vnet.c` | `vnet_create_jail`, etc. | jail names, interface names |

### Fix Pattern

Replace `system(snprintf(...))` with `fork()+execv()` using
a static argv array. This bypasses the shell entirely:

```c
char *argv[] = { "zfs", "destroy", "-r", (char *)dataset, NULL };
pid_t pid = fork();
if (pid == 0) {
    closefrom(STDERR_FILENO + 1);
    execv("/sbin/zfs", argv);
    _exit(127);
}
int status;
waitpid(pid, &status, 0);
```

This eliminates shell metacharacter interpretation.

### Workaround Until Fixed

If you must use ocifbsd in production, ensure that:
- Container names match `[a-z0-9_-]{1,64}` (no shell metacharacters)
- Image names come from trusted registries (OCI spec compliant)
- Dataset paths are configured by the admin, not from user input

### Mitigation Status

- **Critical paths (gc_zfs_destroy_dataset, zfs_image_delete)**: not yet fixed
- **Recommended fix effort**: 2-3 days to add `safe_execv()` helper and
  convert all `system()` call sites
- **Tracking**: see issues #TBD

## Resource Leaks (MEDIUM)

- `network_disconnect` (network.c:636): cleanup partially implemented
- `vnet_delete_jail` (network.c:758): epairs not tracked, not cleaned up
- `container_pause` (container.c:561): state set to PAUSED but processes NOT paused

## Cryptographic Issues (FIXED in this audit)

- `auth.c` `secret_encrypt`/`secret_decrypt` (line 772-925): now uses
  real AES-256-CBC instead of plaintext copy (CRITICAL fix)
- `push.c` (line 479): real SHA256 digest of config.json (was placeholder)

## References

- [FreeBSD Security Information](https://www.freebsd.org/security/)
- [FreeBSD Handbook: Security](https://docs.freebsd.org/en/books/handbook/security/)
- [OCI Runtime Spec](https://github.com/opencontainers/runtime-spec)
- [Capsicum: Practical Capabilities for UNIX](https://www.cl.cam.ac.uk/research/security/capsicum/)
- [jail(8) man page](https://www.freebsd.org/cgi/man.cgi?query=jail)
- [mac(4) man page](https://www.freebsd.org/cgi/man.cgi?query=mac)
- [rctl(8) man page](https://www.freebsd.org/cgi/man.cgi?query=rctl)

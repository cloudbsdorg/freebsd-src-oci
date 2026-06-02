# ocifbsd FAQ

Frequently asked questions about `ocifbsd`, the native OCI runtime for FreeBSD.

## General

### What is ocifbsd?

`ocifbsd` is a native Open Container Initiative (OCI) runtime for FreeBSD. It
translates OCI container specifications into FreeBSD jails, providing
Docker/Kubernetes-compatible container functionality using FreeBSD's native
isolation primitives (jail, VNET, capsicum, RCTL, MAC labels).

### How is it different from Docker on FreeBSD?

Docker on FreeBSD uses a Linux compatibility layer (via linprocfs, linsysfs) to
run Linux containers. `ocifbsd` runs native FreeBSD containers without Linux
compatibility, using FreeBSD's native primitives for better performance,
security, and integration with the FreeBSD ecosystem.

### How is it different from podman on FreeBSD?

Podman uses the same Linux compatibility approach as Docker. `ocifbsd` is a
native runtime designed specifically for FreeBSD.

### What OCI spec version does it support?

The codebase targets OCI Runtime Spec 1.0+ with extensions for:
- OCI Hooks (prestart, poststart, poststop)
- OCI Mounts (tmpfs, bind, ZFS datasets)
- OCI Process (capabilities, namespaces, rlimits)
- OCI Linux (limited - uses FreeBSD equivalents where possible)

## Build & Install

### How do I build ocifbsd?

On a FreeBSD system:
```bash
cd /usr/src
git clone https://github.com/cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
bmake -C usr.sbin/ocifbsd
bmake -C usr.sbin/ocifbsd install
```

On macOS (cross-build):
```bash
cd freebsd-src-oci
bmake -C usr.sbin/ocifbsd darwin-bootstrap
bmake -C usr.sbin/ocifbsd darwin-build
```

See `.omo/drafts/CONTRIBUTING.md` for the full build guide.

### Why does the Makefile work on macOS?

The Makefile uses conditional `.include <src.opts.mk>` and `.include <bsd.prog.mk>`
that are skipped on macOS hosts (where those files don't exist). The custom
inspection targets (`audit`, `lint`, `smoke`, etc.) all work on macOS.

### What are the build dependencies?

On macOS:
- `bmake` (Homebrew)
- `llvm` (Homebrew, provides clang)
- `lld` (Homebrew, separate package since LLVM 19+)
- `python3` (Homebrew)

On FreeBSD:
- Base system (clang, lld, bmake are all in base)

### How long does the cross-build take?

The full cross-build (`darwin-build`) takes 5-15 minutes on a modern Mac,
depending on:
- Number of CPU cores
- Whether you've built before (incremental vs clean)
- Network speed (for downloading dependencies)

For a clean build: ~15 minutes
For an incremental rebuild: ~2-5 minutes

## Testing

### How do I run the tests?

On FreeBSD:
```bash
cd /usr/src/usr.sbin/ocifbsd
bmake
scp ocifbsd root@freebsd-test:/root/
ssh root@freebsd-test "cd /usr/tests/usr.sbin/ocifbsd && kyua test -k Kyuafile"
```

Or with the Makefile target:
```bash
bmake -C usr.sbin/ocifbsd darwin-test
```

See `.omo/drafts/vm-provisioning.md` for VM setup.

### What's the test coverage?

The test suite is in `tests/usr.sbin/ocifbsd/`. Current tests:
- `ocifbsd_test.c` - C unit tests for core functions
- `ocifbsd_test.sh` - Shell-based integration tests
- ATF-style test cases for the runtime

The test suite is a work in progress. See the AI slop backlog for details.

## Architecture

### How does ocifbsd use FreeBSD jails?

`ocifbsd` creates a jail for each OCI container using `jail_set(2)` (or
`jail_create(2)` on older systems). The jail parameters are derived from the
OCI spec:

| OCI spec | FreeBSD equivalent |
|----------|-------------------|
| `process.capabilities` | `jail.caps` (limited) |
| `linux.resources.memory` | `rctl` memory limits |
| `linux.resources.cpu` | `rctl` CPU limits |
| `process.user` | `jail.jail_user` |
| `root.path` | `jail.path` |
| `mounts` | nullfs/tmpfs mounts in jail |
| `hostname` | `jail.hostname` |

### How does ocifbsd handle networking?

`ocifbsd` supports three networking modes:

1. **Bridge mode** - Creates a bridge interface, attaches container VNET to it
2. **Host mode** - Container shares host network stack (no isolation)
3. **CNI mode** - Delegates networking to CNI plugins

CNI is the most flexible and is recommended for Kubernetes deployments.

### What's the difference between RCTL and cgroups?

RCTL (Resource Limits) is FreeBSD's equivalent of Linux cgroups. Both provide
hierarchical resource limits, but with different APIs and capabilities.

RCTL advantages:
- Native FreeBSD integration
- Hierarchical (jail-in-jail limits work)
- No additional kernel modules needed

cgroups advantages:
- More mature ecosystem
- Better tooling support
- More granular controls (e.g., blkio)

`ocifbsd` uses RCTL natively, with limited support for translating cgroup
configs from OCI specs.

### Does ocifbsd support Kubernetes?

`ocifbsd` provides a CRI-compatible shim (in `clustering/`) that allows
Kubernetes to use `ocifbsd` as the container runtime. The shim is a work in
progress.

For full Kubernetes support, you also need:
- A CNI plugin (e.g., Flannel, Calico)
- A CSI driver (for persistent volumes)
- kube-proxy or equivalent

## Security

### Is ocifbsd secure for production?

**Not yet.** The codebase has several known issues:
- 35 TODO markers (some in security-critical code)
- 3 seccomp references (need capsicum translation)
- 5 json-c references (not in FreeBSD base)

See `.omo/drafts/SECURITY.md` for the full pre-production checklist.

### How does ocifbsd isolate containers?

`ocifbsd` uses multiple FreeBSD isolation mechanisms:
1. **jail(8)** - Process and filesystem isolation
2. **VNET** - Network stack isolation
3. **capsicum(4)** - Capability-based sandboxing (planned, not implemented)
4. **MAC labels** - Mandatory access control
5. **RCTL** - Resource limits

The combination provides defense in depth.

### What about seccomp?

`seccomp(2)` is Linux-specific. `ocifbsd` plans to use `capsicum(4)` as the
FreeBSD equivalent. The translation is in progress (see the AI slop backlog).

### Can unprivileged users run containers?

Currently no. `ocifbsd` runs as root because it needs to create jails, configure
networking, and mount filesystems.

Future work may allow unprivileged container creation using:
- `jail_getid(2)` and `jail_setid(2)` (if available)
- A setuid wrapper with capability restrictions
- A separate daemon that handles privileged operations

## Development

### How do I contribute?

See `.omo/drafts/CONTRIBUTING.md` for the full contributor guide. Quick start:

```bash
git checkout main
git pull
git checkout -b feature/my-change
# make changes
bmake -C usr.sbin/ocifbsd prepare-pr  # pre-PR check
git push origin feature/my-change
```

### What's the code style?

FreeBSD's `style(9)`. Key points:
- 4-space tabs
- BSD KNF brace style
- `lower_snake_case` for functions and variables
- `UPPER_SNAKE_CASE` for constants
- One statement per line

### How do I run the inspection targets?

```bash
bmake -C usr.sbin/ocifbsd audit      # AI slop markers
bmake -C usr.sbin/ocifbsd lint       # code quality
bmake -C usr.sbin/ocifbsd smoke      # syntax + license
bmake -C usr.sbin/ocifbsd all-checks # all of the above
```

### What's the AI slop?

The codebase has 35 TODO markers and 1 HACK marker. These are tracked in
`.omo/drafts/ai-slop-backlog.md`. The largest hotspots are:
- `network/network.c` (9 TODOs)
- `security/rctl.c` (7 TODOs)
- `security/mac.c` (5 TODOs)

### How do I find documentation?

- `.plan/` - Master documentation (000.0-OCI-Jail-TOC.md is the entry point)
- `.omo/drafts/` - Work-in-progress drafts (interview notes, guides)
- `.omo/evidence/` - Per-task evidence files
- `usr.sbin/ocifbsd/README.md` - Source-level documentation

### Where can I get help?

- GitHub Issues: https://github.com/cloudbsdorg/freebsd-src-oci/issues
- GitHub Discussions: https://github.com/cloudbsdorg/freebsd-src-oci/discussions
- FreeBSD Forums: https://forums.freebsd.org/

## Troubleshooting

### The cross-build fails with "lld not found"

Install lld separately (since LLVM 19+):
```bash
brew install lld
```

### The cross-build fails with "macOS SDK header leak"

Build in a FreeBSD VM instead. See `.omo/drafts/vm-provisioning.md`.

### The Makefile fails with "Could not find src.opts.mk"

This is expected on macOS. The Makefile uses conditional `.include` so it
should work on both FreeBSD and macOS. If you see this error, you may be
using an older version. Pull the latest from `feature/oci-bootstrap`.

### The bootstrap script fails to find brew

If you don't have Homebrew installed, the script will offer to install it.
You can also install it manually: https://brew.sh

### Tests fail with "permission denied"

Make sure the test binaries are executable:
```bash
chmod +x /root/ocifbsd /root/ocifbsd_test
```

### VM is not reachable

```bash
bmake -C usr.sbin/ocifbsd vm-status
```

If the VM is not reachable, see `.omo/drafts/vm-provisioning.md` for setup.

See `.omo/drafts/TROUBLESHOOTING.md` for a comprehensive list of issues.

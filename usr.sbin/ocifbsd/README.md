# ocifbsd - FreeBSD Native OCI Runtime

`ocifbsd` is a native Open Container Initiative (OCI) runtime for FreeBSD, designed to provide Docker/Kubernetes-compatible container functionality using FreeBSD's native jail(8) technology.

## Features

- **OCI Runtime Compliance**: Runs OCI-compliant containers on FreeBSD
- **FreeBSD Native**: Uses jail(8), VNET, RCTL, ZFS, and other FreeBSD technologies
- **Kubernetes Ready**: Supports pods, stacks, services, and rolling updates
- **Image Management**: Pull/push images from OCI and Docker registries
- **Networking**: Bridge, VNET, CNI plugin support
- **Resource Limits**: Memory, CPU, process limits via RCTL
- **Security**: MAC labels, RBAC, secrets
- **Clustering**: Multi-node support with a gossip protocol and full Raft
  consensus (leader election, log replication, persistence, membership
  changes, and log compaction/snapshots), exposed via the `ocifbsd-cluster(8)`
  daemon
- **Certificates**: Native ACME (RFC 8555) client with ES256/JWS, HTTP-01
  challenges, rotation and backup — built on base OpenSSL 3
- **Config Conversion**: Convert Kubernetes YAML and Docker Compose to native format

## Quick Start

### Build (FreeBSD native — tier-1, the primary build path)

```bash
# On a FreeBSD host with /usr/src and /usr/obj available
cd /usr/src/usr.sbin/ocifbsd    # if you have the source tree
# OR
cd usr.sbin/ocifbsd            # if you have a standalone clone
make
```

That's it. The default `make` target builds the main `ocifbsd`
binary plus all 6 active SUBDIRs. No environment variables
required, no cross-toolchain, no sysroot. This is the build
path that produces the binary that actually runs on FreeBSD.

**Note**: 9 of the 15 SUBDIRs (cert, export, gc, image, logd,
network, orchestration, pam, security) are currently commented
out in the Makefile as deferred AI-slop refactor work. They
build clean, link, and run only after their respective
refactor PRs land. See `OCI-STATUS.md` §5 for per-SUBDIR
status.

**Note**: On FreeBSD, `/usr/bin/make` IS bmake (BSD make has been the
base `make` on FreeBSD since FreeBSD 9). You do not need to install
anything. If you see references to `bmake` in older docs, that means
the same thing on FreeBSD.

```bash
sudo make install              # installs /usr/sbin/ocifbsd
sudo make install-man          # installs /usr/share/man/man8/ocifbsd.8
```

### Run a Container

```bash
# Pull an image
ocifbsd image pull localhost/freebsd:latest

# Create a container
ocifbsd create --name my-container localhost/freebsd:latest /bin/sh

# Start the container
ocifbsd start my-container

# Attach to the container
ocifbsd attach my-container

# Stop the container
ocifbsd stop my-container
```

### Run a Pod

```bash
# Create a pod
ocifbsd pod create --name my-pod --replicas 2

# List pods
ocifbsd pod list

# Get pod logs
ocifbsd pod logs my-pod

# Scale the pod
ocifbsd pod scale my-pod --replicas 5
```

### Convert Kubernetes Config

```bash
# Convert a Kubernetes Deployment
ocifbsd-convert deployment.yaml -f simple > output.yaml

# Convert Docker Compose
ocifbsd-convert docker-compose.yml --format native > output.yaml
```

## Architecture

All SUBDIRs are active and build clean on FreeBSD 16 (BOOTSTRAP 100% complete):

```
ocifbsd
├── src/               # Core runtime (container lifecycle, hooks, OCI translation)
├── api/               # REST API server
├── cert/              # Certificate management (ACME/RFC 8555, rotation, backup)
├── clustering/        # Clustering (gossip + full Raft consensus)
├── convert/           # Config conversion (K8s, Compose)
├── export/            # Cloud export scaffolding (AWS, GCP, Azure)
├── gc/                # Garbage collection
├── image/             # Image management (pull, push, unpack, ZFS storage)
├── logd/              # Logging daemon (+ remote HTTP forwarding)
├── metrics/           # Metrics collection
├── namespace/         # Namespace isolation
├── network/           # Networking (bridge, VNET, CNI)
├── orchestration/     # Orchestration (pods, stacks, services, health checks)
├── pam/               # PAM authentication
├── security/          # Security (RCTL, MAC labels)
├── security-daemon/   # Security daemon (RBAC, secrets, TLS)
└── contrib/json-c/    # Vendored json-c 0.18 (MIT) — built in-tree, no port
```

**Self-contained build:** the JSON dependency (json-c) is vendored under
`contrib/json-c` and built as a private static library, so the tree builds
without the json-c port. The toolchain is entirely FreeBSD base clang/lld +
bmake, and there is **no GPL/copyleft** dependency anywhere (see
[LICENSING-DEPS.md](LICENSING-DEPS.md)).

## Configuration

Default config: `/etc/ocifbsd/ocifbsd.conf`

```yaml
runtime:
  root_dir: /var/run/ocifbsd
  state_dir: /var/db/ocifbsd
  log_level: info

image:
  registry: localhost:5000
  storage: zfs:tank/ocifbsd/images

network:
  driver: bridge
  cni_path: /usr/local/lib/cni

cluster:
  enabled: false
  bind_address: 0.0.0.0
  port: 8080
```

## CLI Commands

### Container Commands

| Command | Description |
|---------|-------------|
| `ocifbsd create` | Create a container |
| `ocifbsd start` | Start a container |
| `ocifbsd stop` | Stop a container |
| `ocifbsd kill` | Kill a container |
| `ocifbsd delete` | Delete a container |
| `ocifbsd pause` | Pause a container |
| `ocifbsd resume` | Resume a container |
| `ocifbsd list` | List containers |
| `ocifbsd inspect` | Inspect a container |
| `ocifbsd logs` | Get container logs |
| `ocifbsd exec` | Execute command in container |
| `ocifbsd attach` | Attach to container |

### Image Commands

| Command | Description |
|---------|-------------|
| `ocifbsd image pull` | Pull an image |
| `ocifbsd image push` | Push an image |
| `ocifbsd image list` | List images |
| `ocifbsd image inspect` | Inspect an image |
| `ocifbsd image rm` | Remove an image |

### Pod/Service Commands

| Command | Description |
|---------|-------------|
| `ocifbsd pod create` | Create a pod |
| `ocifbsd pod list` | List pods |
| `ocifbsd pod logs` | Get pod logs |
| `ocifbsd pod scale` | Scale a pod |
| `ocifbsd pod update` | Update a pod |
| `ocifbsd service create` | Create a service |
| `ocifbsd stack deploy` | Deploy a stack |
| `ocifbsd stack list` | List stacks |

### Cluster Commands

| Command | Description |
|---------|-------------|
| `ocifbsd cluster init` | Initialize a cluster |
| `ocifbsd cluster join` | Join a cluster |
| `ocifbsd cluster leave` | Leave a cluster |
| `ocifbsd cluster status` | Show cluster status |
| `ocifbsd node list` | List cluster nodes |
| `ocifbsd-cluster` | Run the Raft cluster daemon (see `ocifbsd-cluster(8)`) |

### Config Conversion

| Command | Description |
|---------|-------------|
| `ocifbsd-convert` | Convert K8s/Compose to native format |

## Documentation

See the `.plan/` directory for comprehensive documentation:

- [000.0-OCI-Jail-TOC.md](.plan/000.0-OCI-Jail-TOC.md) - Master Table of Contents
- [001.0-Overview.md](.plan/001.0-Overview.md) - Project overview
- [002.0-CLI-Spec.md](.plan/002.0-CLI-Spec.md) - Complete CLI specification
- [006.0-Orchestration.md](.plan/006.0-Orchestration.md) - Pod and service management
- [007.0-Config-Conversion.md](.plan/007.0-Config-Conversion.md) - Config conversion
- [A.0-Glossary-and-Appendix.md](.plan/A.0-Glossary-and-Appendix.md) - Technical glossary

## Testing

```bash
# Build and run tests
cd tests/usr.sbin/ocifbsd
make
make test
```

## License

This software is provided under the BSD 2-Clause License. See the source file headers for details.

## Building from macOS or Linux (cross-build — opt-in)

`ocifbsd` is a FreeBSD-native runtime. It uses `jail(2)`, `capsicum(4)`,
`rctl(8)`, `zfs(8)`, and other FreeBSD-specific syscalls that do not exist
on macOS or Linux. **You cannot build `ocifbsd` as a native binary on
macOS or Linux** — the C code will not compile.

If you are on a macOS or Linux host and want to cross-build FreeBSD
binaries from there, see [`tools/cross-build/README.md`](tools/cross-build/README.md).
This is an opt-in helper for developers on non-FreeBSD workstations; it is
NOT the primary build path and it produces binaries that will not run on
macOS/Linux — they must be deployed to a FreeBSD host to run.

The short version:

```sh
# On macOS, with Homebrew installed:
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
git checkout feature/oci-bootstrap
./tools/cross-build/macos.sh --install --yes   # installs bmake, llvm, lld
. /tmp/ocifbsd-cross-build-env
bmake -C usr.sbin/ocifbsd cross-build
```

The resulting `usr.sbin/ocifbsd/ocifbsd` is a FreeBSD amd64 binary. Deploy
it with `scp` to a FreeBSD host.

If you are doing a release build, do it on FreeBSD native. The cross-build
path is for development iteration only.

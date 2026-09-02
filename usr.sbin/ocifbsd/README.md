# ocifbsd - FreeBSD Native OCI Runtime

`ocifbsd` is a native Open Container Initiative (OCI) runtime for FreeBSD, designed to provide OCI-compatible container functionality using FreeBSD's native jail(8) technology.

## Features

Status reflects what is tested and enforced today versus what is still
scaffold. The runtime core (create → run → networking → limits → security)
is implemented and covered by ATF/kyua tests on FreeBSD; the higher layers
build but are not yet production-complete.

**Working and tested**

- **OCI runtime lifecycle**: `create`, `start`, `state`, `kill`, `delete`,
  `run`, `exec`, `stop`, `pause`/`resume`, `list`, `inspect` — jail-backed,
  with an OCI-conformant `state` output. See [OCI conformance](docs/OCI-CONFORMANCE.md).
- **FreeBSD native**: jail(8), VNET, RCTL, MAC, nullfs, ZFS — no Linux
  emulation.
- **Image management**: pull/push/load/images/rmi against OCI registries;
  layers verified against manifest digests and unpacked to a ZFS-backed store.
- **Networking (VNET)**: per-container network stack — an epair is moved into
  the jail, the container IPv4/gateway configured, and an optional host
  bridge attached; interfaces are cleaned up on delete.
- **Resource limits**: `linux.resources` (memory, cpu, pids) mapped to
  rctl(8) and applied/cleaned across the lifecycle (needs
  `kern.racct.enable=1`).
- **Security**: read-only / nosuid root, `process.noNewPrivileges`, RCTL, and
  MAC labels (`freebsd.macLabel` applied to the init via mac_set_proc).
- **Config conversion**: convert Ensemble manifests (declarative resources and
  multi-service stacks) to the native format.

**Scaffold / experimental** (builds, not production-complete — see
[.plan/005.0](../../.plan/005.0-Risks-TODO.md))

- Orchestration (pods, stacks, services, rolling updates)
- Clustering (gossip + Raft), certificates (ACME), REST API, PAM, RBAC/secrets
- CNI plugin interface, cloud export

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
binary plus all SUBDIRs and the two vendored libraries. No
environment variables required, no cross-toolchain, no sysroot,
and no ports. This is the build path that produces the binary
that actually runs on FreeBSD.

**Note**: On FreeBSD, `/usr/bin/make` IS bmake (BSD make has been the
base `make` on FreeBSD since FreeBSD 9). You do not need to install
anything. If you see references to `bmake` in older docs, that means
the same thing on FreeBSD.

```bash
sudo make install              # installs /usr/sbin/ocifbsd
sudo make install-man          # installs /usr/share/man/man8/ocifbsd.8
```

### Run a container

`create` takes either an OCI image reference (`--image`) or a path to an OCI
bundle directory (one containing `config.json` and `rootfs/`). It prints the
container id.

```bash
# From a pulled image:
ocifbsd pull docker.io/library/hello-world:latest
cid=$(ocifbsd run --name demo --image docker.io/library/hello-world:latest)

# ...or from a local OCI bundle directory:
cid=$(ocifbsd create --name demo ./mybundle)
ocifbsd start "$cid"

ocifbsd state "$cid"          # OCI state JSON (pretty by default; -c for one line)
ocifbsd exec "$cid" /bin/sh -c 'echo hi'
ocifbsd stop "$cid"           # SIGTERM, then SIGKILL after a timeout
ocifbsd delete --force "$cid"
```

### Networking, limits, and MAC (via the bundle config.json)

These are opt-in `config.json` fields; an absent field leaves the default
open. See the CONFIGURATION EXTENSIONS section of `ocifbsd(8)`.

```json
{
  "ociVersion": "1.0.2",
  "process": { "args": ["/bin/sh"], "cwd": "/" },
  "root": { "path": "rootfs", "readonly": true },
  "linux": { "resources": {
    "memory": { "limit": 134217728 },
    "cpu": { "quota": 50000, "period": 100000 }
  } },
  "freebsd": {
    "vnet": true,
    "ip4": ["192.0.2.20/24"],
    "defaultGateway4": ["192.0.2.1"],
    "bridge": "ocibr0",
    "macLabel": "biba/high"
  }
}
```

Starting a container from this bundle gives it a read-only root, a 128&nbsp;MiB
memory cap and a 50%-of-one-core CPU cap via rctl(8), its own VNET interface on
`ocibr0` at `192.0.2.20/24`, and a Biba MAC label on its init (when a labeling
policy is loaded). Resource limits require `kern.racct.enable=1`.

### Convert an Ensemble config

```bash
# Declarative manifest or a multi-service stack -> native config:
ocifbsd_convert deployment.yaml > output.yaml
ocifbsd_convert app.stack.yml   > output.yaml
```

## Architecture

All SUBDIRs are active and build clean on FreeBSD 16 (BOOTSTRAP 100% complete):

```
ocifbsd
├── src/               # Core runtime (container lifecycle, hooks, OCI translation)
├── api/               # REST API server
├── cert/              # Certificate management (ACME/RFC 8555, rotation, backup)
├── clustering/        # Clustering (gossip + Raft) — scaffold
├── convert/           # Config conversion (Ensemble manifests)
├── export/            # Cloud export scaffolding (AWS, GCP, Azure)
├── gc/                # Garbage collection
├── image/             # Image management (pull, push, unpack, ZFS storage)
├── logd/              # Logging daemon (+ remote HTTP forwarding)
├── metrics/           # Metrics collection
├── namespace/         # Namespace isolation
├── network/           # Networking (bridge, VNET, CNI)
├── orchestration/     # Orchestration (pods, stacks, services) — scaffold
├── pam/               # PAM authentication
├── security/          # Security (RCTL, MAC labels)
├── security-daemon/   # Security daemon (RBAC, secrets, TLS)
├── contrib/json-c/    # Vendored json-c 0.18 (MIT) — built in-tree, no port
└── contrib/curl/      # Vendored libcurl 8.21.0 (curl license) — built in-tree, no port
```

**Self-contained build — no ports required.** Both third-party dependencies,
json-c and libcurl, are vendored under `contrib/` and built as private static
libraries, so the whole tree builds with **zero ports**. libcurl is configured
minimally (HTTP/HTTPS only, TLS via FreeBSD **base** OpenSSL, every other
backend disabled). The toolchain is entirely FreeBSD base clang/lld + bmake,
and there is **no GPL/copyleft** dependency anywhere. `ldd` on the resulting
`ocifbsd` shows only base libraries. See [LICENSING-DEPS.md](LICENSING-DEPS.md).

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
| `ocifbsd-convert` | Convert Ensemble manifests to native format |

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

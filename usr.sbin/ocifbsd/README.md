# ocifbsd - FreeBSD Native OCI Runtime

`ocifbsd` is a native Open Container Initiative (OCI) runtime for FreeBSD, designed to provide Docker/Kubernetes-compatible container functionality using FreeBSD's native jail(8) technology.

## Features

- **OCI Runtime Compliance**: Runs OCI-compliant containers on FreeBSD
- **FreeBSD Native**: Uses jail(8), VNET, RCTL, ZFS, and other FreeBSD technologies
- **Kubernetes Ready**: Supports pods, stacks, services, and rolling updates
- **Image Management**: Pull/push images from OCI and Docker registries
- **Networking**: Bridge, VNET, CNI plugin support
- **Resource Limits**: Memory, CPU, process limits via RCTL
- **Security**: MAC labels, RBAC, secrets, TPM support (optional)
- **Clustering**: Multi-node support with gossip protocol and Raft consensus
- **Config Conversion**: Convert Kubernetes YAML and Docker Compose to native format

## Quick Start

### Build

```bash
cd usr.sbin/ocifbsd
make
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

```
ocifbsd
├── src/               # Core runtime (container lifecycle, hooks, OCI translation)
├── image/             # Image management (pull, push, unpack, ZFS storage)
├── network/           # Networking (bridge, VNET, CNI)
├── security/          # Security (RCTL, MAC labels)
├── orchestration/     # Orchestration (pods, stacks, services, health checks)
├── convert/           # Config conversion (K8s, Docker Compose)
├── clustering/        # Clustering (gossip, Raft, service discovery)
├── namespace/         # Namespace isolation
├── metrics/           # Metrics collection
├── security-daemon/   # Security daemon (RBAC, secrets, TLS)
├── tpm/               # TPM support (optional)
├── pam/               # PAM authentication
├── api/               # REST API server
├── gc/                # Garbage collection
├── cert/              # Certificate management
├── logd/              # Logging daemon
└── export/            # Cloud export (AWS, GCP, Azure)
```

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

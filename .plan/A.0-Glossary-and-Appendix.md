## Table of Contents

- [A. Glossary and Technical Appendix](#a-glossary-and-technical-appendix)
  - [A.1 Networking Terms](#a1-networking-terms)
    - [A.1.1 VXLAN](#a11-vxlan)
    - [A.1.2 VNET](#a12-vnet)
    - [A.1.3 ARP and NDP](#a13-arp-and-ndp)
    - [A.1.4 BGP](#a14-bgp)
    - [A.1.5 CNI](#a15-cni)
    - [A.1.6 Load Balancing Algorithms](#a16-load-balancing-algorithms)
  - [A.2 Storage Terms](#a2-storage-terms)
    - [A.2.1 ZFS](#a21-zfs)
    - [A.2.2 ZFS Send and Receive](#a22-zfs-send-and-receive)
    - [A.2.3 Copy-on-Write](#a23-copy-on-write)
    - [A.2.4 Deduplication](#a24-deduplication)
  - [A.3 Security Terms](#a3-security-terms)
    - [A.3.1 mTLS](#a31-mtls)
    - [A.3.2 Certificate Authority](#a32-certificate-authority)
    - [A.3.3 CSR](#a33-csr)
    - [A.3.4 RBAC](#a34-rbac)
    - [A.3.5 MAC Labels](#a35-mac-labels)
    - [A.3.6 RCTL](#a36-rctl)
    - [A.3.7 TLS 1.3](#a37-tls-13)
    - [A.3.8 AES-256-GCM](#a38-aes-256-gcm)
  - [A.4 Container and Orchestration Terms](#a4-container-and-orchestration-terms)
    - [A.4.1 OCI](#a41-oci)
    - [A.4.2 Jail](#a42-jail)
    - [A.4.3 Pod](#a43-pod)
    - [A.4.4 Namespace](#a44-namespace)
    - [A.4.5 Cgroup](#a45-cgroup)
    - [A.4.6 Raft](#a46-raft)
    - [A.4.7 Gossip Protocol](#a47-gossip-protocol)
    - [A.4.8 SWIM](#a48-swim)
  - [A.5 FreeBSD-Specific Terms](#a5-freebsd-specific-terms)
    - [A.5.1 VNET Jail](#a51-vnet-jail)
    - [A.5.2 epair](#a52-epair)
    - [A.5.3 pf](#a53-pf)
    - [A.5.4 CARP](#a54-carp)
    - [A.5.5 Jails vs Linux Containers](#a55-jails-vs-linux-containers)
  - [A.6 Hardware and Virtualization Terms](#a6-hardware-and-virtualization-terms)
    - [A.6.1 HSM](#a61-hsm)
    - [A.6.3 SR-IOV](#a63-sr-iov)
    - [A.6.4 DPDK](#a64-dpdk)
  - [A.7 Protocol and Data Format Terms](#a7-protocol-and-data-format-terms)
    - [A.7.1 JSON](#a71-json)
    - [A.7.2 YAML](#a72-yaml)
    - [A.7.3 Protobuf](#a73-protobuf)
    - [A.7.4 JWT](#a74-jwt)
  - [A.8 Performance and Scalability Terms](#a8-performance-and-scalability-terms)
    - [A.8.1 Eventual Consistency](#a81-eventual-consistency)
    - [A.8.2 Strong Consistency](#a82-strong-consistency)
    - [A.8.3 Quorum](#a83-quorum)
    - [A.8.4 Backpressure](#a84-backpressure)
    - [A.8.5 Connection Draining](#a85-connection-draining)
    - [A.8.6 ACME](#a86-acme)
    - [A.8.7 SCEP](#a87-scep)
    - [A.8.8 EST](#a88-est)
    - [A.8.9 .feb](#a89-feb)

---

# A. Glossary and Technical Appendix

**Author:** Mark LaPointe <mark@cloudbsd.org>
**Date:** 2026-04-29
**Repository:** https://github.com/cloudbsdorg/freebsd-src-oci
**Branch:** `oci-plan`

---

## A.1 Networking Terms

### A.1.1 VXLAN

**Virtual Extensible LAN (VXLAN)** is a network virtualization technology that encapsulates Layer 2 Ethernet frames within Layer 3 UDP packets, allowing virtual networks to span across physical network boundaries.

**Why it matters:** In a container cluster, pods run on different physical hosts. VXLAN creates a single virtual network (overlay) that makes all pods appear to be on the same local network, even when they are on different machines.

**How it works:**
```
Pod A (10.0.1.5) on Host 1
    │
    ▼
Ethernet Frame (src: 10.0.1.5, dst: 10.0.2.3)
    │
    ▼
VXLAN Encapsulation
┌─────────────────────────────────────────┐
│  Outer UDP Header                        │
│  src: 192.168.1.10 (Host 1 IP)          │
│  dst: 192.168.1.20 (Host 2 IP)          │
│  port: 4789 (VXLAN standard)             │
├─────────────────────────────────────────┤
│  VXLAN Header                            │
│  VNI: 100 (Virtual Network Identifier)   │
├─────────────────────────────────────────┤
│  Inner Ethernet Frame                    │
│  (original pod-to-pod traffic)            │
└─────────────────────────────────────────┘
    │
    ▼
Physical Network (Layer 3)
    │
    ▼
Host 2 receives, decapsulates
    │
    ▼
Pod B (10.0.2.3) receives original frame
```

**Key properties:**
- **VNI (VXLAN Network Identifier):** A 24-bit field allowing 16 million isolated virtual networks. In our system, VNI = namespace ID.
- **Encapsulation overhead:** 50 bytes per packet (8 VXLAN + 8 UDP + 20 IP + 14 Ethernet).
- **Multicast vs unicast:** We use unicast with a distributed control plane (gossip) rather than multicast, which many data centers block.
- **Performance:** Modern NICs support VXLAN offloading (checksum, segmentation), reducing CPU overhead.

**Comparison to alternatives:**
| Technology | Encapsulation | Use Case | Complexity |
|-----------|-------------|----------|------------|
| VXLAN | UDP | General purpose overlay | Medium |
| GRE | IP | Simple tunneling | Low |
| Geneve | UDP | Extensible metadata | Medium |
| STT | TCP | High performance | High |
| IP-in-IP | IP | Minimal overhead | Low |

**In our system:** VXLAN is used for the pod overlay network. Each namespace gets its own VNI. The `vxlan0` interface on each host bridges local pod VNET interfaces into the overlay.

---

### A.1.2 VNET

**VNET** is FreeBSD's network stack virtualization feature. It allows multiple independent, fully functional network stacks to coexist on a single kernel.

**Why it matters:** Without VNET, all jails share the host's network stack. With VNET, each jail (or pod) gets its own network interfaces, routing table, firewall rules, and TCP/IP stack — complete network isolation.

**How it works:**
```
Host Kernel Network Stack (default)
    │
    ├── VNET 1 (Pod A)
    │     ├── lo0 (127.0.0.1)
    │     ├── epair0a (10.0.1.5)
    │     ├── Routing table: default via 10.0.1.1
    │     └── pf rules: allow 80, 443
    │
    ├── VNET 2 (Pod B)
    │     ├── lo0 (127.0.0.1)
    │     ├── epair1a (10.0.2.3)
    │     ├── Routing table: default via 10.0.2.1
    │     └── pf rules: allow 3306
    │
    └── VNET 3 (Pod C)
          ├── lo0 (127.0.0.1)
          ├── epair2a (10.0.1.6)
          ├── Routing table: default via 10.0.1.1
          └── pf rules: deny all
```

**Key properties:**
- **Complete isolation:** Each VNET has its own ARP table, routing table, socket buffer limits, and firewall.
- **Performance:** Near-native speed; packets never leave the kernel unless going to another host.
- **Integration:** Works with epair(4), bridge(4), and VXLAN interfaces.
- **Limitations:** Each VNET consumes some kernel memory (~1-2 MB).

**VNET vs Linux network namespaces:**
| Feature | FreeBSD VNET | Linux Network Namespace |
|---------|-------------|------------------------|
| Scope | Full network stack | Network resources only |
| Interfaces | Dedicated per VNET | Can be moved between namespaces |
| Firewall | Independent pf per VNET | iptables/nftables per namespace |
| Routing | Independent FIB | Independent routing table |
| Performance | Kernel-native | Kernel-native |

**In our system:** Every pod gets its own VNET. The pod's VNET interface is bridged to the host's VXLAN interface for cross-host communication.

---

### A.1.3 ARP and NDP

**ARP (Address Resolution Protocol)** maps IPv4 addresses to MAC addresses on a local network. **NDP (Neighbor Discovery Protocol)** is the IPv6 equivalent.

**Why they matter:** For MetalLB-style load balancing, we need to announce IP addresses to the local network so routers know where to send traffic.

**How ARP works:**
```
Router: "Who has 192.168.1.100?"
    │
    ▼
Manager Node (running MetalLB equivalent):
    "I have 192.168.1.100, my MAC is aa:bb:cc:dd:ee:ff"
    │
    ▼
Router updates ARP table:
    192.168.1.100 → aa:bb:cc:dd:ee:ff
```

**How NDP works (IPv6):**
```
Router: "Who has fe80::1?"
    │
    ▼
Manager Node:
    "I have fe80::1, my MAC is aa:bb:cc:dd:ee:ff"
    │
    ▼
Router updates neighbor cache:
    fe80::1 → aa:bb:cc:dd:ee:ff
```

**Gratuitous ARP/NDP:**
When a service IP moves from one node to another (failover), the new node sends a "gratuitous" ARP/NDP announcement:
```
New Node: "I have 192.168.1.100" (unsolicited)
    │
    ▼
All devices on network update ARP table immediately
```

**In our system:** The ingress controller uses ARP/NDP to announce service IPs on the local network. This is simpler than BGP for small deployments.

---

### A.1.4 BGP

**BGP (Border Gateway Protocol)** is the routing protocol that powers the internet. It exchanges routing information between autonomous systems.

**Why it matters:** For production deployments, BGP is the cleanest way to announce service IPs to the network. Instead of ARP/NDP tricks, the cluster speaks directly to routers.

**How it works:**
```
Cluster Manager ──► Router (BGP peer)
    "I can reach 10.0.0.0/24 via me (192.168.1.10)"

Router updates its routing table:
    10.0.0.0/24 → 192.168.1.10

Traffic to 10.0.0.0/24 is routed to the manager node
```

**Key properties:**
- **AS Path:** BGP tracks the path of autonomous systems to prevent loops.
- **Route selection:** Multiple paths possible; BGP picks the best based on policy.
- **Health checking:** If a node fails, BGP session drops; router withdraws routes.
- **Scalability:** Used by all major CDNs and cloud providers.

**In our system:** BGP mode is optional. When enabled, managers establish BGP peering with upstream routers and announce service IP ranges directly.

---

### A.1.5 CNI

**CNI (Container Network Interface)** is a specification and set of libraries for configuring network interfaces in Linux containers.

**Why it matters:** While our system is FreeBSD-native, supporting CNI plugins allows reuse of existing network configurations and tooling.

**How it works:**
```
Container Runtime (ocifbsd)
    │
    ▼
CNI Plugin (bridge, host-local, etc.)
    │
    ├── Creates veth pair (Linux) or epair (FreeBSD)
    ├── Assigns IP address
    ├── Sets up routes
    └── Configures firewall
```

**In our system:** We provide a CNI-compatible interface so existing CNI plugins can be adapted for FreeBSD VNET.

---

### A.1.6 Load Balancing Algorithms

**Round-robin:** Distributes requests sequentially across backends.
```
Request 1 → Backend A
Request 2 → Backend B
Request 3 → Backend C
Request 4 → Backend A
```
**Pros:** Simple, fair. **Cons:** Ignores load and capacity.

**Least connections:** Sends request to backend with fewest active connections.
```
Backend A: 5 connections
Backend B: 2 connections ← Request goes here
Backend C: 8 connections
```
**Pros:** Balances load better. **Cons:** More state to track.

**IP hash:** Uses client IP to determine backend (same client → same backend).
```
hash(client_ip) % backend_count = backend_index
```
**Pros:** Session affinity. **Cons:** Uneven distribution.

**Weighted:** Backends have different capacities.
```
Backend A: weight 3 (powerful)
Backend B: weight 1 (weak)
Backend C: weight 2 (medium)
```
**Pros:** Respects capacity. **Cons:** Requires tuning.

**Random:** Random selection.
**Pros:** Simple, good distribution at scale. **Cons:** No predictability.

**In our system:** All algorithms are supported. Default is round-robin for simplicity, with least-connections recommended for long-lived connections.

---

## A.2 Storage Terms

### A.2.1 ZFS

**ZFS (Zettabyte File System)** is an advanced file system and logical volume manager originally developed by Sun Microsystems. It is the default file system on FreeBSD.

**Why it matters:** ZFS provides the foundation for our container storage. Its features — snapshots, clones, compression, encryption, and checksums — are essential for efficient, reliable container operations.

**Key features:**
| Feature | Description | Container Use |
|---------|-------------|---------------|
| **Snapshots** | Point-in-time read-only copies | Rollback, backup, image layers |
| **Clones** | Writable copies of snapshots | Instant container creation |
| **Compression** | Transparent data compression | Reduced image size |
| **Encryption** | AES-256-GCM encryption | Secret storage, image protection |
| **Checksums** | End-to-end data integrity | Corruption detection |
| **Deduplication** | Store identical blocks once | Layer sharing |
| **Copy-on-Write** | Never overwrite in place | Efficient updates |

**ZFS dataset hierarchy for containers:**
```
zroot/ocifbsd
├── images
│   ├── sha256:abc... (image layer)
│   └── sha256:def... (image layer)
├── containers
│   ├── pod-a (clone of image)
│   └── pod-b (clone of image)
├── volumes
│   ├── db-data (persistent volume)
│   └── cache (persistent volume)
└── secrets
    └── encrypted dataset
```

**In our system:** Every container is a ZFS clone of an image snapshot. Updates are copy-on-write. Snapshots enable instant migration and rollback.

---

### A.2.2 ZFS Send and Receive

**ZFS send** serializes a snapshot into a stream. **ZFS receive** reconstructs the snapshot on another system.

**Why it matters:** This is the mechanism for live migration and image distribution. It is extremely efficient because it only sends changed blocks.

**How it works:**
```
Source Host:
    zfs snapshot zroot/containers/pod-a@migrate
    zfs send zroot/containers/pod-a@migrate
        │
        ▼
    Stream: [metadata][block 1][block 2]...
        │
        ▼
Destination Host:
    zfs receive zroot/containers/pod-a
        │
        ▼
    Snapshot reconstructed identically
```

**Incremental send:**
```
# Send only changes since last snapshot
zfs send -i zroot/containers/pod-a@base zroot/containers/pod-a@migrate
```

**Performance:**
- Full send: ~100 MB/s (limited by disk read)
- Incremental: ~10-50 MB/s (only changed blocks)
- Compression: Built-in, reduces stream size 2-5x

**In our system:** Used for live migration (send running container state) and P2P image distribution (send image layers).

---

### A.2.3 Copy-on-Write

**Copy-on-Write (CoW)** is a strategy where multiple callers ask for resources that appear to be separate but are actually shared until one caller modifies its copy.

**Why it matters:** CoW makes container creation instant and storage-efficient. A new container doesn't copy the entire image — it shares all blocks and only allocates new space when it writes.

**How it works:**
```
Image Layer (read-only)
┌─────────────────┐
│ Block A: "foo"  │
│ Block B: "bar"  │
│ Block C: "baz"  │
└─────────────────┘
         │
         ▼ clone
    Container (read-write)
    ┌─────────────────┐
    │ Block A: "foo"  │ ← shared (no extra space)
    │ Block B: "bar"  │ ← shared (no extra space)
    │ Block C: "qux"  │ ← copied and modified
    └─────────────────┘
```

**Space efficiency:**
- 10 containers from same 1 GB image: ~1 GB total (not 10 GB)
- Only modified blocks consume extra space

**In our system:** All container storage is CoW via ZFS clones.

---

### A.2.4 Deduplication

**Deduplication** stores only one copy of identical data blocks, regardless of how many times they appear.

**Why it matters:** Multiple images may share common layers (e.g., base FreeBSD system). Deduplication ensures we only store one copy.

**Trade-offs:**
| Aspect | Impact |
|--------|--------|
| Memory | Requires ~1 GB RAM per TB of deduped data |
| CPU | Hash computation overhead |
| Disk | Reduced writes, longer read paths |

**In our system:** Deduplication is optional. Recommended for storage nodes with large image libraries.

---

## A.3 Security Terms

### A.3.1 mTLS

**mTLS (Mutual TLS)** is a form of TLS where both the client and server authenticate each other using X.509 certificates.

**Why it matters:** Standard TLS only proves the server's identity (e.g., HTTPS). mTLS proves both sides, preventing any unauthorized node from connecting to the cluster.

**Standard TLS:**
```
Client ──► Server
    "Are you really bank.com?"
    Server presents certificate
    Client validates against trusted CAs
```

**mTLS:**
```
Client ──► Server
    Client: "Are you really the API server?"
    Server presents certificate
    Client validates

    Server: "Are you really worker-1?"
    Client presents certificate
    Server validates

    Both identities confirmed → Connection established
```

**In our system:** All node-to-node communication uses mTLS. Every connection is mutually authenticated.

---

### A.3.2 Certificate Authority

A **Certificate Authority (CA)** is a trusted entity that issues digital certificates.

**Why it matters:** The cluster CA is the root of trust. Every node trusts certificates signed by the CA, and rejects all others.

**Certificate chain:**
```
Cluster CA (self-signed, trusted by all)
    │
    ├── Signs → Manager 1 certificate
    ├── Signs → Manager 2 certificate
    ├── Signs → Worker 1 certificate
    └── Signs → API server certificate
```

**Validation:**
```
Node receives certificate
    │
    ▼
Verify signature using CA public key
    │
    ▼
Check certificate fields (not expired, correct role, etc.)
    │
    ▼
Accept or reject connection
```

**In our system:** The cluster CA is created on first manager initialization. It can be backed by an HSM for additional security.

---

### A.3.3 CSR

**CSR (Certificate Signing Request)** is a message sent to a CA to request a certificate.

**Contents:**
- Public key
- Identity information (CN, O, OU, SAN)
- Signature (proving possession of private key)

**Flow:**
```
Node generates key pair
    │
    ▼
Node creates CSR (includes public key + identity)
    │
    ▼
Node sends CSR + bootstrap token to manager
    │
    ▼
Manager validates token, verifies CSR
    │
    ▼
Manager signs CSR → Certificate issued
    │
    ▼
Node receives certificate
```

**In our system:** Every node generates a CSR during join. The manager validates and signs it.

---

### A.3.4 RBAC

**RBAC (Role-Based Access Control)** restricts system access based on user roles.

**Why it matters:** Not everyone should be able to delete pods or access secrets. RBAC defines who can do what.

**Components:**
| Component | Description | Example |
|-----------|-------------|---------|
| **Role** | Set of permissions | "developer" can create pods |
| **Subject** | Who is being authorized | User "alice", service account "api" |
| **Binding** | Links subject to role | "alice" has "developer" role |
| **Resource** | What is being accessed | pods, services, secrets |
| **Verb** | What action is being taken | create, read, update, delete |

**In our system:** RBAC is enforced on every API request. The web UI operates under its own restricted role.

---

### A.3.5 MAC Labels

**MAC (Mandatory Access Control)** labels are security tags enforced by the operating system kernel.

**Why it matters:** MAC provides isolation beyond Unix permissions. Even root inside a jail cannot access resources labeled for another jail.

**How it works:**
```
Pod A: label "ocifbsd/pod-a"
Pod B: label "ocifbsd/pod-b"

Pod A (root) tries to read Pod B's files
    │
    ▼
Kernel checks MAC label
    │
    ▼
"ocifbsd/pod-a" ≠ "ocifbsd/pod-b"
    │
    ▼
Access denied (even though Unix permissions allow it)
```

**In our system:** Every pod gets a unique MAC label. The kernel enforces isolation at the file, network, and process levels.

---

### A.3.6 RCTL

**RCTL (Resource Control)** is FreeBSD's mechanism for limiting resource usage per process, user, or jail.

**Why it matters:** RCTL prevents a single pod from consuming all host resources (CPU, memory, open files).

**Example rules:**
```
jail:pod-a:memoryuse:deny=1G          # Max 1 GB RAM
jail:pod-a:cpuset:0-3                 # CPUs 0-3 only
jail:pod-a:pcpu:deny=50               # Max 50% CPU
jail:pod-a:openfiles:deny=1024        # Max 1024 open files
jail:pod-a:vmemoryuse:deny=2G         # Max 2 GB virtual memory
```

**Actions:**
| Action | Behavior |
|--------|----------|
| `deny` | Block the operation |
| `log` | Allow but log a warning |
| `sigterm` | Send SIGTERM to offending process |
| `sigkill` | Send SIGKILL to offending process |

**In our system:** OCI resource limits are translated to RCTL rules automatically.

---

### A.3.7 TLS 1.3

**TLS 1.3** is the latest version of the Transport Layer Security protocol, providing encrypted communication.

**Improvements over TLS 1.2:**
| Feature | TLS 1.2 | TLS 1.3 |
|---------|---------|---------|
| Handshake round trips | 2 | 1 (0-RTT optional) |
| Supported ciphers | Many (some weak) | Only strong |
| Perfect forward secrecy | Optional | Required |
| Latency | Higher | Lower |

**In our system:** TLS 1.3 is required for all cluster communication. Older versions are rejected.

---

### A.3.8 AES-256-GCM

**AES-256-GCM** is an authenticated encryption algorithm providing both confidentiality and integrity.

**Why it matters:** This is the standard for encrypting secrets at rest. It ensures data cannot be read or tampered with.

**Properties:**
- **AES-256:** 256-bit key, brute-force resistant
- **GCM (Galois/Counter Mode):** Provides authentication (tamper detection)
- **Nonce:** Must be unique per encryption; reused nonce breaks security

**In our system:** Used for secret storage, ZFS dataset encryption, and Raft log encryption.

---

## A.4 Container and Orchestration Terms

### A.4.1 OCI

**OCI (Open Container Initiative)** is a standards body that defines specifications for container formats and runtimes.

**Key specifications:**
| Spec | Purpose | Our Implementation |
|------|---------|-------------------|
| **Runtime Spec** | How to run a container | `ocifbsd` runtime |
| **Image Spec** | Format for container images | Image pull/unpack |
| **Distribution Spec** | How to push/pull images | Registry client |

**In our system:** We implement OCI specs natively for FreeBSD jails instead of Linux cgroups/namespaces.

---

### A.4.2 Jail

A **FreeBSD jail** is a lightweight operating-system-level virtualization mechanism.

**What it provides:**
- Separate root filesystem
- Separate process tree (PID 1 is isolated)
- Separate user namespace (root in jail ≠ root on host)
- Optional separate network stack (VNET)
- Resource limits via RCTL

**Jail vs VM:**
| Aspect | Jail | VM |
|--------|------|-----|
| Kernel | Shared with host | Separate |
| Boot time | Instant | Seconds |
| Memory overhead | ~1 MB | ~100 MB |
| Performance | Native | ~95% native |
| Isolation | Process-level | Hardware-level |

**In our system:** Jails are the container runtime. OCI containers run as jails.

---

### A.4.3 Pod

A **pod** is the smallest deployable unit in our orchestration system, consisting of one or more tightly coupled containers.

**Why pods (not just containers):**
- **Shared resources:** All containers in a pod share the same network namespace and storage volumes.
- **Sidecars:** A main application container + helper containers (logging, monitoring, proxy).
- **Lifecycle coupling:** Containers in a pod are scheduled, started, and stopped together.

**Example pod:**
```yaml
pod: web-server
containers:
  - name: nginx
    image: nginx:latest
    ports: [80, 443]
  - name: log-shipper
    image: fluent-bit:latest
    volumes: [logs]
  - name: metrics-exporter
    image: node-exporter:latest
    ports: [9100]
```

**In our system:** A pod maps to a single jail with multiple processes (one per container).

---

### A.4.4 Namespace

A **namespace** is a logical isolation boundary for resources.

**What it isolates:**
| Resource | Isolation |
|----------|-----------|
| Pods | Cannot see pods in other namespaces |
| Services | DNS-scoped to namespace |
| Networks | Separate VXLAN VNI |
| Secrets | Only accessible within namespace |
| Volumes | Only mountable within namespace |

**Use cases:**
- **Production vs staging:** Same cluster, separate namespaces
- **Multi-tenant:** Different teams, isolated resources
- **System:** Internal services (DNS, monitoring)

**In our system:** Namespaces are implemented via VNET isolation, RCTL rules, and MAC labels.

---

### A.4.5 Cgroup

**Cgroups (control groups)** are Linux's mechanism for resource limiting and accounting.

**Why it matters for us:** OCI specs reference cgroups. We translate cgroup semantics to FreeBSD RCTL.

**Mapping:**
| Linux Cgroup | FreeBSD RCTL |
|-------------|--------------|
| `memory.limit_in_bytes` | `memoryuse` |
| `cpu.cfs_quota_us` | `pcpu` |
| `cpu.shares` | `priority` |
| `pids.max` | `maxproc` |
| `blkio.throttle.read_bps_device` | `readbps` |

**In our system:** Cgroup configuration in OCI specs is automatically translated to RCTL rules.

---

### A.4.6 Raft

**Raft** is a consensus algorithm for distributed systems. It ensures that a cluster of nodes agrees on a shared state even if some nodes fail.

**Why it matters:** The cluster needs to agree on critical state (node membership, pod placement, TLS certificates). Raft provides this agreement reliably.

**How it works:**
```
Managers (3 nodes)
    │
    ├── Leader (handles all writes)
    │     Receives state change
    │     Appends to local log
    │     Sends to followers
    │     Waits for majority acknowledgment
    │     Commits the change
    │
    ├── Follower 1 (replicates log)
    └── Follower 2 (replicates log)
```

**Key properties:**
- **Leader election:** If leader fails, remaining nodes elect a new leader within seconds.
- **Log replication:** All committed entries are stored on a majority of nodes.
- **Safety:** Committed entries are never lost or overwritten.
- **Quorum:** Majority required for any decision (3 nodes → 2 required, 5 nodes → 3 required).

**In our system:** Managers run Raft for cluster state. Workers do not participate in Raft.

---

### A.4.7 Gossip Protocol

A **gossip protocol** is a communication method where nodes periodically exchange state with random peers, propagating information epidemically.

**Why it matters:** Gossip scales to hundreds of nodes without a central coordinator. It is resilient to network partitions.

**How it works:**
```
Round 1:
    Node A tells Node B: "I know about nodes A, C, D"
    Node B tells Node C: "I know about nodes B, E, F"

Round 2:
    Node A tells Node D: "I know about nodes A, B, C, D, E, F"
    (information spreads exponentially)
```

**Properties:**
- **Scalability:** O(log N) rounds to reach all nodes
- **Fault tolerance:** No single point of failure
- **Eventual consistency:** All nodes converge to same state
- **Bandwidth efficient:** Only deltas exchanged

**In our system:** Gossip is used for membership, health status, and runtime metrics.

---

### A.4.8 SWIM

**SWIM (Scalable Weakly-consistent Infection-style Process Group Membership Protocol)** is a specific gossip protocol for failure detection.

**Why it matters:** SWIM provides fast, accurate failure detection without flooding the network.

**How it works:**
```
1. Direct probe:
    Node A pings Node B
    If no response → Node B is suspect

2. Indirect probe:
    Node A asks Node C and Node D to ping Node B
    If they also get no response → Node B is failed

3. Gossip dissemination:
    Node A tells others: "Node B is failed"
    Information spreads through gossip
```

**In our system:** SWIM is the basis for our gossip-based membership protocol.

---

## A.5 FreeBSD-Specific Terms

### A.5.1 VNET Jail

A **VNET jail** is a jail with its own independent network stack.

**Creation:**
```bash
jail -c vnet name=pod-a path=/var/jails/pod-a \
     interface=epair0a ip4.addr=10.0.1.5
```

**What it gets:**
- Own network interfaces (epair, lo)
- Own routing table
- Own firewall (pf)
- Own socket buffer limits
- Cannot see host network interfaces

**In our system:** All pods are VNET jails for complete network isolation.

---

### A.5.2 epair

**epair** is a FreeBSD virtual Ethernet interface pair — like a virtual cable with two ends.

**How it works:**
```
epair0a ──► epair0b

epair0a is in the host (or bridge)
epair0b is in the jail (pod)

Traffic enters epair0a → exits epair0b inside the jail
```

**Creation:**
```bash
ifconfig epair create
ifconfig epair0a up
ifconfig bridge0 addm epair0a
ifconfig epair0b vnet pod-a
jexec pod-a ifconfig epair0b 10.0.1.5/24 up
```

**In our system:** Each pod gets an epair interface. One end is in the host bridge/VXLAN; the other is inside the pod's VNET.

---

### A.5.3 pf

**pf (packet filter)** is FreeBSD's firewall and NAT engine.

**Why it matters:** pf provides per-jail firewall rules, NAT for outbound traffic, and port forwarding for inbound traffic.

**Example rules:**
```
# NAT for pod outbound traffic
nat on em0 from 10.0.0.0/8 to any -> (em0)

# Allow HTTP/HTTPS to web pods
pass in on em0 proto tcp to any port {80, 443}

# Block everything else to database pods
block in on em0 proto tcp to 10.0.2.0/24
```

**In our system:** Each namespace gets its own pf anchor for isolated firewall rules.

---

### A.5.4 CARP

**CARP (Common Address Redundancy Protocol)** provides IP address failover between multiple hosts.

**How it works:**
```
Host A (master)    Host B (backup)
192.168.1.10       192.168.1.11
    │                    │
    └── Shared IP: 192.168.1.100 ──┘

Host A fails
    │
    ▼
Host B detects (no CARP advertisements)
    │
    ▼
Host B takes over 192.168.1.100
    │
    ▼
Traffic continues via Host B
```

**In our system:** CARP can be used for manager VIP failover in addition to our own failover mechanisms.

---

### A.5.5 Jails vs Linux Containers

| Feature | FreeBSD Jail | Linux Container |
|---------|-------------|-----------------|
| Kernel | Shared | Shared |
| Root filesystem | Separate | Separate (overlayfs) |
| Process isolation | Complete PID namespace | PID namespace |
| Network isolation | VNET (full stack) | Network namespace |
| Resource limits | RCTL | cgroups |
| Security | MAC + jail | seccomp + AppArmor |
| Maturity | 25+ years | ~10 years |
| ZFS integration | Native | Add-on |

**In our system:** We leverage jail's maturity and ZFS integration for a robust container platform.

---

## A.6 Hardware and Virtualization Terms

### A.6.1 HSM

**HSM (Hardware Security Module)** is a physical device that manages digital keys and performs cryptographic operations.

**Why it matters:** The cluster CA private key is the most sensitive asset. An HSM ensures it never leaves protected hardware.

**Operations performed in HSM:**
- Key generation
- Certificate signing
- Decryption

**What never leaves HSM:**
- Private keys

**What can leave HSM:**
- Public keys
- Certificates
- Signed data

**In our system:** HSM support is optional but recommended for production.

---

### A.6.3 SR-IOV

**SR-IOV (Single Root I/O Virtualization)** allows a physical NIC to appear as multiple virtual NICs.

**Why it matters:** Pods can get direct NIC access without the overhead of software bridging.

**How it works:**
```
Physical NIC (PF - Physical Function)
    │
    ├── VF 1 → Pod A (direct access)
    ├── VF 2 → Pod B (direct access)
    └── VF 3 → Pod C (direct access)
```

**In our system:** SR-IOV is optional for high-performance networking workloads.

---

### A.6.4 DPDK

**DPDK (Data Plane Development Kit)** is a set of libraries for fast packet processing in userspace.

**Why it matters:** Bypassing the kernel network stack can achieve millions of packets per second.

**Trade-offs:**
| Aspect | Kernel Networking | DPDK |
|--------|------------------|------|
| Performance | Good | Excellent |
| Complexity | Low | High |
| Portability | Universal | NIC-specific |
| Integration | Native | Requires polling |

**In our system:** DPDK is a future enhancement for high-frequency trading or telco workloads.

---

## A.7 Protocol and Data Format Terms

### A.7.1 JSON

**JSON (JavaScript Object Notation)** is a lightweight data interchange format.

**Example:**
```json
{
  "name": "web-server",
  "replicas": 3,
  "ports": [80, 443],
  "labels": {
    "app": "frontend",
    "tier": "web"
  }
}
```

**In our system:** JSON is used for API responses, state storage, and configuration.

---

### A.7.2 YAML

**YAML (YAML Ain't Markup Language)** is a human-readable data serialization format.

**Example:**
```yaml
pod: web-server
replicas: 3
ports:
  - 80
  - 443
labels:
  app: frontend
  tier: web
```

**In our system:** YAML is the primary configuration format for stacks, pods, and services.

---

### A.7.3 Protobuf

**Protocol Buffers (protobuf)** is a binary serialization format developed by Google.

**Why it matters:** Protobuf is smaller and faster than JSON for internal communication.

**Comparison:**
| Aspect | JSON | Protobuf |
|--------|------|----------|
| Size | Text, verbose | Binary, compact |
| Speed | Parse text | Direct binary read |
| Schema | Implicit | Explicit (.proto file) |
| Human-readable | Yes | No |

**In our system:** Protobuf is used for gossip messages and Raft log entries.

---

### A.7.4 JWT

**JWT (JSON Web Token)** is a compact, self-contained token format for authentication.

**Structure:**
```
eyJhbGciOiJIUzI1NiIs... (header)
.
eyJzdWIiOiIxMjM0NTY3OD... (payload)
.
SflKxwRJSMeKKF2QT4fwpM... (signature)
```

**Contents:**
- **Header:** Algorithm and token type
- **Payload:** Claims (user, expiry, roles)
- **Signature:** Cryptographic verification

**In our system:** JWTs are used for user authentication tokens (24-hour expiry).

---

## A.8 Performance and Scalability Terms

### A.8.1 Eventual Consistency

**Eventual consistency** is a consistency model where updates propagate asynchronously, and all nodes eventually converge to the same state.

**Why it matters:** Not all data needs immediate consistency. Runtime metrics and health status can be eventually consistent, reducing coordination overhead.

**Example:**
```
T=0: Pod A fails on Node 1
T=1: Node 1 marks Pod A failed locally
T=2: Node 1 gossips failure to Node 2
T=3: Node 2 updates its view
T=4: Node 2 gossips to Node 3
T=5: All nodes know Pod A failed
```

**In our system:** Runtime state (pod health, metrics) is eventually consistent. Critical state (placement, certificates) is strongly consistent.

---

### A.8.2 Strong Consistency

**Strong consistency** ensures that all nodes see the same data at the same time.

**Why it matters:** You cannot have two managers disagreeing on which node runs a pod. Strong consistency prevents split-brain scenarios.

**How it's achieved:**
- Raft consensus for cluster state
- All writes go through leader
- Majority acknowledgment required

**In our system:** Cluster configuration, pod placement, and TLS certificates use strong consistency.

---

### A.8.3 Quorum

A **quorum** is the minimum number of votes required for a distributed system to make a decision.

**Formula:**
```
quorum = floor(N / 2) + 1

N=3 → quorum=2
N=5 → quorum=3
N=7 → quorum=4
```

**Why it matters:** Quorum ensures that even if some nodes fail, the remaining nodes can still make progress without conflicting decisions.

**In our system:** Manager decisions require quorum. This is why we require an odd number of managers (3, 5, 7).

---

### A.8.4 Backpressure

**Backpressure** is a flow control mechanism where a downstream system signals an upstream system to slow down.

**Why it matters:** If a node is overloaded, it should tell the scheduler to stop sending new pods.

**Example:**
```
Node CPU: 95%
    │
    ▼
Node reports: "I'm under pressure"
    │
    ▼
Scheduler stops placing new pods on this node
    │
    ▼
Existing pods continue running
```

**In our system:** Nodes report resource pressure via gossip. The scheduler respects backpressure signals.

---

### A.8.5 Connection Draining

**Connection draining** is the process of gracefully removing a backend from a load balancer by allowing existing connections to complete.

**Why it matters:** During migration or updates, we don't want to drop active user connections.

**How it works:**
```
1. Admin initiates drain on Pod A
2. Load balancer stops sending NEW requests to Pod A
3. Existing connections continue
4. Pod A waits for connections to close (or timeout)
5. Once drained, Pod A can be safely stopped/migrated
```

**In our system:** Connection draining is used during live migration and rolling updates.

---

### A.8.6 ACME (Automated Certificate Management Environment)

**ACME (Automated Certificate Management Environment)** is a protocol for automating certificate issuance and management, most famously used by Let's Encrypt.

**Why it matters:** Manually managing TLS certificates is error-prone and often neglected. ACME automates the entire lifecycle.

**How it works:**
```
1. Client generates key pair
2. Client requests certificate from ACME server
3. ACME server issues challenge (HTTP-01, DNS-01, or TLS-ALPN-01)
4. Client proves control over domain
5. ACME server issues signed certificate
6. Client automatically renews before expiry
```

**ACME Challenges:**
- **HTTP-01:** Place a file on port 80 of the domain
- **DNS-01:** Create a TXT record in DNS
- **TLS-ALPN-01:** Respond to TLS ALPN request (ALPN challenge)

**In our system:** The `ocifbsd cert acme` commands support automatic certificate issuance and renewal via Let's Encrypt and other ACME-compatible CAs.

---

### A.8.7 SCEP (Simple Certificate Enrollment Protocol)

**SCEP** is a protocol for automating certificate enrollment, primarily used in enterprise environments and by Microsoft AD CS.

**Why it matters:** Enterprises with existing PKI infrastructure need a way to integrate new services without manual certificate requests.

**How it works:**
```
1. Client generates CSR
2. Client sends CSR to SCEP endpoint
3. SCEP server authenticates request (password, certificate, or HMAC)
4. SCEP server issues certificate
5. Client polls for certificate ready status
6. Client retrieves certificate
```

**In our system:** The `ocifbsd cert scep enroll` command allows integration with Microsoft AD CS and other SCEP-enabled CAs.

---

### A.8.8 EST (Enrollment over Secure Transport)

**EST** is a modern replacement for SCEP, using TLS client certificates and HTTP-based enrollment.

**Why it matters:** SCEP was designed for environments with limited PKI knowledge. EST provides a cleaner, more secure protocol.

**Advantages over SCEP:**
- Uses TLS client certificates for authentication
- No shared secrets or challenge passwords
- Built on modern HTTPS infrastructure
- Supports CSR signing for CA certificate renewal

**In our system:** The `ocifbsd cert est enroll` command provides EST-based enrollment for enterprise PKI integration.

---

### A.8.9 .feb (Flat Export Bundle)

**.feb (Flat Export Bundle)** is our container export format designed for portability across cloud providers.

**Why it matters:** Moving containers between environments (on-prem to cloud, cloud to cloud) requires a standard, portable format.

**Structure:**
```
container.feb/
├── metadata.json          # Container config, entrypoint, env vars
├── rootfs.tar.gz          # Container filesystem (compressed)
├── volumes/               # Volume data (if included)
│   ├── volume1.tar.gz
│   └── volume2.tar.gz
├── config.json            # OCI runtime config
└── manifest.json          # Export manifest (version, platform info)
```

**Benefits:**
- Self-contained: All data in one directory/tree
- Compressed: Efficient storage and transfer
- Platform-agnostic: Works across AWS, GCP, Azure
- Incremental: Can export only changed volumes

**In our system:** The `ocifbsd export` and `ocifbsd import` commands work with .feb bundles for cloud migration and backup.

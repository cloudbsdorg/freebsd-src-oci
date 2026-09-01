# AGENTS START HERE — FreeBSD Native OCI Runtime (`ocifbsd`)

> **Purpose:** This is the primary entry point for autonomous agents working on the FreeBSD Native OCI Runtime. Read this file **first** before consuming any other documents in the `.plan/` directory.

> **FreeBSD:** The environment in which this work is being done may have elements that state that you are in linux, that would be false. You are running in FreeBSD.

---

## What We're Building

A **native OCI (Open Container Initiative) runtime** for FreeBSD that provides:

- **FreeBSD-native container runtime** using jail(8) as the isolation mechanism
- **OCI Runtime Spec v1.0+ compliance** for compatibility with Docker, Podman, and Kubernetes
- **ZFS-native storage** for efficient image layers via snapshots and clones
- **VNET networking** for full network stack virtualization per container
- **Kubernetes-like orchestration** with pods, services, load balancing, and failover
- **Multi-node clustering** with gossip-based membership and Raft consensus
- **Simplified configuration** — much simpler than Kubernetes YAML, but just as powerful
- **ACME/Let's Encrypt support** for automatic TLS certificate management
- **Cloud export** for AWS AMI, GCP, and Azure VHD formats

The project builds on FreeBSD's native technologies:
- **Jails** — FreeBSD's containerization technology
- **ZFS** — Copy-on-write filesystem with snapshots and clones
- **VNET** — Virtual network stack isolation
- **RCTL** — Resource limits (CPU, memory, etc.)
- **MAC** — Mandatory Access Control labels

---

## Document Structure

All plan documents are in the `.plan/` directory, numbered using the `<Major>.<Minor>` convention:

| # | File | What It Covers |
|---|------|----------------|
| `0.0` | [`000.0-OCI-Jail-TOC.md`](.plan/000.0-OCI-Jail-TOC.md) | Master table of contents with clickable links to all documents |
| `0.1` | [`000.1-Agent-Workflow.md`](.plan/000.1-Agent-Workflow.md) | Task claiming, completion, merge conflict handling |
| `1.0` | [`001.0-Overview.md`](.plan/001.0-Overview.md) | Executive summary, current/proposed architecture |
| `1.1` | [`001.1-Implementation-Phases.md`](.plan/001.1-Implementation-Phases.md) | Phase-by-phase task breakdown |
| `1.2` | [`001.2-Alternative-Approaches.md`](.plan/001.2-Alternative-Approaches.md) | Why existing approaches fall short |
| `2.0` | [`002.0-CLI-Spec.md`](.plan/002.0-CLI-Spec.md) | Complete CLI specification |
| `3.0` | [`003.0-Implementation.md`](.plan/003.0-Implementation.md) | Implementation details and file structure |
| `4.0` | [`004.0-Testing.md`](.plan/004.0-Testing.md) | Testing strategy |
| `5.0` | [`005.0-Risks-TODO.md`](.plan/005.0-Risks-TODO.md) | Risks, TODO tracker |
| `6.0` | [`006.0-Orchestration.md`](.plan/006.0-Orchestration.md) | Kubernetes-like orchestration |
| `7.0` | [`007.0-Config-Conversion.md`](.plan/007.0-Config-Conversion.md) | K8s/Compose config converter |
| `8.0` | [`008.0-Namespace-Resource-Management.md`](.plan/008.0-Namespace-Resource-Management.md) | Namespaces, health/resource monitoring, live migration |
| `9.0` | [`009.0-Clustering-Infrastructure.md`](.plan/009.0-Clustering-Infrastructure.md) | Multi-node clustering, gossip, Raft |
| `10.0` | [`010.0-Security-Credentials-WebUI.md`](.plan/010.0-Security-Credentials-WebUI.md) | Security, RBAC, secrets, Web UI |
| `11.0` | [`011.0-Node-Authentication.md`](.plan/011.0-Node-Authentication.md) | Node auth, mTLS, bootstrap tokens |
| `12.0` | [`012.0-Cluster-UX.md`](.plan/012.0-Cluster-UX.md) | TPM management, simplified UX |
| `13.0` | [`013.0-PAM-System-Credentials.md`](.plan/013.0-PAM-System-Credentials.md) | PAM integration, system credentials |
| `14.0` | [`014.0-Observability-Logging.md`](.plan/014.0-Observability-Logging.md) | Structured logging, alerting, events |
| `15.0` | [`015.0-API-Specification.md`](.plan/015.0-API-Specification.md) | REST/gRPC API spec |
| `16.0` | [`016.0-Garbage-Collection.md`](.plan/016.0-Garbage-Collection.md) | Orphan cleanup, Kubernetes mitigations |
| `17.0` | [`017.0-Certificate-Management.md`](.plan/017.0-Certificate-Management.md) | Cert rotation, backup, ACME, external CA |
| `18.0` | [`018.0-Cloud-Export-Migration.md`](.plan/018.0-Cloud-Export-Migration.md) | AWS/GCP/Azure export |
| `A.0` | [`A.0-Glossary-and-Appendix.md`](.plan/A.0-Glossary-and-Appendix.md) | Glossary and technical appendix |

---

## Primary Directives

### 1. Security First
- **Defense in depth** — Multiple layers of security (jail, VNET, MAC, RCTL)
- **mTLS everywhere** — All cluster communication encrypted with mutual TLS
- **Secrets management** — Sensitive data stored encrypted, never in plaintext
- **Audit logging** — All security-relevant actions logged
- **PAM integration** — Leverage FreeBSD's native authentication

### 2. OCI Compliance
- **Runtime Spec v1.0+** — Follow the OCI standard for container configuration
- **Image Spec v1.0+** — Support standard OCI images from registries
- **Conformance testing** — Pass OCI runtime-tools tests

### 3. Simplicity Over Complexity
- **Simple by default** — Sensible defaults, minimal configuration
- **50 lines vs 200+** — Configuration should be much simpler than Kubernetes
- **Common-sense UX** — Don't require a PhD to use
- **Progressive disclosure** — Simple things simple, complex things possible

### 4. ZFS-Native Storage
- **Snapshots and clones** — Efficient image layer storage
- **ZFS send/recv** — For migration and disaster recovery
- **Encryption support** — Per-dataset ZFS encryption
- **Compression** — Built-in ZFS compression

### 5. Traceability
- **Every task must be claimed** — Update the task table before starting work
- **Every task must be completed with tests** — No task is done until tests pass
- **Every change must be committed** — Commit after claiming, commit after completing
- **Fix other agents' code** — If tests fail due to another agent's bugs, fix them

---

## Workflow Summary

### Picking a Task
1. Pull latest: `git pull --rebase`
2. Open [`000.0-OCI-Jail-TOC.md`](.plan/000.0-OCI-Jail-TOC.md) for the master index
3. Find a task with empty `Status`, `Owner`, and `Start` in the Task Index
4. Check that all `Dependencies` are marked `✅ DONE`
5. Claim it: set `Status` → `🔄 IN PROGRESS`, fill `Owner` and `Start`
6. `git pull --rebase` again and check if your task was taken
7. Commit: `git add .plan/000.0-OCI-Jail-TOC.md && git commit -m "Claim task <ID>" && git push`

### Completing a Task
1. Implement the task following the plan document
2. **Run all unit tests** — fix any failures, even in other agents' code
3. Mark complete: set `Status` → `✅ DONE`, fill `End`, update `Notes`
4. Commit: `git add -A && git commit -m "Complete task <ID>: <desc>" && git push`
5. Move to the next task

### Handling Merge Conflicts
1. Check if your task was taken by another agent (look at `Owner`)
2. If taken, abandon and pick a different task
3. If not taken, resolve the conflict, keep both changes if they affect different tasks
4. `git add <file> && git rebase --continue && git push`

> **Full details:** See [`000.1-Agent-Workflow.md`](.plan/000.1-Agent-Workflow.md)

---

## Reading Order

For a new agent, read the documents in this order:

1. **This file** (`AGENTS_START_HERE.md`) — You are here
2. **[`000.1-Agent-Workflow.md`](.plan/000.1-Agent-Workflow.md)** — How to work on tasks
3. **[`001.0-Overview.md`](.plan/001.0-Overview.md)** — The big picture
4. **[`001.1-Implementation-Phases.md`](.plan/001.1-Implementation-Phases.md)** — Phases and tasks
5. **[`002.0-CLI-Spec.md`](.plan/002.0-CLI-Spec.md)** — CLI commands
6. **[`A.0-Glossary-and-Appendix.md`](.plan/A.0-Glossary-and-Appendix.md)** — Technical terms

Then dive into the specific phase you're working on.

---

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Isolation | FreeBSD jail | Native, well-tested, no emulation overhead |
| Storage | ZFS datasets | Snapshots, clones, compression, encryption |
| Networking | VNET + epair | Full network stack per container |
| Runtime | Native `ocifbsd` | No systemd dependency, minimal attack surface |
| Orchestration | Custom simplified format | 50 lines vs 200+ for equivalent K8s config |
| Clustering | Gossip + Raft | Eventual consistency for runtime, strong for critical state |
| TLS | mTLS everywhere | All cluster communication encrypted |
| Secrets | Encrypted ZFS datasets | Leverage ZFS encryption, simple backup |

---

## Quick Reference

### Key Files

| File | Purpose |
|------|---------|
| `usr.sbin/ocifbsd/ocifbsd.c` | CLI entry point |
| `usr.sbin/ocifbsd/container.c` | Container lifecycle |
| `usr.sbin/ocifbsd/oci2jail.c` | OCI spec to jail param translation |
| `usr.sbin/ocifbsd/state.c` | State persistence |
| `usr.sbin/ocifbsd/hooks.c` | OCI hooks execution |
| `usr.sbin/ocifbsd/utils.c` | Utility functions |
| `usr.sbin/ocifbsd/include/ocifbsd.h` | Common header |
| `etc/ocifbsd/` | Configuration directory |
| `var/run/ocifbsd/` | Runtime state directory |
| `var/lib/ocifbsd/` | Data directory (images, volumes) |

### Key Directories

| Directory | Purpose |
|-----------|---------|
| `usr.sbin/ocifbsd/` | Main CLI daemon |
| `usr.sbin/ocifbsd/image/` | Image management module |
| `usr.sbin/ocifbsd/network/` | Networking module |
| `usr.sbin/ocifbsd/orchestrator/` | Orchestration daemon |
| `usr.sbin/ocifbsd/cluster/` | Clustering daemon |

### Key Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `OCIFBSD_STATE_DIR` | `/var/run/ocifbsd` | Runtime state |
| `OCIFBSD_DATA_DIR` | `/var/lib/ocifbsd` | Image/volume data |
| `OCIFBSD_CONFIG_DIR` | `/etc/ocifbsd` | Configuration |
| Default registry | `docker.io` | Default image registry |

---

## Implementation Phases

**All phases 0-18 are now COMPLETE!**

| Phase | Description | Tasks | Status |
|-------|-------------|-------|--------|
| 0 | Foundation and Setup | 0.1-0.6 | ✅ DONE |
| 1 | OCI Runtime Core (`ocifbsd`) | 1.1-1.27 | ✅ DONE |
| 2 | Image Management | 2.1-2.12 | ✅ DONE |
| 3 | Networking | 3.1-3.10 | ✅ DONE |
| 4 | Resource Limits and Security | 4.1-4.8 | ✅ DONE |
| 5 | Integration and Polish | 5.1-5.11 | ✅ DONE |
| 6 | Orchestration | 6.1-6.20 | ✅ DONE |
| 7 | Config Conversion | 7.1-7.18 | ✅ DONE |
| 8 | Namespace/Resource Management | 8.1-8.29 | ✅ DONE |
| 9 | Clustering Infrastructure | 9.1-9.20 | ✅ DONE |
| 10 | Security/Credentials/WebUI | 10.1-10.18 | ✅ DONE |
| 11 | Node Authentication | 11.1-11.20 | ✅ DONE |
| 12 | Cluster UX (TPM removed) | 12.9-12.20 | ✅ DONE |
| 13 | PAM/System Credentials | 13.1-13.18 | ✅ DONE |
| 14 | Observability/Logging | 14.1-14.20 | ✅ DONE |
| 15 | API Specification | 15.1-15.20 | ✅ DONE |
| 16 | Garbage Collection | 16.1-16.20 | ✅ DONE |
| 17 | Certificate Management | 17.1-17.35 | ✅ DONE |
| 18 | Cloud Export | 18.1-18.20 | ✅ DONE |

### What's Next?

The implementation is complete. Focus areas:
- **Testing**: Run the ATF test suite
- **Integration**: Build and test on a FreeBSD system
- **Documentation**: Fill in any missing details in the plan documents
- **Bug fixes**: Any issues found during testing

---

## Need Help?

If you encounter issues:
1. Check the relevant plan document for guidance
2. Check the task's `Notes` column for known issues
3. Mark the task as `🟡 BLOCKED` with the reason
4. Commit and push so other agents know
5. Ask for guidance

> **Remember:** The goal is to build a secure, simple, and powerful container runtime for FreeBSD that rivals Kubernetes in capability while being much simpler to use. Every task should bring us closer to that goal.

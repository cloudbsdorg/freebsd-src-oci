# ocifbsd — Hardening & Productionization Roadmap

Status: draft. Sequenced by dependency and leverage — earlier phases unblock and
de-risk later ones. Each work item lists **Why** (the real gap it closes),
**Do** (concrete work), **DoD** (definition of done). Cross-cutting rule for
every item: red-green tests, live validation on the cluster, blog the result,
commit + merge FreeBSD upstream, push.

Legend — effort: S (≤1 day) · M (2–4 days) · L (1–2 weeks) · XL (multi-week).

---

## Phase 0 — Clean build, CI, packaging  *(unblocks everything)*

0.1 **Green -Werror build** · S
   - Why: the tree only builds with `WARNS=0` (pre-existing lint: unused vars,
     shadowed globals, `random()` visibility). We currently ship a binary built
     on 15.1 and run it on 16.0.
   - Do: fix each warning at the source; restore `WARNS`; standardize the build
     on FreeBSD 16.0-CURRENT (match the runtime).
   - DoD: `make` is clean with default WARNS on a 16.0 host.

0.2 **CI pipeline** · M
   - Do: build + unit tests + a smoke test (create/start/exec/stop a container,
     build an image, run the proxy) on every push; artifact the binary.
   - DoD: red/green gate on the branch; no manual builds.

0.3 **pkgbase packaging** · M
   - Why: distribution today is hand-copied 4.6 GB image tarballs and scp'd
     binaries.
   - Do: package `ocifbsd` + rc.d scripts + man pages as a pkgbase package;
     publish images to a local OCI registry instead of tar-over-ssh.
   - DoD: `pkg install ocifbsd` on a fresh node; `ocifbsd pull` from the registry.

---

## Phase 1 — Container lifecycle & state  *(removes most operational hacks)*

1.1 **Real restart + durable netcfg** · M  — *highest-leverage fix*
   - Why: a `stopped` container can't be started (only `created` can), and
     `delete` wipes the per-name network config. This single design choice
     caused nearly all the reboot/recovery pain and the fragile
     delete→create→network-set→start dance.
   - Do: add `ocifbsd restart`; make stop→start round-trip; persist netcfg
     keyed to the container name/id so it survives stop/delete/recreate.
   - DoD: stop then start keeps the container's IP; `restart` works from any state.

1.2 **First-class restart policy** · S
   - Why: the shell `ocifbsd-supervise` reconciler is bolted on.
   - Do: `ocifbsd run --restart no|on-failure|always`, stored in container
     config and enforced by a runtime-owned supervisor (fold the shell script in).
   - DoD: a crashed jail restarts per policy with no external script.

1.3 **Boot-time state via rc.d, not hand-edited rc.local** · S
   - Why: routes, `ip.forwarding`, pf rules, and the pod bridge gateway were
     restored by hand-patched `rc.local` after the reboot.
   - Do: a proper `ocifbsd` rc.d service that restores pod networking + starts
     the supervisor + declared containers at boot.
   - DoD: a full node reboot restores networking + containers with zero manual steps.

---

## Phase 2 — Native orchestration control loop  *(the big one)*

2.1 **Wire the Ensemble parser fully into `stack`** · L
   - Why: `stack create -f` was a stub; deployment is currently a shell engine
     (`ocifbsd-stack-deploy`) that writes supervisor manifests.
   - Do: parse the manifest natively into the stack/service model; retire the
     shell engine.
   - DoD: `ocifbsd stack up -f` deploys with no shell helper.

2.2 **Native cross-node dispatch** · L
   - Why: `service create --replicas N` places all replicas on the local node;
     the scheduler + `ocifbsd-cluster` daemon exist but don't distribute.
   - Do: finish `clustering/` — scheduler writes replica→node assignments to
     shared cluster state; each node's daemon reconciles its assignments.
   - DoD: a 3-replica service lands one-per-node natively.

2.3 **Stateful workloads = the manifest's job** · L
   - Why: Galera bootstrap ordering and Redis cluster formation are separate
     scripts, not driven by the manifest. "Deploy from one file" is ~80% true.
   - Do: lifecycle hooks / an operator concept in the manifest (bootstrapNode,
     health-gated join, cluster-form step); the control loop executes them.
   - DoD: `ocifbsd stack up -f` on empty nodes yields a formed Galera + Redis
     cluster with no manual bootstrap.

2.4 **Manifest is the complete source of truth** · M
   - Do: the parser reads every field it accepts; schema validation on load.
   - DoD: nothing about the running cluster exists outside the manifest.

---

## Phase 2.5 — Secure control-plane API service  *(the "API service" ask)*

The capability a Kubernetes-style API server provides — a versioned, declarative,
authenticated, authorized cluster API with watch/reconcile — under ocifbsd's own
naming (no external branding). Today ocifbsd is driven node-by-node over SSH +
the CLI; there is no central control plane. Depends on Phase 2 (reconcile loop)
and Phase 6 (security daemon).

A.1 **API server** · XL
   - Do: versioned REST/gRPC over the Ensemble resource model (Stack, Service,
     Deployment, StatefulSet, Pod, ConfigMap, Secret, …). CRUD + **watch**
     (streamed changes) so controllers and clients react to state.
   - DoD: every resource type is create/read/update/delete/watchable over the API.

A.2 **Replicated state store** · L
   - Do: the persistent, leader-elected source of truth the control loop
     reconciles against (raft-style consensus or an embedded replicated KV).
   - DoD: desired state survives loss of any single control node.

A.3 **AuthN** · M
   - Do: mTLS for components; short-lived certs / bearer tokens for users
     (builds on Phase 6). Pinned peer identity for node agents.
   - DoD: no unauthenticated call touches the API.

A.4 **AuthZ (RBAC) + namespaces** · M
   - Do: roles, role-bindings, namespaces; least-privilege by default.
   - DoD: a scoped token can only act within its granted verbs/resources.

A.5 **Admission + audit** · M
   - Do: validation, defaulting, and policy on writes; an append-only audit log
     of every mutation (who/what/when).
   - DoD: invalid specs are rejected at the API; all changes are auditable.

A.6 **Node agents talk to the API** · L
   - Do: each node runs an agent that authenticates to the API, watches its
     assignments, and reconciles local containers/networking (folds in the
     supervisor + cluster daemon).
   - DoD: nodes are driven by the API, not by SSH.

---

## Phase 3 — Networking automation

3.1 **Automated routed pod fabric** · M
   - Why: distinct per-node subnets + inter-node routes + pf are hand-built.
   - Do: derive and apply the routed fabric (subnets, routes, forwarding, NAT
     exclusions) from the manifest; persist it.
   - DoD: adding a node needs only a manifest edit + `stack up`.

3.2 **Container IPv6** · M
   - Why: v6 is ingress-only; containers have no v6.
   - Do: dual-stack pod networks + routed v6 (the ::1 gateway path already works
     at the node level).
   - DoD: containers reachable over v6 end-to-end.

3.3 **Port-driven pf/rdr management** · S
   - Do: publish/expose ports declaratively; ocifbsd manages the rdr rules.
   - DoD: no hand-written `pf.oci.conf`.

---

## Phase 4 — Service discovery, health, dynamic proxy

4.1 **First-class labels/tags** · S
   - Do: `ocifbsd run --label k=v`, stored + queryable (`ocifbsd ps -l k=v`).
4.2 **Health probes** · M
   - Do: TCP/HTTP readiness+liveness per service; unhealthy = out of rotation.
4.3 **Live discovery + dynamic proxy** · M
   - Why: proxy backends resolve only at deploy time.
   - Do: proxy watches the cluster state / label selector and adds/drops
     backends live (with the circuit-breaker already in place).
   - DoD: scaling a service changes the proxy's backends without a redeploy.

---

## Phase 5 — HA / remove single points of failure

5.1 **Proxy HA** · M
   - Why: the L4 proxy runs only on fb16-1.
   - Do: proxy on every node + a CARP VIP so the LB itself fails over.
5.2 **Redundant ingress** · M
   - Why: TLS ingress + the aggregator run only on freedev007.
   - Do: multiple TLS terminators behind CARP (or Cloudflare LB); run the
     status aggregator as a replicated/leader-elected service.
5.3 **Data-tier redundancy** · M
   - Do: Redis replicas + automatic failover (today: 3 masters, no replicas —
     lose a master, lose a shard). Galera read-replicas / arbitrator for the
     write-scaling ceiling the stress test found (~30 uncacheable req/s).

---

## Phase 6 — Security hardening

6.1 **Secrets management** · M
   - Why: CF token in `ddclient.conf`, DB passwords in `wp-config`, plaintext.
   - Do: a secrets store; inject at runtime; rotate.
6.2 **Turn the security daemon on** · M
   - Why: mTLS/JWT/auth exist in-tree (22 review batches) but the lab runs open.
   - Do: mTLS between cluster nodes; authenticated API; pinned peer identity.
6.3 **Least-privilege access** · S
   - Do: replace passwordless root SSH between nodes with scoped keys / an agent.
6.4 **Supply chain** · M
   - Do: image signing + verification on pull.

---

## Phase 7 — Observability

7.1 **Metrics** · M — Prometheus-format endpoint from `ocifbsd stats` + host.
7.2 **Central logs** · M — ship `logd` output to a central store.
7.3 **Alerting** · S — page on Galera size < N, Redis not ok, node/proxy down.
   - Why: the freedev007 reboot outage was invisible until a human noticed.
7.4 **Dashboard on real metrics** · S — the Machine Room reads the metrics store.

---

## Phase 8 — Product, docs, tests

8.1 **Rebuild oci.cloudbsd.org handbook** · S — fold the surviving 58 KB docs
    into the marketing/handbook site; keep it a designated fragment updater
    (never clobber the home again).
8.2 **E2E + mobile tests in CI** · M — CDP-driven (Playwright has no FreeBSD
    build); assert reachability, render, hamburger, no horizontal overflow.
8.3 **Ensemble format spec + validator** · S — document the schema; validate on load.

---

## Phase 9 — User tooling & client experience  *(what operators actually touch)*

The runtime CLI exists, but the operator experience is thin — no config/context
management, no declarative apply against the API, no auth flow, no ergonomic
inspection. This is the client half of Phase 2.5.

9.1 **Declarative client** · M
   - Do: `ocifbsd apply / get / describe / delete -f` against the API, with
     diff, `--dry-run`, and server-side apply.
   - DoD: an operator manages the cluster declaratively, never editing state by hand.

9.2 **Config & multi-cluster contexts** · S
   - Do: `~/.ocifbsd/config` (clusters, users, contexts);
     `ocifbsd config use-context`; talk to prod/lab from one client.
   - DoD: switching clusters is one command; no re-auth per call.

9.3 **Auth flow** · M
   - Do: `ocifbsd login` (token/cert issuance), secure credential storage +
     rotation (ties to Phase 6 secrets).
   - DoD: a user authenticates once; the CLI carries scoped, expiring creds.

9.4 **Ergonomic inspection** · M
   - Do: `get … -o wide|json|yaml` (pretty by default, `--compact` for scripts —
     already the runtime rule), `logs -f`, `exec -it`, `top` (live stats),
     `events`, `explain`.
   - DoD: troubleshoot a workload entirely through the CLI without SSH-ing to nodes.

9.5 **Console TUI + web parity** · M
   - Do: a read-only TUI cluster dashboard (console UI — **consult agy** per the
     UI rule) and the web Machine Room reading the same API/metrics.
   - DoD: one source of truth behind CLI, TUI, and web.

9.6 **Polish** · S
   - Do: shell completion, full `--help`, a man page per subcommand, stable
     scriptable output contracts, and a client SDK/library so other tools drive
     the API.
   - DoD: tab-completion works; every subcommand is documented; an SDK exists.

---

## Phase 10 — Capstone: rebuild on the new platform + retell the adventure

The final proof. Once Phases 0–9 land, tear down and **reimplement both the
single-node VM and the 3-node cluster from scratch using only the new platform**
— the native orchestrator, the secure API, and the client tooling — with zero
hand-SSH steps. This is the ultimate dogfood: if the platform can rebuild the
sites that document it, from one manifest through the API, it is real.

10.1 **Back up everything first** · S  — *non-negotiable, data-loss is unacceptable*
   - Do: full logical dumps of both WordPress databases (single + cluster) and
     the `wp-content` uploads, copied off-node and checksummed, **before**
     touching any VM. (We lost the oci.cloudbsd.org home once by overwriting
     without a backup — never again.)
   - DoD: verified, restorable backups of all site data exist off-host.

10.2 **Reimplement the deployments from the new tooling** · L
   - Do: recreate the single VM and the cluster purely via
     `ocifbsd apply -f …` against the secure API — networks, Galera, Redis
     Cluster, web tier, proxy, all declared and reconciled, no manual bootstrap
     or SSH plumbing. Fresh nodes, clean state.
   - DoD: both deployments come up green from the manifest through the API alone.

10.3 **Restore and verify the data** · M
   - Do: load the backed-up databases + uploads into the rebuilt stacks; verify
     post-for-post and page-for-page parity, session migration, and byte-for-byte
     content correctness on both sites.
   - DoD: the live sites are indistinguishable from before the rebuild; nothing lost.

10.4 **Retell the adventure** · M
   - Do: update the journey/blog with a fresh **adventure series** — a story for
     **every major step** taken to build this new material: the lifecycle fix,
     native orchestration, the secure control-plane API + RBAC, the state store,
     the node agents, discovery/health, HA/CARP, secrets + mTLS, observability,
     the tooling, and this from-scratch rebuild. Same warm, human voice as the
     existing posts; grounded in real commits and real numbers. Publish to both
     sites; keep the local-time + REVYTECH/Mark LaPointe footer.
   - DoD: the sites carry a complete, engaging narrative of the whole journey —
     the platform, rebuilt on itself, telling the story of how it got there.

---

## Suggested execution order

**Spine:** Phase 0 → 1 → 2 (build / lifecycle / native orchestration). Then the
control plane splits into two tracks that proceed together:

- **Control-plane track:** Phase 6 (security daemon) → Phase 2.5 (secure API +
  state store + RBAC + node agents) → Phase 9 (client tooling on top of the API).
  The API needs security (6) and the reconcile loop (2) under it, and the tooling
  needs the API (2.5) in front of it.
- **Platform track:** Phase 3 (networking) → 4 (discovery/health/dynamic proxy)
  → 5 (HA / kill SPOFs) → 7 (observability). These harden the data path.

Phase 8 (docs/tests/site) runs continuously alongside.

**If prioritizing by pain-removed-per-effort, do these first:** 1.1 (restart +
durable netcfg), 1.3 (rc.d boot state), 0.1 (clean build). They delete most of
the fragility everything else is papering over. **The headline builds** are
Phase 2 (native orchestration), Phase 2.5 (the secure API service), and Phase 9
(the tooling) — that trio is what turns this from an impressive lab into an
operable, Kubernetes-class platform with ocifbsd's own identity.

Total scope is genuinely large (multiple XL/L items). It is best run as a series
of focused, independently-shippable increments — each landing green tests, a
live demo, a blog write-up, and an upstream merge — rather than one big-bang
effort.

**Phase 10 is the finale:** with the platform built, rebuild the single-node and
cluster deployments from scratch through the new API + tooling (data backed up
first, restored and verified after), then retell the whole journey as a fresh
adventure series on the sites. The platform proving itself by rebuilding the very
sites that document it — and narrating how it got there — is the closing chapter.

# ocifbsd security audit — 2026-09-01

Scope: ocifbsd's own C sources under `usr.sbin/ocifbsd/` (excluding vendored
`contrib/`). Method: reviewed the dangerous primitives (`execv`/`system`/`popen`,
`sprintf`/`strcpy`/`strcat`, `mount`/`open`/`stat`/`realpath`, the gossip/Raft
wire path) and read the surrounding code in the highest-risk modules.

Severity legend: 🔴 critical (remote root/cluster compromise) · 🟠 medium ·
Status: **FIXED**, **OPEN**, or **LATENT** (not currently reachable).

## Findings

### 🔴 1. Image-layer symlink traversal → arbitrary host write as root — FIXED
`image/unpack.c`. Entry *names* were validated (no absolute / `..`) and the leaf
opened `O_NOFOLLOW`, but the *parent* path was not protected: a hostile layer
could plant a symlink entry (`etc` → `/`) and then a file entry (`etc/crontab`);
`mkdirp`/`open` traversed the symlinked parent and wrote to the **host** as root
during `image pull`. **Fix:** all creation now happens beneath an open handle to
the layer root, each component resolved `O_RESOLVE_BENEATH` (`open_dir_beneath`),
so a symlink that escapes the root fails with `ENOTCAPABLE`; in-rootfs symlinks
(which real multi-layer images use) still work. Regression test
`unpack_rejects_symlink_escape` builds the exploit layer and asserts the
through-symlink write is blocked; all 7 unpack tests pass.

### 🔴 2. Cluster consensus over unauthenticated UDP → cluster-wide RCE — OPEN (top follow-up)
`clustering/cluster.c`. The gossip + Raft transport is a plain UDP socket with
no authentication, HMAC, or mTLS. Any host that can reach the port can inject a
`JOIN` (rogue membership) or a forged `APPEND_REQ` carrying a Raft log command
(`CREATE <svc> <n> <image>`, `ASSIGN`, …); committed commands are applied by
`cp_apply()` and schedule/run containers, so an unauthenticated packet becomes
**code execution on every node**. Wire fields are correctly bounds-checked
(no over-read) — the defect is the missing authentication. The `cluster_mtls`
module already exists (`clustering/cluster_mtls.c`, correct `SSL_VERIFY_PEER` +
`FAIL_IF_NO_PEER_CERT`) but is **not wired into this path**. **Fix (planned):**
carry gossip/Raft over the mTLS channel, or at minimum authenticate every
datagram with an HMAC keyed by a cluster join secret, and drop unauthenticated
packets before any state mutation. Until then, the clustering ports must be
confined to a trusted network by deployment (documented operational control).

### 🟠 3. MAC-label command injection in namespace apply — FIXED
`namespace/namespace.c`. `ns_apply_mac_label()` runs `system("jail -m label=%s …")`
with the label; the label was stored unvalidated, so `x; touch /pwned;` was root
command injection. **Fix:** `ns_set_mac_label()` now rejects any label containing
a character outside the MAC-label set (alphanumerics and `/ - _ . : ,`),
excluding every shell metacharacter. (The container/namespace *name* was already
validated by `ns_name_is_valid`, which covers the many other `system()` builders
in this file.)

### 🟠 4. Unvalidated identifiers into openssl `system()`/`popen()` — LATENT
`security-daemon/auth.c` (`cert_generate_ca`, `cert_generate_node`,
`cert_check_expiry`). `name`/`node_id` are interpolated into `openssl …` shell
strings run as root. No in-tree caller today, so not presently reachable, but
it is exported API that will bite once a CSR/join flow passes peer-supplied
identifiers. **Fix (planned):** drive `openssl` via `fork`/`execv` with an argv
array (or validate identifiers) before any caller is added.

### 🟠 5. `output_path` injection in image export — OPEN (low real-world risk)
`export/export.c` builds `system("dd … of=%s", output_path)` and a `qemu-img …`
string from the operator-supplied `output_path`. Mostly self-injection (the
operator already runs the tool), but a destination like `/x; reboot` executes.
**Fix (planned):** `fork`/`execv` `dd`/`qemu-img` with argv.

### 🟠 6. Path-safety checks are lexical-only — PARTIALLY ADDRESSED
`src/oci2jail.c` `oci_path_is_safe()` and `image/unpack.c` `entry_path_is_safe()`
reject literal `..` but do not resolve symlinks. For image extraction this is now
moot (finding #1 resolves symlinks at open time). For OCI mount destinations a
residual TOCTOU remains — but jail creation/mount uses the `jailparam`/`nmount`
syscall API (no shell) and the destination stays lexically under the root, so
the exposure is limited. Config-time traversal is rejected by `oci_validate_spec`
(mount destinations, readonlyPaths, maskedPaths). Closing it fully would reuse a
symlink-safe resolver at mount time.

## Verified clean

- **Privilege drop** (`src/container.c`): `setgroups`→`setgid`→`setuid`, each
  `_exit`ing on failure, after `jail_attach`. Order correct.
- **TLS/registry** (`image/pull.c`, `image/push.c`): `SSL_VERIFYPEER=1` +
  `VERIFYHOST=2` on every request; HTTP is opt-in per-registry (localhost only by
  default); layer digests verified after download.
- **Jail creation** (`src/oci2jail.c`): `jailparam`/`jail_set` syscall API — no
  shell, so no injection via jail params or mount options.
- **Format strings**: all `printf`/`syslog`/`fprintf` use literal formats.
- **Unsafe string primitives**: the residual `sprintf` calls write fixed-width
  hex/`\uXXXX` into adequately sized buffers; the codebase otherwise uses
  `strlcpy`/`snprintf`. `gc/gc.c` `popen` builders interpolate only compile-time
  constants.
- **Gossip parsing** (`cluster.c`): NUL-terminates fixed fields and bounds every
  payload reinterpret against the received length (the problem there is auth, not
  memory safety).
- **Hooks** (`src/hooks.c`): `execve` with explicit argv/envp (no shell).

## Priority

Finding **#2 (unauthenticated consensus)** is the top remaining item — it is a
design change (wire `cluster_mtls`/HMAC into the gossip/Raft path) and until it
lands the clustering ports must be network-confined by deployment.

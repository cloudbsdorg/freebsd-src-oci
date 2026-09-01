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

### 🔴 2. Cluster consensus over unauthenticated UDP → cluster-wide RCE — FIXED (opt-in auth)
`clustering/cluster.c`. The gossip + Raft transport is a plain UDP socket; a
forged `APPEND_REQ` carrying a Raft command (`CREATE <svc> <n> <image>`, …) was
applied by `cp_apply()` and ran the attacker's container **on every node** —
network-unauthenticated cluster-wide code execution. **Fix:** every gossip/Raft
datagram now carries a trailing HMAC-SHA256 tag keyed by the configured cluster
key, computed at the single send choke point (`gossip_send_message`) and
verified in constant time (`CRYPTO_memcmp`) at the single receive choke point
(`gossip_handle_message`) **before any state is touched**; forged, tampered,
wrong-key and untagged (raw-injection) packets are dropped. Consistent with the
project's open-default posture, authentication is **opt-in via the cluster key**
(`OCIFBSD_CLUSTER_KEYFILE`, or `OCIFBSD_CLUSTER_KEY`) — with a key the cluster is
authenticated; with none it runs unauthenticated and **warns loudly at startup**
to confine the port or set a key. Verified: keyed vs unkeyed startup messages,
and a deterministic test showing valid packets accepted while
tampered/wrong-key/raw-injection are rejected. See
`docs/security-restrictions.md` for how to set the key. (The `cluster_mtls`
module remains available for a future full-mTLS transport; HMAC is the bounded
fix appropriate to a UDP protocol.)

### 🟠 3. MAC-label command injection in namespace apply — FIXED
`namespace/namespace.c`. `ns_apply_mac_label()` runs `system("jail -m label=%s …")`
with the label; the label was stored unvalidated, so `x; touch /pwned;` was root
command injection. **Fix:** `ns_set_mac_label()` now rejects any label containing
a character outside the MAC-label set (alphanumerics and `/ - _ . : ,`),
excluding every shell metacharacter. (The container/namespace *name* was already
validated by `ns_name_is_valid`, which covers the many other `system()` builders
in this file.)

### 🟠 4. Unvalidated identifiers into openssl `system()` — FIXED
`security-daemon/auth.c` (`cert_generate_ca`, `cert_generate_node`).
`name`/`node_id` were interpolated into `openssl …` shell strings run as root —
latent (no in-tree caller) but exported API that would bite once a CSR/join flow
passes peer-supplied identifiers. **Fix:** `auth_id_is_safe()` now validates
each identifier to the cert-name charset (alphanumerics and `- _ .`), rejecting
every shell metacharacter, at the entry of both functions.

### 🟠 5. `output_path` injection in image export — FIXED
`export/export.c` `export_to_raw`/`export_to_qcow2` built `system("dd … of=%s")`
and a `qemu-img …` string from the operator-supplied `output_path`. **Fix:** both
now run `dd`/`qemu-img` via `fork`+`execvp` with an argv array (helper
`export_run`), so paths are passed as single arguments and can never be
interpreted as shell syntax; the temp raw image is removed with `unlink()`
instead of `&& rm`.

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

## Remaining

Findings #1–#5 are fixed. Only **#6** remains — the path-safety checks are
lexical (no symlink resolution); it is residual and low: image extraction now
resolves symlinks at open time (#1), config-time traversal is rejected by
`oci_validate_spec`, and mounts use the `nmount` syscall API (no shell). Nothing
remotely triggerable is open.

# FreeBSD build caching + agent-readable regression

> Lab note for `ocifbsd` / FreeBSD `src` work on CloudBSD fredev hosts.  
> Complements `.plan/004.0-Testing.md` and `.plan/021.0` §8.  
> Date: 2026-07-22

---

## 1. Faster FreeBSD system / subtree builds (with caching)

FreeBSD already has first-class hooks for compiler caching and dependency-aware incremental builds. Prefer **in-tree knobs** over inventing wrappers.

### 1.1 Parallelism (always)

```sh
NCPU=$(sysctl -n hw.ncpu)
make -j$((NCPU))                 # full world/kernel
make -j$((NCPU)) -C usr.sbin/ocifbsd
```

Handbook guidance: try between ~½× and 2× cores; measure. On fredev005/006 (64 cores), use high `-j` for **subtree** builds; for full `buildworld`, watch memory/IO.

### 1.2 `ccache` via FreeBSD’s `WITH_CCACHE_BUILD` (primary cache)

In-tree support lives in `share/mk/bsd.compiler.mk` (`MK_CCACHE_BUILD`). When enabled, make prepends `/usr/local/bin/ccache` to `CC`/`CXX`.

**Install** (CURRENT pkg names):

```sh
doas pkg install -y ccache4
# or older: ccache / ccache-static
# FreeBSD src expects ${LOCALBASE}/bin/ccache and optionally
# ${LOCALBASE}/libexec/ccache as wrapper path
```

**Enable for src / subtree** (do **not** commit host-specific paths into FreeBSD tree):

```sh
# Environment or /etc/src.conf / make.conf on the *lab host only*
export WITH_CCACHE_BUILD=yes
# Optional: stable keys across path layouts
export CCACHE_DIR=/var/cache/ccache-ocifbsd   # or $HOME/.ccache
export CCACHE_BASEDIR=$HOME/git/freebsd-src-oci
export MAKEOBJDIRPREFIX=$HOME/obj/freebsd-src-oci

make -j$(sysctl -n hw.ncpu) -C usr.sbin/ocifbsd WITH_CCACHE_BUILD=yes
```

Notes from FreeBSD’s integration:

- Uses **content** hashing for in-tree compilers when appropriate (`CCACHE_COMPILERCHECK`).
- Clears bogus `CCACHE_PATH` so the in-tree compiler is not bypassed.
- `CCACHE_BASEDIR` + consistent `MAKEOBJDIRPREFIX` improve cache hit rates when the checkout path moves.
- First clean build fills the cache; second build should show high hit rate (`ccache -s`).

**Handbook also recommends** `devel/ccache` when repeatedly building custom pkgbase packages.

### 1.3 `sccache` (optional)

`sccache` is available in ports and supports remote/shared caches. FreeBSD base does **not** wire it as tightly as ccache. Prefer ccache for FreeBSD `src` unless you already run a sccache server farm. Can wrap manually:

```sh
export RUSTC_WRAPPER=sccache   # not relevant to C base
# For C, typically: CC="sccache cc" — less tested with FreeBSD’s MK_CCACHE_BUILD
```

### 1.4 META_MODE + `filemon` (incremental correctness)

FreeBSD’s **meta mode** tracks full command lines and file dependencies via **filemon(4)** so rebuilds skip work that truly does not need redoing.

- fredev005 already had `filemon.ko` loaded.
- Controlled via `MK_META_MODE` / `META_MODE` in `share/mk` (see `src.sys.env.mk`, `bsd.init.mk`).
- Without filemon, meta mode is less useful (build system warns).

Typical use is for large incremental **world** builds; for `ocifbsd` alone, ccache + `-j` usually dominates.

### 1.5 Object directories (do not pollute the tree)

```sh
export MAKEOBJDIRPREFIX=$HOME/obj/freebsd-src-oci
# or per-host: /usr/obj/...
```

Avoid building with objects mixed into the source tree when possible. Keep one obj prefix per branch if switching often.

### 1.6 Clean vs incremental

| Goal | Command / practice |
|------|-------------------|
| Fast iterate on `ocifbsd` | `-jN`, `WITH_CCACHE_BUILD=yes`, no `clean` |
| Suspicious build | `make -C usr.sbin/ocifbsd clean all` |
| Full world reset | `make cleanworld` then rebuild (destroys cache benefit for world) |

### 1.7 Multi-host

Handbook “Tracking for Multiple Machines”: one **build host** NFS-exports `/usr/obj` (or obj prefix); others mount read-only. For CloudBSD fredev, simpler pattern:

- fredev005/006 = burst compile with ccache on local NVMe/ZFS  
- fredev001 = durable tree + larger disk  
- Optional later: shared `CCACHE_DIR` on NFS (watch locking/latency)

### 1.8 What not to do

- Do not require ccache in FreeBSD **upstream** patches; keep it **lab `src.conf` / env**.  
- Do not commit `obj/` or `.ccache` into git.  
- Do not use Linux-only distributed compilers without measuring; FreeBSD native ccache first.

---

## 2. Inspecting test results so agents “understand everything”

### 2.1 FreeBSD’s native stack: ATF + Kyua

| Role | Tool |
|------|------|
| Write tests | ATF (`atf-c`, `atf-c++`, `atf-sh`) |
| Run / store / report | **kyua** (`FreeBSD-kyua` on CURRENT pkgbase) |
| Suite entry | `Kyuafile` under `tests/...` |

Our current suite:

```text
tests/usr.sbin/ocifbsd/   # parser_test, k8s_test; ocifbsd_test.c.disabled
```

### 2.2 Run once, inspect many ways

```sh
cd tests/usr.sbin/ocifbsd
make
kyua test -k Kyuafile
# Results DB: ~/.kyua/store/results.<suite>.<timestamp>.db
```

**Human / agent inspection:**

```sh
# Pass/fail summary + context
kyua report --verbose -r LATEST

# Only failures (default filter often skips "passed")
kyua report -r LATEST --results-filter=failed,broken,xfail,skipped

# Machine-readable for CI / agents (preferred for Grok)
kyua report-junit -r LATEST --output=/tmp/ocifbsd-junit.xml

# HTML tree for humans
kyua report-html -r LATEST --output=/tmp/ocifbsd-html --force

# SQL over the results DB
kyua db-exec -r LATEST \
  "SELECT test_case_id, result_type, result_reason FROM test_results WHERE result_type != 'passed';"
```

**Why this matters for agents:**  
JUnit XML + verbose report give **structured** status, timing, stdout/stderr, and metadata (`required_user`, `timeout`, descriptions). Prefer those over scraping TTY color.

### 2.3 Debug a single failing case

```sh
kyua debug -k Kyuafile parser_test:yaml_escape_null
# or:
kyua test -k Kyuafile parser_test:yaml_escape_null
```

### 2.4 Full FreeBSD base regression (when needed)

Installed suite (if present):

```sh
cd /usr/tests
kyua test
# subset:
kyua test usr.bin/grep
```

For **ocifbsd**, full base regression is optional background; product gate is **our** suite + integration tests, not all of `/usr/tests`.

---

## 3. PTY / interactive / CLI testing on FreeBSD

“PTY tests” mean: exercise **real CLI** as a user would, including programs that open a pseudo-terminal (interactive shells, pagers, line discipline), not only pure library calls.

### 3.1 Prefer non-PTY when possible

FreeBSD ATF style for CLIs is often **`atf_check`** with expected exit/stdout/stderr files (see `bin/sh` functional tests): deterministic, fast, no races.

```sh
atf_check -s exit:0 -o match:"create" -e empty \
  ocifbsd --help
```

Use this for Phase 1 CLI surface (`--help`, `create` error paths, `state` JSON).

### 3.2 When you need a real TTY

Patterns used in FreeBSD / ATF ecosystems:

| Technique | Use when |
|-----------|----------|
| **`atf_check` + subprocess** | CLI that does not require a tty |
| **`script(1)` / `pty` helpers** | Capture “looks like a terminal” sessions |
| **`openpty(3)` / `forkpty(3)` in C ATF** | Full control of master/slave; send bytes, assert output |
| **`expect`-style** (ports) | Heavy interactive dialogs — avoid if possible (extra dep) |
| **Jail integration ATF** | `require.user=root`, create real jail, `jexec`, cleanup in ATF cleanup |

For **ocifbsd**:

1. **Unit:** `oci2jail` / state JSON with ATF-C (no jail).  
2. **CLI:** `atf_check` against `ocifbsd` binary paths.  
3. **Integration (root):** ATF metadata `required_user=root`, create disposable rootfs under `/tmp` or ZFS clone, run create/start/state/kill/delete, cleanup always.  
4. **PTY only if** we add interactive attach/console (`ocifbsd attach`) that requires a terminal.

### 3.3 Jail-aware ATF metadata

ATF supports metadata such as:

- `required_user`  
- `required_programs`  
- `timeout`  
- platform/arch filters  

Kyua surfaces these in `report-junit` properties and verbose reports — agents should read failures **with metadata** so “needs root” is not confused with a code bug.

---

## 4. Full regression loop an agent can “understand”

Goal: one command produces artifacts Grok can load without guessing.

### 4.1 Recommended layout (lab, not necessarily upstream yet)

```text
~/git/freebsd-src-oci/
  artifacts/regression/<date-or-gitsha>/
    build.log
    kyua-junit.xml
    kyua-report.txt
    kyua-html/          # optional
    integration.log     # root lifecycle if run
    summary.md          # machine-written one-pager
```

### 4.2 Driver script sketch (agent-executed)

```sh
#!/bin/sh
set -eu
REPO=${REPO:-$HOME/git/freebsd-src-oci}
OBJ=${MAKEOBJDIRPREFIX:-$HOME/obj/freebsd-src-oci}
SHA=$(git -C "$REPO" rev-parse --short HEAD)
OUT=$REPO/artifacts/regression/$SHA
NCPU=$(sysctl -n hw.ncpu)
mkdir -p "$OUT"

export WITH_CCACHE_BUILD=yes
export MAKEOBJDIRPREFIX=$OBJ
export CCACHE_DIR=${CCACHE_DIR:-$HOME/.ccache/ocifbsd}
export CCACHE_BASEDIR=$REPO

{
  echo "sha=$SHA host=$(hostname) date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  make -C "$REPO/usr.sbin/ocifbsd" -j"$NCPU" 2>&1
} | tee "$OUT/build.log"

make -C "$REPO/tests/usr.sbin/ocifbsd" -j"$NCPU"
(
  cd "$REPO/tests/usr.sbin/ocifbsd"
  kyua test -k Kyuafile 2>&1 | tee "$OUT/kyua-test.txt"
  kyua report --verbose -r LATEST --output="$OUT/kyua-report.txt"
  kyua report-junit -r LATEST --output="$OUT/kyua-junit.xml"
)

# Parse for agent summary
python3 - <<'PY' "$OUT/kyua-junit.xml" "$OUT/summary.md"
import sys, xml.etree.ElementTree as ET
from pathlib import Path
xml, out = Path(sys.argv[1]), Path(sys.argv[2])
root = ET.parse(xml).getroot()
cases = list(root.iter("testcase"))
fails = [c for c in cases if c.find("failure") is not None or c.find("error") is not None]
lines = [f"# Regression {xml.parent.name}", f"total={len(cases)} failed={len(fails)}"]
for c in fails:
    lines.append(f"- FAIL {c.get('classname')}.{c.get('name')}")
out.write_text("\n".join(lines) + "\n")
print(out.read_text())
PY
```

### 4.3 How the agent consumes it

1. Read `summary.md` (counts + fail names).  
2. If failures: read `kyua-report.txt` / JUnit body for that case (stdout/stderr + metadata).  
3. `kyua debug` for one case; fix with red–green.  
4. Re-run script; only mark COMPLETED when JUnit shows zero failures for the claimed scope.  
5. Log non-obvious findings to **Honcho**.

### 4.4 Layers of “full” regression for ocifbsd

| Layer | Command | Needs root? | Agent artifact |
|-------|---------|-------------|----------------|
| A. Unit (convert) | `kyua test` parser/k8s | no | junit |
| B. Unit (runtime logic) | ATF for oci2jail/state (to add) | no | junit |
| C. CLI | ATF `atf_check` on `ocifbsd` | no | junit |
| D. Integration lifecycle | ATF/jails create→delete | **yes** | junit + integration.log |
| E. Network/image | later phases | often yes | junit |
| F. Optional base suite | `cd /usr/tests && kyua test` | mixed | separate junit |

Phase 1 gate in 021.0 = **A+B+C+D**, not F.

---

## 5. fredev lab checklist (caching + regression)

On fredev005/006 (burst) or fredev001 (durable):

```sh
doas pkg install -y ccache4 FreeBSD-kyua FreeBSD-atf-dev
# ensure filemon for meta mode if doing large world builds
doas kldload filemon 2>/dev/null || true

export WITH_CCACHE_BUILD=yes
export CCACHE_DIR=$HOME/.ccache/ocifbsd
export CCACHE_BASEDIR=$HOME/git/freebsd-src-oci
export MAKEOBJDIRPREFIX=$HOME/obj/freebsd-src-oci
```

Then run the regression driver; copy `artifacts/regression/<sha>/` back or leave on the host for Grok/`grok-build` to read over SSH.

---

## 6. References

- FreeBSD Handbook: *Updating FreeBSD from Source* (`-j`, clean builds, multi-machine NFS)  
- FreeBSD Handbook / pkgbase notes: ccache for repeated package builds  
- In-tree: `share/mk/bsd.compiler.mk` (`WITH_CCACHE_BUILD`), meta/filemon in `bsd.init.mk`  
- `kyua(1)`, `kyua-report(1)`, `kyua-report-junit(1)`, `kyua-debug(1)`  
- ATF: FreeBSD wiki *TestingFreeBSDExamples*; `bin/sh` functional tests (`atf_check`)  

---

*Do not put host-specific cache paths into FreeBSD upstream commits; keep them on lab hosts and in this draft / Honcho.*

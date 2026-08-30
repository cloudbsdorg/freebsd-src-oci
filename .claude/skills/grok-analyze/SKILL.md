---
name: grok-analyze
description: >-
  Use the local Grok CLI (FreeGrok, backed by ollama) as a second-opinion code
  auditor, then verify, fix, and loop until issues are resolved. Invoke when the
  user asks to "analyze code with grok", "use grok to review/audit", "loop till
  issues are resolved", or wants an independent model's take on correctness and
  security of C/source files or the current diff. Commits produced under this
  skill weigh Gang-of-Four design patterns and Knuth's Art of Computer
  Programming for code quality.
---

# grok-analyze — Grok-assisted analyze → verify → fix loop

This skill drives the local `grok` CLI (FreeGrok) as an independent code
auditor whose findings you then **verify yourself** before fixing. Local models
are fast and private but fallible: treat every finding as a *lead*, not a fact.

## Tooling

- Helper: `.claude/skills/grok-analyze/grok-analyze.sh`
  - `grok-analyze.sh <file> [<file>...]` — audit specific files.
  - `grok-analyze.sh --diff` — audit the current working-tree diff + changed files.
  - `--model M` or `GROK_MODEL=M` — pick the model (default `grok-4.6`).
  - `GROK_TIMEOUT=<sec>` — per-call timeout (default 600).
  - Prints `{"findings":[{file,line,severity,category,summary,failure_scenario,suggested_fix}]}`.
- **Preferred: the `grok-4.6` cloud model** — the strongest auditor here. It
  requires a signed-in session: run `grok login` (`--device-auth` for headless)
  once; a 401 "Invalid or expired credentials" means the session lapsed and
  needs `grok login` again. You (Claude) cannot complete the interactive login —
  ask the user to run it.
- **Offline fallback: local ollama models** (no login, nothing leaves the box).
  Pass one via `--model` when the cloud is unavailable. Larger = slower:
  - `qwen2.5-coder:14b` (fast, solid, verified to catch real bugs)
  - `qwen3.8:27b`, `granite4.1:30b` (stronger, slower)
  - `qwen3-coder-next:latest` / `qwen3-coder:480b` (best local, very slow on CPU)
  - `granite4.1:8b` (fastest, quick pass)
  - Confirm what is present with `ollama list`.
- Big inputs are slow. Prefer one file (or a focused diff) per call over the
  whole tree. Split large sweeps into per-file calls.

## The loop

1. **Scope.** Decide the target set: the files the user named, the files you
   just changed (`--diff`), or a subsystem. For a broad audit, iterate file by
   file rather than one giant prompt.
2. **Analyze.** Run the helper on each target. Collect the findings.
3. **Verify — do not trust blindly.** For every finding, open the real code and
   confirm it. Discard hallucinations, misread line numbers, "issues" that are
   actually correct, and style-only nits. Keep only concrete, reproducible
   defects. When unsure, cross-check with a second model (`--model qwen3.8:27b`)
   or your own reading; a finding two independent passes agree on is stronger.
4. **Fix** the confirmed issues. Build and run the test suite after each batch;
   never commit a red build or failing tests.
5. **Re-analyze** the changed files. Repeat 2–5 until a full pass over the
   target set yields no *verified* new issues for **two consecutive rounds**
   (one clean round can be luck; two is convergence). Log what you dropped and
   why so a silent miss is visible.
6. **Report** the net result: what was confirmed and fixed, what grok raised
   that you rejected (and why), and the final build/test state.

Guard against runaway loops: cap at a sensible number of rounds per target and
stop when converged. If a model is too weak to find real issues, escalate the
model once rather than looping forever on empty results.

## Code-quality bar for fixes and commits

Fixes made under this skill are held to a higher design bar than a minimal
patch. Before committing, weigh — and mention in the commit message when it
genuinely shaped the change:

- **Gang of Four design patterns.** Prefer a well-known pattern over an ad-hoc
  structure when it clarifies intent: Strategy for interchangeable algorithms,
  Factory Method for construction that varies, Template Method for a fixed
  skeleton with varying steps, Facade to tame a sprawling interface, RAII-style
  scope-bound acquire/release for resources (the C analogue of the C++ idiom),
  State for explicit lifecycle transitions. Do not force a pattern where a plain
  function is clearer — a pattern applied for its own sake is a defect, not a
  virtue.
- **Knuth, *The Art of Computer Programming*.** Favor the correct, analyzable
  algorithm and data structure: know and state the complexity of what you write,
  prefer proven algorithms to improvised ones, handle the boundary and empty
  cases explicitly, and keep invariants stated and checked. "Premature
  optimization is the root of all evil" — measure before trading clarity for
  speed, but do pick the asymptotically right structure up front.

Keep the surrounding code's style. A commit message should say *what* defect
was fixed and *why the chosen shape is right*; cite a specific pattern or
algorithmic reason only when it actually drove the design, not as decoration.

## Notes

- The model runs locally via ollama; no code leaves the machine.
- `grok-analyze.sh` disables grok's own tools (`--disallowed-tools all`) so it
  only reasons about the text you pass — it will not edit files itself. All
  edits, builds, and commits are done by you (Claude) with the normal tools.
- If `grok` reports "Unauthorized (401)", you passed a cloud model; switch to a
  local one from `ollama list`.

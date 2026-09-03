---
name: review-gate
description: The project's STANDARD two-level quality & security review gate. Run before merging or committing any substantive change to ocifbsd (code or integration work). Codifies code-level review (GoF + Knuth, Grok second-opinion audit, /code-review, /security-review, C memory-safety) and integration-level review (authN/authZ, secrets, mTLS, E2E on the real cluster, supply chain). Nothing is "done" until it passes this gate.
---

# review-gate — the standard quality & security review

This is the definition of done for any substantive ocifbsd change. Do not merge
or call work "complete" until it clears **both** levels below and findings are
**fixed, not deferred**. Scale depth to the change: a one-line fix needs the
code level; a new subsystem or anything touching the control plane, auth,
networking, or untrusted input needs the full gate including the dedicated
passes.

## 1. Code-level review (every change)

1. **Design & correctness to the project bar.** Gang-of-Four design patterns and
   Knuth-grade rigor; small, legible, `style(9)`-clean C (hard tabs, no trailing
   whitespace). Reads like the surrounding code.
2. **Independent second opinion — Grok.** Invoke the `grok-analyze` skill on the
   changed files / current diff and **loop until its findings are resolved**.
   (Standing project preference: Grok is the second-opinion auditor.)
3. **Built-in reviews on the diff.** Run `/code-review` (correctness,
   simplification, efficiency) and `/security-review` (security surface). Treat
   confirmed findings as blockers.
4. **C memory-safety, explicitly.** No realloc-into-the-same-pointer leaks, no
   use-after-free / double-free, no unchecked returns; all untrusted input is
   bounded and escaped before use (hand-built JSON, registry/API data,
   decompression sizes, replica counts). The 22 prior "review batch" fixes are
   the standing baseline — do not regress them.

## 2. Integration-level review (every phase / feature)

1. **Security boundaries end to end.** authN and authZ enforced at every entry
   point; secrets never in plaintext, logs, or process args; mTLS between
   components; network segmentation and least privilege; the audit log captures
   every mutation.
2. **E2E on the REAL cluster, not mocks.** Exercise the actual deployment:
   failover, reboot recovery, RBAC denials, token expiry, node-agent auth,
   admission rejection, and correct content under load. (Use `oci-failover-test`
   for node-down; `update-oci-websites` for the site E2E; drive browsers via CDP
   since Playwright has no FreeBSD build.)
3. **Supply chain.** Dependency and image scanning; signed images verified on
   pull; reproducible, `-Werror`-clean build (no `WARNS=0` escape hatch in CI).

## 3. Dedicated passes (before any "production" claim)

- **Threat model + adversarial audit** of anything in the control plane / API:
  authz bypass, token forgery, admission bypass, injection, path traversal,
  decompression and replica bombs.
- **Performance + resource-safety pass** — the stress-test discipline: find the
  breaking point, prove clean degradation (honest 5xx, never corrupted content),
  and record the numbers.

## Output

Summarize: what was reviewed, the code-level findings (with Grok's), the
integration/security findings, what was fixed, and the E2E result. Only then is
the change eligible to merge — and per project routine, commit (GoF + Knuth
commit quality), merge FreeBSD upstream, and push. Load `grok-analyze`,
`code-review`, and `security-review` as the tools this gate orchestrates.

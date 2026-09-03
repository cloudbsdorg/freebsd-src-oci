---
name: consult-agy-for-ui
description: Consult agy (the AI design agent on freedev007) whenever building or changing any user interface — web, desktop, or console/TUI. Use before finalizing UI work.
---

# Consult agy for UI work

For **any** user interface this project produces — a web page, a desktop app, a
console/TUI, a dashboard, a report layout — **agy must be at least consulted**
on the design. agy is an AI design agent on **freedev007** at
`/usr/local/bin/agy`. Do not ship a UI you built without giving agy a chance to
weigh in on it.

## When this applies

- New pages/sections on the OCI WordPress sites or oci.cloudbsd.org.
- Any redesign or restyle (theme changes, new panels, new visualizations).
- Desktop or console/TUI interfaces for ocifbsd tooling.
- Any place a human looks at output and layout/legibility matters.

## How to consult agy

Run it on freedev007 with a focused prompt and a real timeout (it prints only at
the end and can be slow):

```sh
ssh mlapointe@freedev007.cloudbsd.org \
  'agy --dangerously-skip-permissions --print-timeout 25m --add-dir <DIR> \
   -p "<what you are designing; ask for critique/improvements>"'
```

Give agy the context (what the UI is, who uses it, the current markup/screenshot
path if any) and ask specifically for design critique and concrete
improvements. Fold its suggestions into the result, then verify the rendered UI
(screenshot for web/desktop; run it for console). If agy is unavailable or times
out, note that you attempted the consult and proceed, but do not skip the
attempt.

## Also remember

Every web/UI update still needs the viewer-local-time timestamp and the
REVYTECH, Inc. / Mark LaPointe &lt;mark@revytechinc.com&gt; attribution at the
bottom (see the `web-footer-attribution-and-local-timestamp` rule / the
`update-oci-websites` skill).

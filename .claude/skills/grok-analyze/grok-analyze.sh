#!/bin/sh
# grok-analyze.sh — run the local Grok CLI (FreeGrok) as a headless code
# auditor and print structured findings as JSON.
#
# Uses `grok -p` in headless mode with a JSON schema so the model is
# constrained to emit machine-readable findings. Grok's default cloud model
# needs credentials; this script defaults to a locally-served ollama model so
# it works offline. Override with GROK_MODEL or --model.
#
# Usage:
#   grok-analyze.sh [--model M] [--diff | <file> [<file>...]]
#
#   --diff        Analyze the current working-tree diff (git diff HEAD) plus
#                 the full text of each changed file.
#   <file>...     Analyze the named source files.
#
# Output: JSON object {"findings":[...]} on stdout (from grok's
# structuredOutput). Non-finding chatter goes to stderr.
#
# Environment:
#   GROK_MODEL    model id (default: grok-4.6). The cloud models (grok-4.6,
#                 grok-4.5, …) require `grok login` first; if you see a 401,
#                 either log in or set GROK_MODEL to a local ollama model
#                 (e.g. qwen2.5-coder:14b) — see `ollama list`.
#   GROK_TIMEOUT  per-call timeout seconds (default: 600)

set -eu

MODEL="${GROK_MODEL:-grok-4.6}"
TIMEOUT="${GROK_TIMEOUT:-600}"
MODE="files"

while [ $# -gt 0 ]; do
	case "$1" in
	--model) MODEL="$2"; shift 2 ;;
	--model=*) MODEL="${1#--model=}"; shift ;;
	--diff) MODE="diff"; shift ;;
	--) shift; break ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

command -v grok >/dev/null 2>&1 || { echo "grok CLI not found in PATH" >&2; exit 127; }

# The structured-output schema: a list of concrete findings.
SCHEMA='{
  "type":"object",
  "properties":{
    "findings":{
      "type":"array",
      "items":{
        "type":"object",
        "properties":{
          "file":{"type":"string"},
          "line":{"type":"integer"},
          "severity":{"type":"string","enum":["critical","high","medium","low"]},
          "category":{"type":"string"},
          "summary":{"type":"string"},
          "failure_scenario":{"type":"string"},
          "suggested_fix":{"type":"string"}
        },
        "required":["file","severity","summary","failure_scenario"]
      }
    }
  },
  "required":["findings"]
}'

# Build the corpus to analyze.
CORPUS=$(mktemp)
trap 'rm -f "$CORPUS"' EXIT

if [ "$MODE" = "diff" ]; then
	{
		echo "=== git diff HEAD ==="
		git diff HEAD 2>/dev/null || true
		echo
		for f in $(git diff HEAD --name-only 2>/dev/null); do
			[ -f "$f" ] || continue
			echo "=== FILE: $f ==="
			cat "$f"
			echo
		done
	} > "$CORPUS"
else
	[ $# -gt 0 ] || { echo "no files given (and no --diff)" >&2; exit 2; }
	for f in "$@"; do
		[ -f "$f" ] || { echo "not a file: $f" >&2; continue; }
		echo "=== FILE: $f ===" >> "$CORPUS"
		cat "$f" >> "$CORPUS"
		echo >> "$CORPUS"
	done
fi

PROMPT="You are a meticulous C code auditor for a FreeBSD system program that
runs as root (an OCI container runtime built on jail(8), ZFS and libcurl).
Analyze the code below for REAL, concrete defects only — do not speculate or
report style nits. Prioritize:
  - memory safety (leaks, use-after-free, double-free, buffer/OOB, NULL deref)
  - unchecked return values that change behavior
  - integer/size overflow and truncation
  - command/shell/path injection and traversal (this runs as root)
  - TOCTOU / races
  - incorrect error handling that leaves partial state
For each finding give: file, line (best estimate), severity, category, a one
line summary, a concrete failure_scenario (inputs/state -> wrong outcome),
and a suggested_fix. If there are no real defects, return an empty findings
array. Report findings ONLY via the structured output schema.

CODE:
$(cat "$CORPUS")"

# Run grok headless. Disable its own tools (we only want analysis), constrain
# to the schema. Grok prints a JSON envelope; extract .structuredOutput.
OUT=$(timeout "$TIMEOUT" grok -m "$MODEL" --disallowed-tools all \
	--json-schema "$SCHEMA" -p "$PROMPT" 2>/dev/null) || {
	echo "grok invocation failed (model=$MODEL). Is the model available? Try: ollama list" >&2
	exit 1
}

# The response is a JSON envelope with a .structuredOutput field (the
# schema-constrained object); it may also carry the JSON as a string in
# .text. Extract {"findings":[...]} robustly with jq, falling back to
# python3, and finally to the raw envelope.
if command -v jq >/dev/null 2>&1; then
	printf '%s\n' "$OUT" | jq -c '
		if (.structuredOutput? // empty | type) == "object" then .structuredOutput
		elif has("findings") then {findings: .findings}
		elif (.text? | type) == "string" then (.text | fromjson)
		else . end' 2>/dev/null && exit 0
fi
if command -v python3 >/dev/null 2>&1; then
	printf '%s' "$OUT" | python3 -c '
import sys, json
d = json.load(sys.stdin)
out = None
if isinstance(d.get("structuredOutput"), dict):
    out = d["structuredOutput"]
elif "findings" in d:
    out = {"findings": d["findings"]}
elif isinstance(d.get("text"), str):
    try:
        out = json.loads(d["text"])
    except Exception:
        out = None
print(json.dumps(out if out is not None else d))
' 2>/dev/null && exit 0
fi
# Last resort: print raw for the caller to parse.
printf '%s\n' "$OUT"

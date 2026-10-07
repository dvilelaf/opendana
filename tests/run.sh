#!/usr/bin/env bash
# The standard suite, in tiers. Every channel of the requested gate runs to the
# end, whatever the channel before it did, and the suite ends with one line per
# channel: PASS, FAIL, BLOCKED or SKIPPED.
#
#   level 1 (--unit)  needs nothing of the original: no ROM, no imported
#                     package, no reference data. It is what a public clone runs.
#   levels 2 and 3    (--product --corpus --rom --packages --publication) need
#                     imported content, the user's ROM or reference data. Their
#                     gates live in tests/run-lab.sh, which is not part of the
#                     public tree.
#
#   bash tests/run.sh [--unit|--product|--corpus|--rom|--packages|--publication|--all]
#
# A channel or gate that belongs to the lab-only tier and whose files are not in
# this tree prints "skipped: lab-only" and is counted as SKIPPED. A channel whose
# data is missing is BLOCKED. The suite is never green with a blocked or failed
# channel; a skipped one is reported, not hidden.
set -uo pipefail
cd "$(dirname "$0")/.."
export BUILD_DIR="${BUILD_DIR:-build}"
gate="${1:---all}"
case "$gate" in
    --unit|--product|--corpus|--rom|--packages|--publication|--all) ;;
    *) echo 'usage: tests/run.sh --unit|--product|--corpus|--rom|--packages|--publication|--all' >&2
       exit 2 ;;
esac
want() { [ "$gate" = --all ] || [ "$gate" = "$1" ]; }

logs="${OD_RUN_LOGS:-$(mktemp -d "${TMPDIR:-/tmp}/opendana-run.XXXXXX")}"
mkdir -p "$logs"
names=() states=() notes=()

# Defaults for the public tier; tests/run-lab.sh, when present, redefines them.
lab_missing() { return 1; }
waiting_note() { return 1; }
[ -f tests/run-lab.sh ] && . tests/run-lab.sh

missing() { # missing REQ... -> prints what is missing, status 0 if something is
    lab_missing "$@"
}

skip() { # skip NAME WHY
    echo
    echo "######## $1"
    echo "skipped: lab-only ($2)"
    names+=("$1"); states+=(SKIPPED); notes+=("lab-only: $2")
}

lab_channel() { # lab_channel NAME FILE REQS CMD... : runs only when FILE is in this tree
    local name="$1" file="$2" reqs="$3"; shift 3
    if [ -e "$file" ]; then channel "$name" "$reqs" "$@"; else skip "$name" "$file is not in this tree"; fi
}

channel() { # channel NAME "REQS" CMD...
    local name="$1" reqs="$2"; shift 2
    local log="$logs/$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '_').log" lack rc note
    echo
    echo "######## $name"
    # shellcheck disable=SC2086
    if lack="$(missing $reqs)"; then
        echo "BLOCKED: $name needs $lack" | tee "$log"
        names+=("$name"); states+=(BLOCKED); notes+=("needs $lack")
        return
    fi
    "$@" 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    if [ "$rc" -eq 0 ]; then
        names+=("$name"); states+=(PASS); notes+=("")
    elif [ "$rc" -eq 2 ] && grep -q 'BLOCKED' "$log"; then
        names+=("$name"); states+=(BLOCKED); notes+=("$(grep -m1 'BLOCKED' "$log" | cut -c1-120)")
    else
        if note="$(waiting_note "$log")"; then note="all failures waiting on $note"; fi
        names+=("$name"); states+=(FAIL); notes+=("$note")
    fi
}

if want --unit; then
    channel "unit: model tests, provenance" "" bash tests/nivel1-unitario.sh
    channel "unit: editor/catalogue layout" "" bash tests/editor-ui-layout-run.sh
    channel "unit: interface screens" "" bash tests/ui-screens-layout-run.sh
    channel "unit: interface behaviour" "" bash tests/ui-behaviour-run.sh
    channel "unit: first-run screen layout" "" bash tests/first-run-layout-run.sh
    channel "unit: states across versions" "" bash tests/state-compat-run.sh
    channel "unit: cursor auto-hide timer" "" bash tests/cursor-hide-run.sh
    channel "unit: applications-menu launcher" "" bash tests/desktop-run.sh
    channel "unit: ROM chooser child" "" bash tests/import-picker-child.sh
    lab_channel "unit: reference manifest" tools/audit/content_manifest.sh "" bash tools/audit/content_manifest.sh --check
    channel "unit: packaging controls" "" bash tests/linux-package-unit.sh
    lab_channel "unit: ledger check, positive control" tools/dev/test_check_l8.sh "" bash tools/dev/test_check_l8.sh
    lab_channel "unit: ledger check" tools/dev/check_l8.sh "" bash tools/dev/check_l8.sh
fi
if [ "$gate" != --unit ]; then
    if declare -F lab_gates >/dev/null; then
        lab_prepare
        lab_gates
    else
        skip "levels 2 and 3 ($gate)" "tests/run-lab.sh is not in this tree"
    fi
fi

echo
echo "================ summary: tests/run.sh $gate ================"
pass=0 fail=0 blocked=0 skipped=0
for i in "${!names[@]}"; do
    printf '%-8s %s%s\n' "${states[$i]}" "${names[$i]}" "${notes[$i]:+  -- ${notes[$i]}}"
    case "${states[$i]}" in PASS) pass=$((pass + 1)) ;; FAIL) fail=$((fail + 1)) ;; SKIPPED) skipped=$((skipped + 1)) ;; *) blocked=$((blocked + 1)) ;; esac
done
echo "channels: $pass pass, $fail fail, $blocked blocked, $skipped skipped (logs: $logs)"
if [ "$fail" -eq 0 ] && [ "$blocked" -eq 0 ]; then
    echo "PASS requested gate $gate (not a certificate for unexecuted gates)"
    exit 0
fi
[ "$fail" -gt 0 ] && exit 1
echo "BLOCKED: $blocked channel(s) could not run; the gate is not green"
exit 2

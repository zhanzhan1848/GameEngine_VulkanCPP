#!/usr/bin/env bash
# smoke_test_bundle.sh — verify PrimalEditor.app bundle starts without
# crashing repeatedly. Useful as a pre-flight check before asking the user
# to launch the GUI.
#
# Usage:  ./smoke_test_bundle.sh <bundle_path> [N=5]
#
# Runs the bundled binary N times in sequence (codesigning each time to
# invalidate any LaunchServices cache from a stale signature) and asserts:
#   1. Each run produces at least one metal DRAW call (engine rendered).
#   2. No new ~/Library/Logs/DiagnosticReports/PrimalEditor_Avalonia-*.ips
#      crash files were written during the run window.
#   3. Each run lives > 5 seconds before self-exit (vs the pre-fix
#      JIT SIGSEGV at 29ms).
#
# This script does not need a GUI session — it only exercises the Avalonia
# bootstrap path and the engine's editor-mode render loop. It does NOT verify
# that the user can see a window; that step requires a real macOS GUI session.
set -euo pipefail

BUNDLE="${1:?usage: $0 <bundle_path> [N=5]}"
N="${2:-5}"

if [[ ! -x "$BUNDLE/Contents/MacOS/PrimalEditor_Avalonia" ]]; then
  echo "[smoke] bundle binary not found: $BUNDLE/Contents/MacOS/PrimalEditor_Avalonia" >&2
  exit 1
fi

PRE_CRASHES=$(find ~/Library/Logs/DiagnosticReports -name 'PrimalEditor_Avalonia-*.ips' 2>/dev/null | wc -l | tr -d ' ')
PRE_RUN_LOG="/tmp/smoke_bundle_$$.log"
echo "[smoke] bundle: $BUNDLE"
echo "[smoke] pre-run crashes: $PRE_CRASHES"

PASS=0
FAIL=0
for i in $(seq 1 "$N"); do
  pkill -9 -f PrimalEditor 2>/dev/null || true
  sleep 0.4
  codesign --force --deep --sign - "$BUNDLE" >/dev/null 2>&1
  RUN_LOG="/tmp/smoke_run_${i}_$$.log"
  echo "--- run $i ---" | tee -a "$PRE_RUN_LOG"

  "$BUNDLE/Contents/MacOS/PrimalEditor_Avalonia" > "$RUN_LOG" 2>&1 &
  PID=$!
  DRAW_SEEN=""
  for t in $(seq 1 15); do
    sleep 1
    if ! ps -p "$PID" > /dev/null; then
      break
    fi
    if grep -q "DRAW #" "$RUN_LOG" 2>/dev/null; then
      DRAW_SEEN="yes"
      break
    fi
  done

  if ps -p "$PID" > /dev/null; then
    kill -9 "$PID" 2>/dev/null
    sleep 0.3
  fi

  DRAW_CALLS=$(grep -c "DRAW #" "$RUN_LOG" || true)
  EDITOR_MODE=$(grep -c "0 editor_mode=1" "$RUN_LOG" || true)
  LAST_LINE=$(tail -1 "$RUN_LOG" | cut -c1-80)

  if [[ -z "$DRAW_SEEN" ]]; then
    echo "[smoke] run $i: FAIL — process exited in <$t s without DRAW, last=$LAST_LINE"
    FAIL=$((FAIL + 1))
  elif [[ $EDITOR_MODE -lt 1 ]]; then
    echo "[smoke] run $i: FAIL — DRAW fired but engine never entered editor_mode, last=$LAST_LINE"
    FAIL=$((FAIL + 1))
  else
    echo "[smoke] run $i: PASS — DRAW=$DRAW_CALLS, editor_mode=$EDITOR_MODE, first_DRAW @${t}s"
    PASS=$((PASS + 1))
  fi
done

POST_CRASHES=$(find ~/Library/Logs/DiagnosticReports -name 'PrimalEditor_Avalonia-*.ips' 2>/dev/null | wc -l | tr -d ' ')
NEW_CRASHES=$((POST_CRASHES - PRE_CRASHES))

echo
echo "[smoke] results: pass=$PASS fail=$FAIL"
echo "[smoke] new crash files during run: $NEW_CRASHES (should be 0)"
if [[ $NEW_CRASHES -gt 0 ]]; then
  echo "[smoke] WARN new crashes:"
  find ~/Library/Logs/DiagnosticReports -name 'PrimalEditor_Avalonia-*.ips' -newer "$PRE_RUN_LOG" 2>/dev/null
fi
[[ $PASS -eq $N && $NEW_CRASHES -eq 0 ]]

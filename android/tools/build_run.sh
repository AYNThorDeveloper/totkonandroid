#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/.." && pwd)
report="$android/../../Test/$(date +%Y%m%d_%H%M%S)_build_run"
mkdir -p "$report"
if ! bash "$android/game_activity/build_apk.sh" > "$report/build.log" 2>&1; then
  tail -40 "$report/build.log" >&2
  exit 1
fi
TOTK_RUN_REPORT="$report" exec bash "$here/thor_run.sh"

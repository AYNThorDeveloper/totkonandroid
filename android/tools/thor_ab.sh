#!/usr/bin/env bash
# Unattended A/B on the Thor: for each env file, start the game with it, press A to continue the save (Link then stands
# where the save put him: the same scene every run, as long as nobody touches the controller), measure, and move on.
# At the end the original env is put back and a table is printed (thor_ab_report.py).
#   usage: thor_ab.sh <out dir> <env file>...   (MEASURE=60 seconds of gameplay, LOAD_TIMEOUT=150)
# A run whose process dies is recorded as such and the next one starts. Each run dir holds run.log, shots/, and the
# TOTK_TIMELINE file when that env asks for one (pulled from /data/local/tmp/totk-android-v143/).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$1; shift
pkg=org.macnativegames.totk.gameprobe
dir=/data/local/tmp/totk-android-v143
measure=${MEASURE:-60}; load_timeout=${LOAD_TIMEOUT:-150}
adb get-state >/dev/null 2>&1 || { echo 'Thor not connected' >&2; exit 1; }
mkdir -p "$out"
adb shell cat "$dir/env" > "$out/env.original"
restore() { adb push "$out/env.original" "$dir/env" >/dev/null; adb shell ": > $dir/shader_debug"; }
trap restore EXIT
for env in "$@"; do
  name=$(basename "$env" .txt); run="$out/$name"; mkdir -p "$run/shots"
  echo "== $name"
  adb push "$env" "$dir/env" >/dev/null
  adb shell ": > $dir/shader_debug"
  adb shell input keyevent KEYCODE_WAKEUP
  adb shell am force-stop "$pkg"; sleep 2
  adb logcat -c
  adb logcat -v monotonic -s 'TOTKGameLoader:*' 'AdrenoVK-0:*' 'MESA:*' 'DEBUG:*' 'libc:*' > "$run/run.log" 2>&1 &
  logger=$!
  adb shell am start -n "$pkg/.VideoSettingsActivity" >/dev/null
  # Title screen, then Continue: A twice with pauses (a press during loading does nothing).
  started=$(date +%s); pressed=0; ingame=0
  while (( $(date +%s) - started < load_timeout )); do
    sleep 5
    adb shell pidof "$pkg" >/dev/null || break
    if grep -qE 'draws per frame' "$run/run.log" && awk '/passes, [0-9]+ draws per frame/ { n = $0; sub(/.*passes, /, "", n); sub(/ draws.*/, "", n); if (n + 0 > 1500) found = 1 } END { exit !found }' "$run/run.log"; then ingame=1; break; fi
    if (( $(date +%s) - started >= 20 && pressed < 4 )); then adb shell input gamepad keyevent KEYCODE_BUTTON_A 2>/dev/null || adb shell input keyevent KEYCODE_BUTTON_A; pressed=$((pressed + 1)); fi
  done
  if (( ingame )); then
    echo "in game after $(( $(date +%s) - started )) s; measuring $measure s"
    sleep 10   # streaming settles
    mark=$(wc -l < "$run/run.log"); echo "$mark" > "$run/measure_from_line"
    for ((i = 1; i <= measure; i++)); do
      sleep 1
      (( i % 20 == 0 )) && adb exec-out screencap -p > "$run/shots/t$i.png"
      adb shell pidof "$pkg" >/dev/null || { echo "process ended during the measurement"; break; }
    done
    { adb shell -n 'for z in /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq /sys/class/kgsl/kgsl-3d0/gpuclk; do [ -r $z ] && echo "$z $(cat $z)"; done; for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); case "$t" in gpu*|cpu*) echo "$t $(cat $z/temp 2>/dev/null)";; esac; done'; } > "$run/clocks_thermal.txt" 2>&1
  else
    echo "not in game (process alive: $(adb shell pidof $pkg >/dev/null && echo yes || echo no))"
  fi
  tl=$(grep -E '^TOTK_TIMELINE=' "$env" | head -1 | cut -d= -f2 | cut -d: -f1)
  if [ -n "$tl" ]; then sleep 15; adb pull "$tl" "$run/timeline.tsv" >/dev/null 2>&1 && python3 "$here/timeline_report.py" "$run/timeline.tsv" > "$run/timeline_report.txt" 2>&1; fi
  kill "$logger" 2>/dev/null; wait "$logger" 2>/dev/null
done
adb shell am force-stop "$pkg"
python3 "$here/thor_ab_report.py" "$out" | tee "$out/report.txt"

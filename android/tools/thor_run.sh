#!/usr/bin/env bash
# Run only when the Thor is connected. Existing saves, shaders and logs survive.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/.." && pwd)
package=org.macnativegames.totk.gameprobe
if ! adb get-state >/dev/null 2>&1; then
  echo 'Thor niet aangesloten; geen installatie of testrun uitgevoerd.' >&2
  exit 1
fi
report=${TOTK_RUN_REPORT:-"$android/../../Test/$(date +%Y%m%d_%H%M%S)_thor"}
mkdir -p "$report/shots"
adb reverse tcp:27760 tcp:27760
adb install -r "$android/game_activity/build/TOTK-Game-Loader.apk"
adb shell input keyevent KEYCODE_WAKEUP
adb shell am force-stop "$package"
adb logcat -T 1 -s 'TOTKGameLoader:*' 'AdrenoVK-0:*' 'MESA:*' > "$report/run.log" 2>&1 &
logger=$!
trap 'kill "$logger" 2>/dev/null || true' EXIT
adb shell am start -n "$package/.VideoSettingsActivity"
for ((i=1;i<=${DURATION:-60};i++)); do
  sleep 1
  if ((i % 15 == 0)); then adb exec-out screencap -p > "$report/shots/t${i}.png"; fi
  # Temperatures and clocks every 15 s (thermal.txt): hottest CPU and GPU zones in degrees C, GPU and prime-core MHz.
  # Read as the adb shell user, no root; a zone or clock the device does not expose prints "-".
  if ((i % 15 == 0)); then adb shell -n 'c=-; g=-; for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); v=$(cat $z/temp 2>/dev/null) || continue;
      case "$t" in cpu*|cpuss*) [ "$c" = - ] || [ "$v" -gt "$c" ] && c=$v;; gpu*) [ "$g" = - ] || [ "$v" -gt "$g" ] && g=$v;; esac; done;
      f=$(cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null || cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null || echo -);
      p=$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq 2>/dev/null || echo -);
      echo "cpu_max_mC=$c gpu_max_mC=$g gpu_hz=$f cpu7_khz=$p"' | sed "s/^/t${i}s /" >> "$report/thermal.txt" 2>&1; fi
  # RAM of the game process (Android's own accounting, incl. graphics) once a minute
  if ((i % 60 == 0)); then { echo "== t${i}s"; adb shell dumpsys meminfo "$package" | sed -n '1,45p'; } >> "$report/meminfo.txt" 2>&1; fi
  if grep -qE 'FAIL:|loader returned|refusing a fault loop|fatal signal' "$report/run.log"; then break; fi
  if ! adb shell pidof "$package" >/dev/null; then break; fi
done
tail -40 "$report/run.log"
echo "Resultaten: $report"

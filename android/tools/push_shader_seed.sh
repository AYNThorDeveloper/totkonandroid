#!/usr/bin/env bash
# Copies the shader seed (android_shader_seed.py) into the app's shader cache on the Thor, so the game needs no
# Mac bridge. Files of the same name are the same translation (the name hashes shader and answers), so existing
# cache entries are simply refreshed; the device's own recorded variants stay.
# usage: push_shader_seed.sh [seed dir, default ../Uitgepakt/shader-seed-v143]
set -euo pipefail
cd "$(dirname "$0")/../.."
seed=${1:-../Uitgepakt/shader-seed-v143}
package=org.macnativegames.totk.gameprobe
[ -f "$seed/cache/seed.idx" ] || { echo "no seed at $seed (run android/tools/android_shader_seed.py --out $seed)" >&2; exit 1; }
adb get-state >/dev/null 2>&1 || { echo "Thor not connected (adb)" >&2; exit 1; }
tarball=$(mktemp "${TMPDIR:-/tmp}/totk-seed.XXXXXX")
trap 'rm -f "$tarball"' EXIT
COPYFILE_DISABLE=1 tar -cf "$tarball" -C "$seed/cache" .
# Variants the Thor met outside the seed, translated afterwards by seed_missing.py (loaded by their exact name).
if ls "$seed"/extra/*.vks >/dev/null 2>&1; then COPYFILE_DISABLE=1 tar -rf "$tarball" -C "$seed/extra" .; fi
echo "seed: $(wc -l < "$seed/cache/seed.idx") variants, $(du -h "$tarball" | cut -f1)"
adb push "$tarball" /data/local/tmp/totk-shader-seed.tar
adb shell am force-stop "$package"
adb shell run-as "$package" sh -c "'mkdir -p files/shaders && cd files/shaders && tar -xf /data/local/tmp/totk-shader-seed.tar && ls | wc -l'"
adb shell rm /data/local/tmp/totk-shader-seed.tar
echo "pushed; play without the Mac: TOTK_SHADER_BRIDGE=0 in the env file (or no bridge / adb reverse running)"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
headers="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include"
work=$(mktemp -d /private/tmp/totk-cachefix-tests.XXXXXX)
trap 'rm -rf "$work"' EXIT
mkdir "$work/include"
ln -s "$headers/vulkan" "$work/include/vulkan"
ln -s "$headers/vk_video" "$work/include/vk_video"
gfx=games/totk/third_party/d2r_gfx/gfx
base=(-std=gnu11 -O1 -g -I"$work/include" -Iandroid/build -I"$gfx/nvn/include" -I"$gfx/nvn" -I"$gfx/tex"
  -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -ffunction-sections -fdata-sections -Wl,-dead_strip)
clang "${base[@]}" -fsanitize=address,undefined android/tests/feature_chain_test.c -o "$work/features"
"$work/features"
for sanitizer in address,undefined thread; do
  clang "${base[@]}" -fsanitize="$sanitizer" android/tests/pipeline_prewarm_test.c -o "$work/warm"
  mkdir "$work/$sanitizer"
  "$work/warm" "$work/$sanitizer"
done
clang "${base[@]}" -fsanitize=address,undefined android/tests/depth_cache_test.c -o "$work/depth"
mkdir "$work/depth-data"
cache=${TOTK_SEED_CACHE:-../Uitgepakt/shader-seed-v143/cache}
found=0
for writer in "$cache"/233dfb93*.vks; do
  [ -f "$writer" ] || continue
  "$work/depth" "$writer" "$work/depth-data"
  found=1; break
done
[ "$found" = 1 ] || { echo 'Missing real depth-writer fixture' >&2; exit 1; }
bash android/test_graphics_settings.sh
echo 'Cachefix targeted checks passed (ASan/UBSan and prewarm TSan).'

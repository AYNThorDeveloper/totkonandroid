#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ndk_root=${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to the Android NDK directory}
compiler="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang"
if [[ ! -x "$compiler" ]]; then
  compiler="$ndk_root/toolchains/llvm/prebuilt/darwin-arm64/bin/aarch64-linux-android29-clang"
fi
# Every core of the Thor (Snapdragon 8 Gen 2: Cortex-A510/A715/X3) has ARMv8.2: atomics are inlined LSE
# instructions instead of __aarch64_* helper calls (9% of the replay profile, 21 Oct). Not -mcpu=cortex-x3: that
# allows SVE, which this SoC does not expose to apps. TOTK_CPU_FLAGS="" builds for generic ARMv8 as before.
read -r -a cpu_flags <<< "${TOTK_CPU_FLAGS--march=armv8.2-a -mtune=cortex-x3}"
if [[ ! -x "$compiler" ]]; then
  echo "Android NDK clang not found. Set ANDROID_NDK_HOME to your NDK directory." >&2
  exit 1
fi

python3 "$repo_root/android/prepare_android_runtime.py"
mkdir -p "$repo_root/android/build"
# Vulkan through function pointers (system driver or a custom one via libadrenotools).
python3 "$repo_root/android/gen_vk_dispatch.py" "$repo_root/android/build/nvk_vk_dispatch" \
  "$repo_root/android/android_gfx_bridge.c" "$repo_root"/android/nvn_vk_*.c
adreno_build="$repo_root/android/build/adrenotools"
if [[ ! -f "$adreno_build/libadrenotools.a" ]]; then
  cmake -S "$repo_root/android/third_party/libadrenotools" -B "$adreno_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk_root/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$adreno_build" >/dev/null
fi
vulkan_flags=(-DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR)
common=(
  -std=gnu11 -O2 ${cpu_flags[@]+"${cpu_flags[@]}"} -Wall -Wextra -Werror -Wno-format "${vulkan_flags[@]}"
  -I"$repo_root/android/third_party/libadrenotools/include"
  -D__ANDROID__ -DTOTK_GAME_V143
  -I"$repo_root/android/compat"
  -I"$repo_root/games/totk/runtime"
  -I"$repo_root/games/totk/third_party/d2r_gfx/gfx/nvn/include"
  -I"$repo_root/android/build"
  -I"$repo_root/games/totk/build/v143"
)
sources=(
  "$repo_root/android/module_loader_probe.c"
  "$repo_root/android/host_gate_probe.S"
  "$repo_root/android/android_hle_stack.S"
  "$repo_root/android/android_hle_stack.c"
  "$repo_root/android/android_thread_enter.S"
  "$repo_root/android/android_runtime_threads.c"
  "$repo_root/android/android_runtime_stubs.c"
  "$repo_root/android/nvn_vk_translate.c"
  "$repo_root/android/android_gfx_bridge.c"
  "$repo_root/android/nvn_vk_host.c"
  "$repo_root/android/android_vk_loader.c"
  "$repo_root/android/android_frame_step.c"
  "$repo_root/android/android_video_menu.c"
  "$repo_root/android/android_video_menu_hooks.S"
  "$repo_root/android/android_vpx_mediacodec.c"
  "$repo_root/android/build/nvk_vk_dispatch.c"
  "$repo_root/android/android_input.c"
  "$repo_root/android/thor_gamepad.c"
  "$repo_root/games/totk/runtime/hid_feedback.c"
  "$repo_root/android/android_front_probe.c"
  "$repo_root/android/android_romfs.c"
  "$repo_root/android/graphics_settings.c"
  "$repo_root/android/android_compat.c"
  "$repo_root/games/totk/runtime/services.c"
  "$repo_root/games/totk/runtime/threads.c"
  "$repo_root/games/totk/runtime/ipc.c"
  "$repo_root/android/android_thread_slots.c"
  "$repo_root/android/android_dynamic_tls.c"
)
# Shared NVN layer of the macOS port (d2r_gfx), compiled against Android
# compat headers, plus the Vulkan execution layer that replaces nvn_mtl.m.
gfx="$repo_root/games/totk/third_party/d2r_gfx/gfx"
gfx_out="$repo_root/android/build/nvn"
mkdir -p "$gfx_out"
pic=()
[[ ${ANDROID_LOADER_LIBRARY:-0} == 1 ]] && pic=(-fPIC)
# TOTK_OPT=fast (default): -O3 and ThinLTO over the shared NVN layer and the Vulkan layer, so the replay's calls across
# files (image table, texture registry, pool lookups, state) can be inlined. TOTK_OPT=O2: -O2 without LTO, as before.
# The loader and runtime sources above keep -O2.
case "${TOTK_OPT:-fast}" in
  fast) gfx_opt=(-O3 -flto=thin); link_opt=(-flto=thin -O3 -Wl,--lto-O3 -Wl,--thinlto-cache-dir="$repo_root/android/build/thinlto-cache") ;;
  O2) gfx_opt=(-O2); link_opt=() ;;
  *) echo "TOTK_OPT must be fast or O2" >&2; exit 1 ;;
esac
echo "NVN/Vulkan layer: TOTK_OPT=${TOTK_OPT:-fast} (${gfx_opt[*]})" >&2
gfx_flags=(-std=gnu11 "${gfx_opt[@]}" ${cpu_flags[@]+"${cpu_flags[@]}"} -D__ANDROID__ -DTOTK_GAME_V143 "${pic[@]}" "${vulkan_flags[@]}"
  -I"$repo_root/android/compat" -I"$repo_root/android" -I"$repo_root/games/totk/runtime"
  -I"$gfx/nvn/include" -I"$gfx/nvn" -I"$gfx/tex" -I"$repo_root/android/build" -I"$repo_root/games/totk/build/v143"
  -Wno-format -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable)
gfx_objects=()
for f in "$gfx"/nvn/hle_nvn.c "$gfx"/nvn/nvn_dirty.c "$gfx"/nvn/nvn_tex.c "$gfx"/nvn/totk_lifelog.c \
         "$gfx"/nvn/totk_nvn_ext.c "$gfx"/nvn/totk_timeline.c "$gfx"/nvn/totk_vpool.c \
         "$gfx"/tex/nvn_bcdec.c "$gfx"/tex/nvn_deswizzle.c "$gfx"/tex/nvn_bc1astc.c \
         "$repo_root/games/totk/runtime/account.c" "$repo_root/games/totk/runtime/applet_host.c" \
         "$repo_root/games/totk/runtime/time_host.c" "$repo_root/games/totk/runtime/audio_renderer.c" \
         "$repo_root/games/totk/runtime/audio_mixer.c" "$repo_root/games/totk/runtime/audio_reverb.c" \
         "$repo_root/games/totk/runtime/audio_effects.c" "$repo_root/games/totk/runtime/fs_host.c" \
         "$repo_root/games/totk/runtime/movie_host.c"; do
  o="$gfx_out/$(basename "$f").o"
  "$compiler" -c "${gfx_flags[@]}" -include "$repo_root/android/compat/android_darwin.h" "$f" -o "$o"
  gfx_objects+=("$o")
done
for f in nvn_vk_backend nvn_vk_image nvn_vk_present nvn_vk_format nvn_vk_texture_stage nvn_vk_sampler nvn_vk_shader nvn_vk_draw nvn_vk_pipeline_cache nvn_vk_record; do
  "$compiler" -c "${gfx_flags[@]}" -Wall -Wextra -Werror "$repo_root/android/$f.c" -o "$gfx_out/$f.o"
  gfx_objects+=("$gfx_out/$f.o")
done
# Opus decoder (hwopus): the macOS port's opus_codec.c over libopus, when the
# user has placed the libopus source in android/third_party/.
opus_lib=$("$repo_root/android/build_opus.sh" "$compiler")
if [[ -n "$opus_lib" ]]; then
  opus_src=$(ls -d "$repo_root"/android/third_party/opus-* | sort | tail -1)
  "$compiler" -c "${gfx_flags[@]}" -include "$repo_root/android/compat/android_darwin.h" -I"$opus_src/include" \
    "$repo_root/games/totk/runtime/opus_codec.c" -o "$gfx_out/opus_codec.o"
  gfx_objects+=("$gfx_out/opus_codec.o" "$opus_lib")
else
  echo "note: libopus source absent (android/third_party/opus-*): hwopus stays unavailable" >&2
fi
for f in nvn_thunks nvn_entry; do
  python3 "$repo_root/android/port_macho_asm.py" "$gfx/nvn/$f.S" "$gfx_out/$f.elf.S"
  "$compiler" -c "${pic[@]}" "$gfx_out/$f.elf.S" -o "$gfx_out/$f.o"
  gfx_objects+=("$gfx_out/$f.o")
done

if [[ ${ANDROID_LOADER_LIBRARY:-0} == 1 ]]; then
  glue="$ndk_root/sources/android/native_app_glue"
  output="$repo_root/android/game_activity/build/lib/arm64-v8a/libtotkgame.so"
  mkdir -p "$(dirname "$output")"
  "$compiler" "${common[@]}" ${link_opt[@]+"${link_opt[@]}"} -Wno-unused-parameter -DANDROID_LOADER_LIBRARY -fPIC -shared -I"$glue" \
    "${sources[@]}" "$repo_root/android/game_activity/game_activity.c" \
    "$glue/android_native_app_glue.c" "${gfx_objects[@]}" "$adreno_build/libadrenotools.a" "$adreno_build/lib/linkernsbypass/liblinkernsbypass.a" -lc++_static -lc++abi -ldl -landroid -llog -laaudio -lmediandk -lm -o "$output"
else
  output="$repo_root/android/build/module_loader_probe"
  "$compiler" "${common[@]}" ${link_opt[@]+"${link_opt[@]}"} "${sources[@]}" "${gfx_objects[@]}" -ldl -landroid -lvulkan -laaudio -lmediandk -lm -o "$output"
fi
file "$output"

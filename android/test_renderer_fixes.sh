#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
headers="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include"
work=$(mktemp -d /private/tmp/totk-renderer-tests.XXXXXX)
trap 'rm -rf "$work"' EXIT
mkdir "$work/include"
python3 android/gen_vk_dispatch.py "$work/nvk_vk_dispatch" android/android_gfx_bridge.c android/nvn_vk_*.c
ln -s "$headers/vulkan" "$work/include/vulkan"
ln -s "$headers/vk_video" "$work/include/vk_video"
gfx=games/totk/third_party/d2r_gfx/gfx
common=(-std=gnu11 -O1 -g -fsanitize=address,undefined -I"$work/include" -I"$work" -I"$gfx/nvn/include" -I"$gfx/nvn" -I"$gfx/tex")
# Production sources use generated Vulkan pointers even in host tests.
vk=(-DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR)
clang "${common[@]}" android/tests/feature_chain_test.c -o "$work/features"
"$work/features"
clang "${common[@]}" android/tests/frame_step_test.c -o "$work/frame"
"$work/frame"
TOTK_FRAME_CAP=30 "$work/frame"
TOTK_FRAME_CAP=60 "$work/frame"
if TOTK_FRAME_CAP=30 TOTK_FPS60=0 "$work/frame"; then echo 'invalid 30-FPS setup was accepted' >&2; exit 1; fi
clang "${common[@]}" android/tests/dirty_failure_test.c -o "$work/dirty"
"$work/dirty"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/ubo_flush_coalesce_test.c -o "$work/ubo_flush"
"$work/ubo_flush"
TOTK_VK_UBO_COALESCE=0 "$work/ubo_flush"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/query_counter_test.c -o "$work/query"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/ubo_capture_test.c -o "$work/ubo_capture"
"$work/ubo_capture"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/gw_index_test.c -o "$work/gw_index"
"$work/gw_index"
TOTK_VK_GW_INDEX=0 "$work/gw_index"
clang -std=gnu11 -O1 -g -fsanitize=address,undefined android/tests/fmutex_test.c -o "$work/fmutex"
"$work/fmutex"
clang -std=gnu11 -O1 -g -fsanitize=thread android/tests/fmutex_test.c -o "$work/fmutex_tsan"
"$work/fmutex_tsan"
"$work/query"
TOTK_VK_QUERY_BATCH=1 "$work/query"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/conditional_redirect_test.c -o "$work/conditional"
TOTK_VK_UBO_POOL=1 "$work/conditional"
TOTK_VK_RECORD_SEGMENTS=1 "$work/conditional"
TOTK_VK_RECORD_SEGMENTS=1 TOTK_VK_SEGMENT_CHECKPOINTS=1 "$work/conditional"
"$work/conditional"
TOTK_VK_UBO_INDEX=0 "$work/conditional" | grep 'uniform log'
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/variant_share_test.c -o "$work/variant_share"
"$work/variant_share"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/seed_match_test.c -o "$work/seed_match"
mkdir "$work/seed-data"
"$work/seed_match" "$work/seed-data"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/zero_inputs_test.c -o "$work/zero_inputs"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/depth_offset_test.c -o "$work/depth_offset"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/depth_cache_test.c -o "$work/depth_cache"
# Word-for-word against the bridge needs real replies (game-derived, outside the repo): the seed's cache.
seed_cache=${TOTK_SEED_CACHE:-../Uitgepakt/shader-seed-v143/cache}
for writer in "$seed_cache"/233dfb93*.vks "$seed_cache"/b117e55c*-6c026feb388a.vks "$seed_cache"/5218c5e5*-6c026feb388a.vks; do
  [ -f "$writer" ] || continue
  "$work/depth_offset" "$writer" "$work/depth_offset.spv" && spirv-val "$work/depth_offset.spv"
  mkdir "$work/depth-cache-$(basename "$writer")"
  "$work/depth_cache" "$writer" "$work/depth-cache-$(basename "$writer")"
done
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/exact_position_test.c -o "$work/exact_position"
if [ -f "$seed_cache/seed.idx" ]; then   # every 40th seeded vertex shader: rewritten, then spirv-val on each
  mkdir "$work/exact"
  awk '$3==0{print $2}' "$seed_cache/seed.idx" | awk 'NR%40==1' | sed "s#^#$seed_cache/#; s#\$#.vks#" > "$work/exact.list"
  "$work/exact_position" "$work/exact" $(cat "$work/exact.list")
  for spv in "$work"/exact/*.spv; do spirv-val --target-env vulkan1.3 "$spv"; done
else echo "exact vertex positions: skipped (no seed cache at $seed_cache)"; fi
if [ -f "$seed_cache/seed.idx" ]; then python3 android/tests/zero_inputs_test.py "$work/zero_inputs" "$seed_cache" 150
else echo "input stripping: skipped (no seed cache at $seed_cache; android/tools/android_shader_seed.py makes one)"; fi
if [ -f "$seed_cache/seed.idx" ]; then python3 android/tests/gather_nudge_test.py "$seed_cache"
else echo "gather nudge: skipped (no seed cache)"; fi
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/fetch_variant_test.c -o "$work/fetch_variant"
if [ -f "$seed_cache/seed.idx" ]; then python3 android/tests/fetch_variant_test.py "$work/fetch_variant" "$seed_cache" 150 1
    python3 android/tests/fetch_variant_test.py "$work/fetch_variant" "$seed_cache" 2000 8          # depth (TOTK_VK_FETCH_DEPTH)
    python3 android/tests/fetch_variant_test.py "$work/fetch_variant" "$seed_cache" 150 8 0x0000   # depth swizzled to RRRR
else echo "framebuffer fetch variants: skipped (no seed cache)"; fi
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/desc_reuse_test.c -o "$work/desc_reuse"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/descriptor_recycle_test.c -o "$work/desc_recycle"
"$work/desc_recycle"
TOTK_VK_DESC_RECYCLE=0 "$work/desc_recycle"
TOTK_VK_DESC_WAYS=4 "$work/desc_reuse"
TOTK_VK_DESC_WAYS=4 TOTK_VK_DESC_TEMPLATE=0 "$work/desc_reuse"
"$work/desc_reuse"
TOTK_VK_DESC_RECENT=0 "$work/desc_reuse"
TOTK_VK_DESC_CACHE=0 "$work/desc_reuse"
TOTK_VK_DESC_TEMPLATE=0 "$work/desc_reuse"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/depth_fetch_test.c android/nvn_vk_format.c "$gfx/nvn/nvn_tex.c" -o "$work/depth_fetch"
"$work/depth_fetch"
TOTK_VK_UBO_EARLY_CHECK=0 "$work/depth_fetch"
TOTK_VK_KEY_CACHE=0 TOTK_VK_TEX_LOOKUP_ONCE=0 TOTK_VK_DRIVER_BANK_FAST=0 TOTK_VK_SIG_CACHE=0 TOTK_VK_SBUF_COPY_REUSE=0 "$work/depth_fetch"
# Recording thread: under ThreadSanitizer (its own build), on and off.
clang -std=gnu11 -O1 -g -fsanitize=thread -I"$work/include" -I"$work" -I"$gfx/nvn/include" -I"$gfx/nvn" -I"$gfx/tex" "${vk[@]}" android/tests/record_thread_test.c -o "$work/record_thread"
TOTK_VK_RECORD_THREAD=1 "$work/record_thread"
TOTK_VK_RECORD_THREAD=1 TOTK_VK_BARRIER_DEDUP=0 "$work/record_thread"
TOTK_VK_RECORD_THREAD=1 TOTK_VK_RECORD_BATCH=0 "$work/record_thread"
TOTK_VK_RECORD_THREAD=1 TOTK_VK_RECORD_SEGMENTS=1 "$work/record_thread"
TOTK_VK_RECORD_SEGMENTS=1 "$work/record_thread"
"$work/record_thread"
clang "${common[@]}" android/tests/deswizzle_fast_test.c "$gfx/nvn/nvn_tex.c" "$gfx/tex/nvn_deswizzle.c" -o "$work/deswizzle"
"$work/deswizzle"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/upload_threads_test.c android/nvn_vk_format.c android/nvn_vk_texture_stage.c \
    "$gfx/nvn/nvn_tex.c" "$gfx/tex/nvn_deswizzle.c" "$gfx/tex/nvn_bcdec.c" -o "$work/upload_threads"
"$work/upload_threads"
TOTK_VK_UPLOAD_THREADS=0 "$work/upload_threads"
TOTK_VK_UPLOAD_STRIPES=0 "$work/upload_threads"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/image_dirty_epoch_test.c -o "$work/dirty_epoch"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/image_share_test.c android/nvn_vk_format.c android/nvn_vk_texture_stage.c \
    "$gfx/nvn/nvn_tex.c" "$gfx/tex/nvn_deswizzle.c" "$gfx/tex/nvn_bcdec.c" -o "$work/image_share"
"$work/image_share"
TOTK_VK_FORMAT_LIST=0 TOTK_VK_TEX_SUBALLOC=0 "$work/image_share"
"$work/dirty_epoch"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/sampler_id_cache_test.c android/nvn_vk_sampler.c -o "$work/sampler_id"
"$work/sampler_id"
TOTK_VK_SAMPLER_ID_CACHE=0 "$work/sampler_id"
clang -std=gnu11 -O1 -g -fsanitize=thread -I"$work/include" -I"$work" -I"$gfx/nvn/include" -I"$gfx/nvn" -I"$gfx/tex" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/sampler_id_cache_test.c android/nvn_vk_sampler.c -o "$work/sampler_id_tsan"
"$work/sampler_id_tsan"
TOTK_VK_SAMPLER_ID_CACHE=0 "$work/sampler_id_tsan"
clang "${common[@]}" "${vk[@]}" android/tests/driver_sampler_test.c android/nvn_vk_sampler.c -o "$work/driver"
"$work/driver"
clang "${common[@]}" android/tests/swizzle_rows_test.c "$gfx/nvn/nvn_tex.c" "$gfx/tex/nvn_deswizzle.c" -o "$work/swizzle_rows"
"$work/swizzle_rows"
clang "${common[@]}" android/tests/texture_bc_fallback_test.c android/nvn_vk_texture_stage.c android/nvn_vk_format.c \
    "$gfx/nvn/nvn_tex.c" "$gfx/tex/nvn_deswizzle.c" "$gfx/tex/nvn_bcdec.c" -o "$work/texture"
"$work/texture"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/upload_pool_test.c -o "$work/upload_pool"
"$work/upload_pool"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/pipeline_prewarm_test.c -o "$work/prewarm"
clang "${common[@]}" "${vk[@]}" -ffunction-sections -fdata-sections -Wl,-dead_strip android/tests/pipeline_memo_test.c -o "$work/pipe_memo"
"$work/pipe_memo"
TOTK_VK_PIPE_MEMO=0 "$work/pipe_memo"
mkdir "$work/prewarm-data"
"$work/prewarm" "$work/prewarm-data"
clang "${common[@]}" "${vk[@]}" android/tests/pipeline_cache_test.c -o "$work/cache"
mkdir "$work/cache-data"
"$work/cache" "$work/cache-data"
"$work/cache" "$work/cache-data"
echo 'All renderer host regressions passed (ASan + UBSan).'

#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ndk_root=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
compiler="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang"
if [[ ! -x "$compiler" ]]; then
  compiler="$ndk_root/toolchains/llvm/prebuilt/darwin-arm64/bin/aarch64-linux-android29-clang"
fi
if [[ ! -x "$compiler" ]]; then
  echo "Android NDK clang not found. Set ANDROID_NDK_HOME to your NDK directory." >&2
  exit 1
fi

out="$repo_root/android/build/android_core"
mkdir -p "$out"
common=(-std=gnu11 -O2 -D__ANDROID__ -DTOTK_GAME_V143 -Wall -Wextra -Werror -Wno-format
  -I"$repo_root/android/compat"
  -I"$repo_root/games/totk/runtime"
  -I"$repo_root/games/totk/third_party/d2r_gfx/gfx/nvn/include")
"$compiler" "${common[@]}" -c "$repo_root/games/totk/runtime/services.c" -o "$out/services.o"
"$compiler" "${common[@]}" -c "$repo_root/games/totk/runtime/threads.c" -o "$out/threads.o"
"$compiler" "${common[@]}" -c "$repo_root/games/totk/runtime/ipc.c" -o "$out/ipc.o"
"$compiler" "${common[@]}" -c "$repo_root/android/android_compat.c" -o "$out/compat.o"
file "$out"/*.o

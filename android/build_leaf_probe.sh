#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ndk_root=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
clang="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
if [[ ! -x "$clang" ]]; then
  clang="$ndk_root/toolchains/llvm/prebuilt/darwin-arm64/bin/aarch64-linux-android26-clang"
fi
if [[ ! -x "$clang" ]]; then
  echo "Android NDK clang not found. Set ANDROID_NDK_HOME to your NDK directory." >&2
  exit 1
fi
mkdir -p "$repo_root/android/build"
"$clang" -std=c11 -O2 -Wall -Wextra -Werror \
  "$repo_root/android/native_leaf_probe.c" \
  -o "$repo_root/android/build/native_leaf_probe"
file "$repo_root/android/build/native_leaf_probe"
"$clang" -std=c11 -O2 -Wall -Wextra -Werror \
  "$repo_root/android/vulkan_probe.c" -lvulkan \
  -o "$repo_root/android/build/vulkan_probe"
file "$repo_root/android/build/vulkan_probe"

# Native ELF TLS (used for a stack slot accessed directly from assembly) is
# available from Android API 29. The Thor runs Android 13 / API 33.
gate_clang="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang"
if [[ ! -x "$gate_clang" ]]; then
  gate_clang="$ndk_root/toolchains/llvm/prebuilt/darwin-arm64/bin/aarch64-linux-android29-clang"
fi
"$gate_clang" -std=c11 -O2 -Wall -Wextra -Werror \
  "$repo_root/android/host_gate_probe.c" "$repo_root/android/host_gate_probe.S" \
  "$repo_root/android/android_hle_stack.c" "$repo_root/android/android_hle_stack.S" \
  "$repo_root/android/android_thread_slots.c" \
  -o "$repo_root/android/build/host_gate_probe"
file "$repo_root/android/build/host_gate_probe"

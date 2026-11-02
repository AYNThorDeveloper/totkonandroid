#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/totk-graphics.XXXXXX")
trap 'rm -rf -- "$task_tmp"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer "$root/android/graphics_settings.c" \
    "$root/android/tests/graphics_settings_test.c" -o "$task_tmp/test"
"$task_tmp/test" "$task_tmp/graphics.cfg" "$@"

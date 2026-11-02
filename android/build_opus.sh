#!/usr/bin/env bash
# Build libopus (user-fetched source in android/third_party/opus-*) as a static
# arm64 library for the game loader. Sources come from the release's own .mk
# lists; generic C paths, float API. Prints the library path, or nothing when
# the source is absent.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
compiler=$1
src=$(ls -d "$here"/third_party/opus-* 2>/dev/null | sort | tail -1 || true)
[[ -n "$src" && -f "$src/include/opus.h" ]] || exit 0
out="$here/build/opus"
lib="$out/libopus.a"
if [[ -f "$lib" && "$lib" -nt "$src/include/opus.h" ]]; then echo "$lib"; exit 0; fi
mkdir -p "$out"
lists() { # print the sources assigned to the given make variables
  python3 - "$src" "$@" <<'PY'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1]); want = set(sys.argv[2:])
for mk in ("celt_sources.mk", "silk_sources.mk", "opus_sources.mk"):
    text = (root / mk).read_text().replace("\\\n", " ")
    for name, value in re.findall(r"^(\w+)\s*=\s*(.*)$", text, re.M):
        if name in want:
            for f in value.split():
                print(root / f)
PY
}
objects=()
while IFS= read -r f; do
  o="$out/$(basename "$f" .c).o"
  "$compiler" -c -O2 -fPIC -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -w \
    -I"$src/include" -I"$src/celt" -I"$src/silk" -I"$src/silk/float" -I"$src/src" "$f" -o "$o"
  objects+=("$o")
done < <(lists CELT_SOURCES SILK_SOURCES SILK_SOURCES_FLOAT OPUS_SOURCES OPUS_SOURCES_FLOAT)
ar=$(dirname "$compiler")/llvm-ar
rm -f "$lib"; "$ar" rcs "$lib" "${objects[@]}"
echo "$lib"

#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk="${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}"
ndk="${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}"
java_home="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21}"
export JAVA_HOME="$java_home"
export PATH="$java_home/bin:$PATH"
tools="$sdk/build-tools/35.0.0"
platform="$sdk/platforms/android-35/android.jar"
clang="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
glslc="$ndk/shader-tools/darwin-x86_64/glslc"
output="$here/build"
asset_bundle="$output/asset_bundle"

for file in "$tools/aapt" "$tools/zipalign" "$tools/apksigner" "$platform" "$clang" "$glslc"; do
    if [[ ! -e "$file" ]]; then
        echo "Missing Android build dependency: $file" >&2
        exit 1
    fi
done

mkdir -p "$output/lib/arm64-v8a"
mkdir -p "$asset_bundle/shaders"
cp "$here/assets/settings.html" "$asset_bundle/settings.html"
"$glslc" --target-env=vulkan1.1 -O "$here/shaders/textured.vert" \
    -o "$asset_bundle/shaders/textured.vert.spv"
"$glslc" --target-env=vulkan1.1 -O "$here/shaders/textured.frag" \
    -o "$asset_bundle/shaders/textured.frag.spv"
"$clang" -std=c11 -O2 -fPIC -Wall -Wextra -Werror -Wno-unused-parameter \
    -DVK_USE_PLATFORM_ANDROID_KHR \
    -I"$ndk/sources/android/native_app_glue" \
    -I"$here/../../games/totk/third_party/d2r_gfx/gfx/nvn/include" \
    -shared "$here/renderer_probe.c" "$here/../thor_gamepad.c" "$here/../graphics_settings.c" \
    "$here/../nvn_vk_format.c" "$here/../nvn_vk_sampler.c" \
    "$here/../nvn_vk_texture_stage.c" \
    "$here/../../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c" \
    "$here/../../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c" \
    "$here/../../games/totk/third_party/d2r_gfx/gfx/tex/nvn_bcdec.c" \
    "$ndk/sources/android/native_app_glue/android_native_app_glue.c" \
    -landroid -llog -lvulkan \
    -o "$output/lib/arm64-v8a/libthorvulkan.so"

rm -f "$output/unsigned.apk" "$output/aligned.apk" "$output/TOTK-Vulkan-Proof.apk"
mkdir -p "$output/classes" "$output/dex"
java_sources=("$here"/java/org/macnativegames/totk/vulkanprobe/*.java)
"$java_home/bin/javac" -source 8 -target 8 -Xlint:-options -classpath "$platform" \
    -d "$output/classes" "${java_sources[@]}"
class_files=()
while IFS= read -r -d '' item; do class_files+=("$item"); done < <(find "$output/classes" -name '*.class' -print0)
"$tools/d8" --lib "$platform" --min-api 26 --output "$output/dex" "${class_files[@]}"

"$tools/aapt" package -f -M "$here/AndroidManifest.xml" -I "$platform" -A "$asset_bundle" \
    -F "$output/unsigned.apk"
(cd "$output" && "$tools/aapt" add unsigned.apk lib/arm64-v8a/libthorvulkan.so)
(cd "$output/dex" && "$tools/aapt" add "$output/unsigned.apk" classes.dex)
"$tools/zipalign" -f 4 "$output/unsigned.apk" "$output/aligned.apk"

keystore="$output/debug.keystore"
if [[ ! -e "$keystore" ]]; then
    "$java_home/bin/keytool" -genkeypair -keystore "$keystore" \
        -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname 'CN=Android Debug,O=Android,C=US' >/dev/null 2>&1
fi
"$tools/apksigner" sign --ks "$keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android \
    --out "$output/TOTK-Vulkan-Proof.apk" "$output/aligned.apk"
"$tools/apksigner" verify "$output/TOTK-Vulkan-Proof.apk"
echo "$output/TOTK-Vulkan-Proof.apk"

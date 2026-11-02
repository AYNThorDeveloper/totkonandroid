# Prepared cache plus local translation

Existing exact cache entries and compatible prepared seeds are used first. A missing variant is translated by the bundled Android NativeAOT library and stored under its exact key. The Mac bridge remains optional. No full shader seed is required.

Build with `DOTNET=/path/to/dotnet` (.NET SDK 10) and `ANDROID_NDK_HOME` configured. The APK build invokes `games/totk/tools/totk_xlate/publish_android.sh` and packages `libtotkshader.so`.

`TOTK_SHADER_LOCAL=0` disables local translation for diagnostics. `TOTK_SHADER_BRIDGE=0` independently disables the Mac bridge; it does not disable local compilation. Prepared seeds and existing caches are retained.

New local translations do not yet generate the optional framebuffer-fetch proof sidecar. Without proof the renderer retains its conservative copy/split path. Prepared sidecars remain effective. This can affect performance, not permission to skip required rendering.

Device gameplay validation is separate from building/installing; do not claim it was tested when only installed.

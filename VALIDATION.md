# Testing

**Reference hardware:** AYN Thor, Snapdragon 8 Gen 2, Adreno 740. The build host is macOS ARM64. Other devices are unverified.

## Build checks

The supported extracted game input has been prepared into runtime data, the Android NativeAOT shader translator built from source, and a local debug APK packaged and signature-verified. The APK is a private build output and is not distributed.

The build used Python 3.14, JDK 21, Android SDK/build-tools 35, NDK 30 and .NET SDK 10.0.403. Restoring dependencies needs network access. NativeAOT emits reflection-analysis warnings, so a successful build doesn't establish that every shader path works.

## Host tests

| Check | Result |
|---|---|
| RomFS backend | 23 scenarios passed with ASan/UBSan |
| Graphics-settings persistence and validation | Passed with ASan/UBSan |
| Renderer host regressions | Passed for the synthetic cases |
| Installation plan without device writes | Passed |
| Source-only publication check | Passed |

Run:

```sh
python3 tools/publication_guard.py
python3 android/test_romfs.py
bash android/test_graphics_settings.sh
# macOS with the Android NDK available:
bash android/test_renderer_fixes.sh
```

GitHub Actions runs the first three checks. See the Actions tab for the result on the current commit. Renderer cases requiring a private shader cache are skipped when no cache is supplied.

## NSP preparation

The importer verifies the supported update's ExeFS hashes before merging RomFS and preparing runtime data. The merge implementation was tested with a Z-A base game and 1.0.2 update: all six IVFC levels passed, and the resulting image matched the existing reference hash. The 2.0.2 importer also correctly rejected that unsupported 1.0.2 update.

Runtime preparation and the Android build were tested using verified extracted TOTK 1.4.3 and Z-A 2.0.2 input. A complete fresh base/update-NSP-to-APK run for both supported versions still needs testing.

## Gameplay coverage

The README describes local play on the AYN Thor. A complete playthrough, empty-cache runs and other device/driver combinations still need more coverage. Passing host tests or building an APK doesn't establish full-game compatibility.

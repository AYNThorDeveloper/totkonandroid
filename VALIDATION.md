# Validation — 2 November 2026

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

## Export build

The publication source was copied into an isolated temporary checkout. Game input and generated output were kept outside the publication directories.

| Check | Result |
|---|---|
| Supported local dump preparation | Passed |
| Android NativeAOT translator built from vendored source | Passed |
| Native Android loader/Vulkan library | Built successfully |
| Debug APK packaging and signature verification | Passed; private test output only |
| RomFS backend regressions | 23 scenarios passed, ASan/UBSan |
| Graphics-settings regressions | Passed, ASan/UBSan |
| Renderer host regression script | Passed; its synthetic checks completed |
| Installation plan without device writes | Passed |
| Production C/header/assembly/C#/GLSL source comparison | Final publication files match the successful build checkout |
| Source-only publication guard | Passed before packaging |

The host used macOS ARM64, Python 3.14, JDK 21, Android SDK/build-tools 35, Android NDK 30 and .NET SDK 10.0.403. Package restoration needed network access. NativeAOT emitted reflection-analysis warnings; successful compilation is not proof that every shader path works. No device was installed, launched or modified during this validation.

## NSP and private data preparation

The command-line wrapper checks supported ExeFS hashes before attempting RomFS merge and runtime preparation. The exported merge implementation was exercised on the owner's Z-A base + 1.0.2 update: all six IVFC levels verified and the merged image SHA-256 matched the previous local reference. The wrapper correctly rejected that 1.0.2 update when configured for the supported 2.0.2 runtime.

Supported TOTK 1.4.3 and Z-A 2.0.2 runtime preparation was exercised using existing verified extracted input. Their original supported update NSP files were not available for a complete fresh base-to-APK rerun of the new wrapper. The tests above establish the preparation/build stages and merge/version-check components; they do not establish a new single-command end-to-end NSP run for both supported versions.

## Remaining validation limits

- No fresh gameplay, cold-cache or full-game test of this export was performed.
- Renderer cases that require private shader caches were skipped when those caches were absent; a passing synthetic suite does not validate all game shaders.
- Performance in README.md refers to the documented local gameplay observations, not a new benchmark of this export.
- No new device/driver compatibility claim follows from a successful host build.
- Hosted workflow results appear in this repository’s Actions tab. The local validation reported above predates upload and does not substitute for hosted CI.
- Source selection and licensing review scope is documented in PUBLICATION_REVIEW.md. Pattern scans are not a legal clearance.

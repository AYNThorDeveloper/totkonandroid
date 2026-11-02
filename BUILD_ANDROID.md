# Build your own Android copy (macOS ARM64)

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

Developer workflow, under validation. Android 10+ ARM64 with a compatible Vulkan GPU is required; gameplay has only been tested locally on AYN Thor. Windows and Linux hosts are not supported by these instructions.

## Tools

Install Python 3.14+, JDK 21, Android SDK platform 35 and build-tools 35.0.0, an Android NDK with API 29 support, CMake, Ninja, Xcode command-line tools, SPIRV-Tools, SPIRV-Cross, and .NET SDK 10 for building the shader translator from source. Set `ANDROID_HOME`, `ANDROID_NDK_HOME` and `JAVA_HOME` to their actual installation directories. Do not copy another person's machine paths blindly.

```sh
python3 -m venv publication-work/venv
source publication-work/venv/bin/activate
pip install -r requirements.txt
python tools/port.py doctor
```

The included Opus and libadrenotools source trees are compiled locally. No prebuilt game library or shader seed is supplied.

## Your own game input

Supply a decrypted, merged TOTK 1.4.3 ExeFS directory (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`) and a raw merged `romfs.bin`. The first supported workflow starts from this already extracted dump. Alternatively, use the command-line NSP importer documented below.

```sh
python tools/port.py prepare --exefs /absolute/path/to/exefs --romfs /absolute/path/to/romfs.bin
```

This verifies every ExeFS file against known 1.4.3 hashes and the full 17,052,625,232-byte RomFS image against its expected SHA-256, indexes the image, rewrites modules for Android and generates private loader tables. It does not modify your inputs. The RomFS remains referenced by a local symlink, so keep the original file at the same location. Inputs and generated outputs remain git-ignored. A failed preparation may leave partial outputs: retry in a fresh checkout, not over a completed preparation.

## Build shaders and APK

```sh
python tools/port.py build
```

The APK build compiles and bundles the Android NativeAOT shader translator (`libtotkshader.so`). Existing exact caches and compatible prepared seeds are used first. A missing shader variant is translated locally on the Android device and cached under its exact key. No full seed, Python installation or .NET installation is required on the phone. First-use translation can cause visible stalls.

Optional Mac-side preparation remains available with `python tools/port.py translator` and `python tools/port.py shaders`, followed by the existing shader-seed transfer helper. Existing seeds and caches need not be replaced. `TOTK_SHADER_LOCAL=0` disables local translation for diagnostics; `TOTK_SHADER_BRIDGE=0` disables only the Mac bridge.
The APK is written to `android/game_activity/build/TOTK-Game-Loader.apk`. It is a debug build signed with a locally generated debug key. It contains generated loader metadata and locally reconstructed Options-menu assets; treat this APK as private game-derived output. The neutral launcher icon is generated from project code.

## Installation

Connect your ARM64 Android device with USB debugging enabled and inspect `adb devices`. The commands require its explicit serial number:

```sh
python tools/port.py install --serial DEVICE_SERIAL
python tools/port.py install --serial DEVICE_SERIAL --apply
```

The first command validates inputs and prints a plan. The second installs the APK and transfers the modules, approximately 17 GB RomFS, and index. Reserve at least 20 GB free on the device. It does not launch the game or import saves. The optional shader transfer helper force-stops the app before changing its shader cache; ordinary installation does not transfer or replace the existing cache.

Existing environment/driver files are not overwritten. On a new device the system Vulkan driver is used; the local Thor configuration and Turnip driver are not bundled or automatically reproduced. Performance and compatibility on another device require testing. This is a USB developer deployment workflow, not yet an in-app game importer.

## Public source versus private output

Only reviewed source, dependency source, documentation and verified non-binary metadata belong on GitHub. Never upload NSP/NCA/XCI, keys, game modules, RomFS, game-derived overlays, shader binaries, APKs from this workflow, saves or private caches. CI should not request game uploads. Preserve `LICENSE`, `THIRD_PARTY_NOTICES.md` and dependency notices.

## Starting from your own NSP files

The command-line importer takes a base game NSP (or XCI), the supported update NSP and your own key file. It verifies the update ExeFS against the supported hashes, merges the base/update RomFS with integrity checks, verifies the resulting image against the reference hash, and prepares private runtime data. It does not build or install an APK.

```sh
python tools/import_nsp.py --base /absolute/path/to/base.nsp --update /absolute/path/to/update.nsp --keys /absolute/path/to/prod.keys
```

The first invocation fetches a pinned hactool source revision and builds the project's bounded-input/private-key-file adaptation using Git, make and clang. That dependency requires network access; no game input is sent. `--hactool /absolute/path/to/hactool` uses an existing build with those project hooks instead. A stock unmodified hactool is insufficient. Allow at least 20 GiB of temporary space in addition to the input files and build dependencies.

Private inputs/outputs remain in ignored `publication-work/` (and, for Z-A, the adjacent `Uitgepakt/` directory). Use a fresh checkout for a new import; the importer refuses to overwrite existing work. `--exefs-only` stops after verifying the executable dump, leaving a partial import for diagnostics; use a fresh checkout for the full workflow afterwards. There is no in-app importer.

The distributed `games/totk/assets/shader_list.tsv` is intentionally empty. Optional Mac seed generation discovers shaders from your own dump; no collected game identifiers or cache are bundled. The Android local translator handles variants encountered during play.

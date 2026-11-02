> Android fork reference device: **AYN Thor**. For current setup, see [the build guide](../BUILD_ANDROID.md) and [reference-device notes](../docs/REFERENCE_DEVICE.md). Older notes below may describe earlier experiments.

# Android ARM64 port probes

This directory contains several Android port probes and the current
headless TOTK 1.4.3 loader milestone. On the connected AYN Thor Max, the
module loader runs original SDK startup and module constructors, verifies and
mounts the 1.4.3 RomFS, and opens a Vulkan 1.3 device. The ordinary path then
stops at Switch display service `vi:m`. A diagnostic-only display substitute
reaches the first NVN lookup, `nvnDeviceInitialize`. It does not display the
game or demonstrate gameplay. See [GFX_BRIDGE.md](GFX_BRIDGE.md).

The earlier `native_leaf_probe` executes two verified leaf functions from the
original TOTK 1.0.0 or 1.4.3 `main.elf`. It applies standard relative
relocations when present.

`host_gate_probe` separately tests the Android host-call gate and import
wrappers on the Thor. It switches from a guarded 4 KiB stand-in guest stack
to a 64 KiB host stack, handles nested calls, and restores the stack slot.
The import wrappers are now used by the game loader; the standalone probe
itself executes synthetic guest code.

## Offline RomFS development (20 October 2026)

The loader includes a read-only Android RomFS backend for mount, file reads
and entry-type queries. It passed synthetic host regressions with ASan/UBSan
and mounted the verified 1.4.3 game image on the Thor. Preparation, test
commands and remaining limitations are in [ROMFS.md](ROMFS.md).

## Load the local 1.4.3 modules on the Thor

The loader milestone consumes the Android-rewritten modules in
`games/totk/rewritten/android-v143`. It generates a private table header from
the local game files and builds an Android ARM64 executable. The tables and
rewritten modules are git-ignored because they contain data derived from the
user's game dump.

```sh
./android/build_module_loader_probe.sh
adb shell mkdir -p /data/local/tmp/totk-android-v143
adb push android/build/module_loader_probe \
  games/totk/rewritten/android-v143/main.patched.bin \
  games/totk/rewritten/android-v143/subsdk0.patched.bin \
  games/totk/rewritten/android-v143/sdk.patched.bin \
  /data/local/tmp/totk-android-v143/
adb shell chmod 700 /data/local/tmp/totk-android-v143/module_loader_probe
adb shell /data/local/tmp/totk-android-v143/module_loader_probe \
  /data/local/tmp/totk-android-v143 clock
```

The loader checks module SHA-256 hashes, maps `main`, `subsdk0`, and `sdk`,
applies RELA/RELR relocations, and sets Android memory protections. The
`clock` mode calls the original SDK `GetSystemTick` function from four native
threads, 1,000 times per thread, through independent Android TLS slots and
the host gate. It does not run
Switch services, module constructors, `nn::init::Start`, or the game entry
point. On the connected Thor, all three modules loaded, all 554,281
relocations applied, no strong imports were unresolved, and all 4,000 timer
calls passed across four threads.

## Run the 1.4.3 startup checkpoint

After pushing the three rewritten modules as above, this command starts the
original `nn::init::Start`, dispatches the Switch services observed during SDK
initialization, runs all three module constructors, and reaches the original
`nnMain` focus setup. To stop at that verified point and print
`"status":"pass"`, run:

```sh
adb shell 'TOTK_STOP_AT_STARTUP_CHECKPOINT=1 /data/local/tmp/totk-android-v143/module_loader_probe /data/local/tmp/totk-android-v143 boot'
```

With `TOTK_ROMFS` set to the verified data directory, the probe continues
through game initialization and RomFS mount, then opens a Vulkan device. The
ordinary path stops at `vi:m`. `TOTK_PROBE_HEADLESS_VI=1` advances the
diagnostic path to `nvnDeviceInitialize` without a real screen. The Android
service, thread, and IPC implementations are linked into this loader;
the separate `build_android_service_core.sh` command remains useful for
compiling those sources by themselves.

## Build

Install the Android NDK and platform tools, then run:

```sh
./android/build_leaf_probe.sh
```

The outputs are `android/build/native_leaf_probe` (API 26+) and
`android/build/host_gate_probe` (API 29+). The gate uses Android ELF TLS via
`TPIDR_EL0`; API 29 is required for native ELF TLS.

The SVC, thread, and IPC code is linked into the current loader. The separate
`./android/build_android_service_core.sh` script remains as an isolated
compatibility build for those shared sources.

Before a game dump is available, `python3 android/make_synthetic_fixture.py`
creates a small test ELF. Running the probe against it checks mapping,
relocations, and execution of the two instruction sequences on the device,
but does not verify the user's game file.

## Prepare a local game module

Use your base-game 1.0.0 XCI or NSP and keys. From `games/totk`, the existing
extractor produces `extracted/v0/elf/main.elf`:

```sh
make dependencies
make extract ROM=/path/to/your/base-game.nsp KEYS=/path/to/your/prod.keys
make modules
```

For an update 1.4.3 NSP, extract its code separately:

```sh
./.venv/bin/python scripts/extract_exefs.py /path/to/your/update.nsp \
  --keys /path/to/your/prod.keys --update --output extracted/v143/exefs
./.venv/bin/python scripts/prepare_modules.py extracted/v143/exefs \
  --output extracted/v143/elf
```

Keep the game file and keys on your computer. The probe needs only
`main.elf` on the Android device.

## Run on an attached device

Enable USB debugging, connect the device, and confirm it appears in
`adb devices`. Then run:

```sh
adb push android/build/native_leaf_probe /data/local/tmp/native_leaf_probe
adb push games/totk/extracted/v0/elf/main.elf /data/local/tmp/totk-main.elf
adb shell chmod 700 /data/local/tmp/native_leaf_probe
adb shell /data/local/tmp/native_leaf_probe /data/local/tmp/totk-main.elf
```

For 1.4.3, push `games/totk/extracted/v143/elf/main.elf` to the device and
append `v143` to the probe command.

Success is a JSON line with `"status":"pass"`, the two function names and
returned offsets. A failure means only this first loader proof failed; it does
not establish that a full Android port is impossible.

Run the independent host-call gate probe with:

```sh
adb push android/build/host_gate_probe /data/local/tmp/totk-host-gate-probe
adb shell chmod 700 /data/local/tmp/totk-host-gate-probe
adb shell /data/local/tmp/totk-host-gate-probe
```

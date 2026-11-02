# Android read-only RomFS milestone (20 October 2026)

`android_romfs.c` replaces the previous cache-size-only stub. It routes
`MountRom`, both cache-size queries, `OpenFile`, `GetFileSize`, `CloseFile`,
`GetEntryType` and the three existing `ReadFile` signatures to an Android
POSIX backend. SDK filesystem initialization remains data-free; mounting is
where data is opened and checked. Only one named read-only mount is supported.

This passed synthetic Mac tests (17 scenarios under ASan/UBSan) and was run
with the verified 1.4.3 image on the Thor on 20 October. The loader printed
`[android-romfs] read-only backend ready: 304243 files` and continued until
the graphics startup path. The current ordinary-loader blocker is `vi:m` after
the Vulkan device is initialized; see `GFX_BRIDGE.md`.

## Data preparation

Use the existing extraction workflow to obtain a fully IVFC-verified **1.4.3**
RomFS in `games/totk/extracted/v143/romfs`. The executable modules are 1.4.3;
do not supply a base-game 1.0.0 image. See `games/totk/scripts/extract_update_romfs.py`
for merging the user's base NSP and update with a BKTR-capable hactool reader.
The script requires the executable path through `--hactool`. The pinned source
revision and local patch/build procedure are in `build_app.py:build_hactool`.
The build produced `games/src/hactool/hactool` (ignored as fetched source).
The merged 1.4.3 image passed all six IVFC levels and matched the size and
SHA-256 in `games/totk/assets/expected-v1.4.3.json`; its `index.json` records
304,243 files. Do not substitute an unverified image.

From the repository root, after extraction:

```sh
(cd games/totk && python3 scripts/prepare_romfs_backend.py --version v143)
./android/build_module_loader_probe.sh
```

The existing preparation script produces `runtime-index.bin` and the private
`games/totk/build/v143/romfs_config.h`. The Android build reads that header
when present. The resulting executable is tied to those exact index/image
hashes. Without the header, the loader still builds for clock/checkpoint tests,
but MountRom fails with a specific missing-metadata message.

On mounting, the backend checks the binary index hash, header, sorted paths,
string bounds and file ranges, then the full image size and SHA-256 before
serving bytes. It reads from the opened image with `pread`; it does not load
the entire image into RAM. Full image hashing happens on each process start,
so mounting a real image can take time. Keep the image unchanged while running.
Hashes bind files to build metadata; version matching relies on preparing the
correct local version, rather than detection by this backend.

## Thor test result

Push the rebuilt loader/modules as described in `README.md`, then copy
`romfs.bin` and `runtime-index.bin` to a data directory on the Thor with enough
free space. Preserve the existing private game-data files. For example, using
`/data/local/tmp/totk-android-v143/romfs` as that data directory:

```sh
adb shell 'TOTK_ROMFS=/data/local/tmp/totk-android-v143/romfs TOTK_STOP_AT_STARTUP_CHECKPOINT=1 /data/local/tmp/totk-android-v143/module_loader_probe /data/local/tmp/totk-android-v143 boot'
adb shell 'TOTK_ROMFS=/data/local/tmp/totk-android-v143/romfs /data/local/tmp/totk-android-v143/module_loader_probe /data/local/tmp/totk-android-v143 boot'
```

The first command verifies that the existing startup checkpoint still works.
The second showed `[android-romfs] read-only backend ready` and then stopped
at the graphics startup path. A successful mount does not imply a
successful title screen or renderer startup.

## Local verification

```sh
python3 android/test_romfs.py
```

This compiles the actual backend with AddressSanitizer and UndefinedBehaviorSanitizer.
It checks exported SDK mappings, lazy initialization without game data, mount,
reads, exact reads, offsets, EOF, directory/file types, root normalization,
traversal rejection, write-mode rejection, stale handles, handle-table exhaustion,
400 concurrent open/read/close cycles, image corruption, and structurally invalid
indexes with matching build hashes. The fixture contains no game bytes.

Remaining: writable save/cache storage,
directory enumeration, split images, overlays and later filesystem SDK calls.
The Vulkan game renderer is a separate, much larger unfinished component.

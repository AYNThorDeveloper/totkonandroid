# Contributing

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

Keep contributions source-only. Do not attach APKs, game files, firmware, keys, extracted modules, saves, shader binaries or caches to commits, releases, issues or pull requests. Use synthetic fixtures in tests.

Run `python3 tools/publication_guard.py`, `python3 android/test_romfs.py` and `bash android/test_graphics_settings.sh` before contributing. On macOS with an Android NDK, `bash android/test_renderer_fixes.sh` exercises additional renderer regressions; game-derived shader checks are skipped when no private cache is provided.

For a bug report, include the source commit, game version, device/SoC, Android version, driver, resolution, frame-cap settings, reproduction steps, expected behavior and observed behavior. Distinguish measured frametimes from estimates. Redact personal paths, device identifiers and private game input before sharing diagnostic text. Check the README's known bugs first.

Preserve dependency licenses and identify the origin and license of any imported source. The repository's license does not grant rights to game or firmware content.

# Android game activity checkpoint (20 October 2026)

> Superseded in part (20 October, evening): the NVN dispatcher described below was replaced by the shared NVN layer on Vulkan. See `../VULKAN_BACKEND.md` and `../SESSION_HANDOFF.md`.

`build_apk.sh` packages the actual 1.4.3 loader as a NativeActivity library.
It receives the activity's `ANativeWindow` and passes it to the VI bridge.
It does not use the synthetic four-colour texture renderer. The APK is a
diagnostic build and does not show a TOTK frame.

```sh
bash android/game_activity/build_apk.sh
adb install -r android/game_activity/build/TOTK-Game-Loader.apk
adb shell am start -n org.macnativegames.totk.gameprobe/android.app.NativeActivity
adb logcat -d -s TOTKGameLoader:I
```

On Thor the APK installed and obtained a 1920×1080 native window. It mapped
all three rewritten 1.4.3 modules, applied their relocations, ran original
SDK startup and all constructors, entered `nnMain`, verified/mounted the
304,243-file RomFS, opened Vulkan on Adreno 740, and initialized VI with the
actual Android window. The app reads the existing data under
`/data/local/tmp/totk-android-v143`; another 17 GB copy was unnecessary.

The first activity blocker was a verified ABI mismatch: rewritten guest code loads its
emulated x18, TLS, TLR and host-gate slots at fixed offsets from `TPIDR_EL0`.
That holds in the standalone executable. An Android library loaded by
NativeActivity uses dynamic TLS, with different offsets on its threads. On
Thor, the activity's first game thread had offsets beginning `0x3814db090`;
the next had offsets beginning `0x38f459110`.

`android_dynamic_tls.c` now adapts the verified in-memory modules after their
SHA-256 check: 43,488 main, 9,784 subsdk0 and 15,797 SDK slot references use
per-thread TLSDESC resolution. Every rewrite validates the original four-word
instruction pattern; the original files and their hashes remain intact. The
trampolines preserve x0/x1/x2/x30 and NZCV below the original 128-byte scratch
area, use the resolver's register-preservation ABI, and leave Android x18
untouched. Extra code occupies about 5.8 MiB. A second issue was exposed by
the changed mapping layout: QueryMemory described unknown host pages as
executable. Android now reports those pages and gaps as inaccessible.

The loader now runs on a worker while the activity drains lifecycle/input
events. Android Choreographer frame callbacks signal bound VI vsync events.
The new NVN dispatcher implements initial device setup and version queries;
other function pointers fail explicitly at use, naming the missing operation.
Actual GPU command, texture, shader and presentation integration is unfinished.
The activity exits on surface loss because NVN resource recreation is pending.

The latest full Thor run (20:49 on 20 October) confirmed `Android surface=1`,
VI registration with Choreographer, successful `DeviceInitialize` and version
queries 55/15. It then stopped explicitly at device property `0x28`,
`SUPPORTS_MIN_MAX_FILTERING`. Frame callback delivery and game rendering have
not yet been measured. Standalone SDK-startup regression also passed.

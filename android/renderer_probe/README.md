# Thor Vulkan texture proof

This Android NativeActivity opens a Vulkan swapchain with default 1280×720
buffers and presents a fullscreen textured triangle near 30 Hz. Its 4×4
quadrant texture is synthetic RGBA8 data: the probe first stores it in the
existing Tegra/NVN layout, passes it through `nvn_vk_texture_stage.c`, uploads
the staged bytes into an optimal Vulkan image, and samples that image in a
fragment shader. This proves one texture-format and upload route end to end; it
does not load TOTK, game shaders, or game assets. The app also logs key events
and joystick axes. `android/thor_gamepad.c` converts those events to the
existing `RuntimeGamepadState` used by the game's HID implementation, with the
default controller layout.

Build on the Mac with Android NDK, SDK platform 35, build-tools 35, and Java 21:

```sh
./android/renderer_probe/build_apk.sh
adb install -r android/renderer_probe/build/TOTK-Vulkan-Proof.apk
adb shell am start -n org.macnativegames.totk.vulkanprobe/.ThorActivity
adb logcat -s TOTKVulkanProof:I
```

The APK is signed with a local debug key in the ignored `build/` directory.
The package is `org.macnativegames.totk.vulkanprobe`; uninstall it with
`adb uninstall org.macnativegames.totk.vulkanprobe`. The upper-screen
screenshot can be captured with `adb exec-out screencap -p > thor-screen.png`.

The logged frame count measures this synthetic textured presentation loop.
The left stick (X/Y), right stick (Z/RZ), and Android button codes 96/97 have
been observed from the built-in `Odin Controller`. The Android key layout supplies
the other standard button codes, while their physical labels and the trigger
threshold still require confirmation. Keys or hat axes map to the D-pad;
BRAKE/GAS analog axes map to ZL/ZR, with a 0.55 threshold. The game's input
runtime is not linked into this proof APK yet.

## Graphics settings menu (20 October 2026)

The APK now includes a local WebView overlay over the native Vulkan surface.
Open it with the top-right **Beeld** button or **Start**. Touch, D-pad, left
stick and logical Android A/B keys navigate it. Native input handling yields
to the overlay while it is open, and clears the guest controller state when
opening/closing. Physical face-button labels still need Thor calibration.

The menu has a dark green/teal palette, warm text, serif headings and geometric
accents. It uses no Nintendo images or fonts. Browser preview:
`android/renderer_probe/assets/settings.html` (no persistent settings outside
Android). The HTML/CSS is bundled in the APK; no website is contacted.

- Render: 640×360, 960×540, 1280×720. Stored for the next game start.
- Output: 960×540, 1280×720, 1920×1080. Recreates the proof swapchain on the
  render thread, reports its actual extent, and attempts the previous working
  extent if creation fails. The driver may clamp the requested size.
- Reset chooses 720p/720p; **Instellingen bewaren** commits it. **Terug** discards
  unsaved choices. Native saving validates presets and atomically replaces a
  private `files/graphics.cfg` with mode 0600. Errors leave the old config intact.

This is still a proof app without a game renderer. On 20 October the rebuilt
APK ran on Thor: the log confirmed synthetic RGBA8 upload, a textured SPIR-V
pipeline, a 1280×720 swapchain and presented frames. The on-device screenshot
is `../../../Test/TOTK-Vulkan-Textuur-Proef-2026-10-20.png`. BC fallback, real
game textures and shaders, menu touch/controller navigation, Vulkan resizing,
and lifecycle transitions remain unverified on Thor. Browser preview
selection/save/reset/cancel/keyboard checks were completed for the menu work.

At startup, the native log reports Vulkan sampled-image, linear-filter,
color/depth attachment, storage-image, and transfer support for every mapped
NVN format. This checks the format table against the active GPU; it does not
create or upload those textures. Packed formats are marked for later bit-order
validation.

`android/nvn_vk_texture_stage.c` prepares color textures for a Vulkan upload:
it unpacks supported block-linear and pitch/linear layouts, packs mip levels
and array layers into one staging allocation, and returns `VkBufferImageCopy`
regions plus image creation metadata, including the NVN texture swizzle composed with format
fallback channels. `android/nvn_vk_sampler.c` maps NVN filter, wrap, LOD and
comparison state to Vulkan sampler descriptions; it reports when anisotropy,
mirror clamp or min/max reduction features are required and rejects states
that cannot be represented safely. The probe now calls the staging helper for
the synthetic 4×4 RGBA8 image and the sampler mapper for its nearest sampler,
uploads that image, binds it through a combined image sampler, and draws it to
the swapchain. Depth/stencil and packed formats with unverified NVN bit order
are rejected. This validates only the synthetic RGBA8 route; actual game
textures, their per-texture swizzles, shader resource bindings, and Thor GPU
behavior still need validation.

The staging layer also has an optional BC fallback for devices that lack
sampled/transfer support for a BC format. It reuses the existing CPU decoder
after deswizzling and updates the Vulkan copy regions for uncompressed pixels.
The probe selects the path from the physical device's format features; its
current synthetic RGBA8 image exercises only the regular path. See
[`WONDER_REFERENCE.md`](../WONDER_REFERENCE.md) for the source comparison and
remaining device checks.

## Connect the chosen render resolution to the headless loader

The loader now accepts `TOTK_GRAPHICS_CONFIG=/path/to/graphics.cfg`. It reads the
same versioned format, validates all four original 1.4.3 instructions, then
applies the existing scene-size patch before making code executable. The data
lives in the reserved tail page, separate from code and Android TLS keys.
No config variable means the existing startup behavior is preserved.

The app and headless loader are still separate processes. For now copy the
app's saved config through the development shell (this APK is debuggable):

```sh
adb exec-out run-as org.macnativegames.totk.vulkanprobe cat files/graphics.cfg > android/build/thor-graphics.cfg
adb push android/build/thor-graphics.cfg /data/local/tmp/totk-android-v143/graphics.cfg
adb shell 'TOTK_GRAPHICS_CONFIG=/data/local/tmp/totk-android-v143/graphics.cfg TOTK_STOP_AT_STARTUP_CHECKPOINT=1 /data/local/tmp/totk-android-v143/module_loader_probe /data/local/tmp/totk-android-v143 boot'
```

Then continue boot with matching RomFS as documented in `../ROMFS.md`. The
loader uses **render** size; its **output** size will matter after app/loader/game
renderer integration. There is no live in-game resolution switching yet.

Host regression tests (no device required):

```sh
bash android/test_graphics_settings.sh
# Optional: check the actual private rewritten file without executing it:
bash android/test_graphics_settings.sh games/totk/rewritten/android-v143/main.patched.bin
```

Tests cover persistence, invalid/overflowed inputs, preserving previous config
on rejected writes, patch bounds and version guards, ADRP target decoding,
resolution floats, and rejecting double patches with ASan/UBSan. The optional
local-file check passes; it proves instruction layout, not game rendering.

On the Thor test: save each output size, inspect the active extent in the menu
and logcat, close/reopen the menu, relaunch the app to check persistence, and
background/foreground it. Test both touch and controller, including input
isolation and A/B positions. Then run the loader checkpoint with the copied
config before attempting further game startup.

# Android graphics checkpoints (20 October 2026)

> Later on 20 October the game passed NVN device, pool and window setup on the Vulkan backend; see `VULKAN_BACKEND.md`.

The 1.4.3 loader intercepts `SetGraphicsAllocator`,
`SetGraphicsDevtoolsAllocator`, and `InitializeGraphics`. The activity path
creates a Vulkan 1.3 device, a real Android Vulkan surface and a graphics/present
queue on Adreno 740. On the Thor, `nvnDeviceInitialize` succeeds and the guest
ABI-version queries return 55.15. The current next stop is device property
`0x28` (`SUPPORTS_MIN_MAX_FILTERING`). Vulkan swapchain, resource and draw
integration for the game remain unfinished. Unsupported functions retain
named failure dispatch; they fail when called instead of at table lookup.

With the verified RomFS mounted, the ordinary headless run reaches this Vulkan
device and then stops at Switch display service `vi:m`. This is expected until
the game loader runs with an Android activity and `ANativeWindow`. Do not
pretend the service is implemented merely to advance startup.

`TOTK_PROBE_HEADLESS_VI=1` enables a diagnostic-only substitute for the five
VI functions used by the macOS port. It binds a genuine SDK system-event object
for vsync but has no timer, and returns inert display/window tokens. On Thor it
reached `nvnBootstrapLoader` and recorded the first requested function:
`nvnDeviceInitialize`. This was the earlier checkpoint; current dispatch advances through device creation.

The separate `renderer_probe` APK **did** run on Thor. It uploaded a synthetic
NVN-layout RGBA8 texture, created a SPIR-V textured graphics pipeline and a
1280×720 Vulkan swapchain, and presented frames. The screenshot is
`../../Test/TOTK-Vulkan-Textuur-Proef-2026-10-20.png`. This validates the
synthetic texture path and Android surface independently of the game loader;
it says nothing yet about real game textures or shaders.

The previously intermittent SDK time-service abort came from Android reporting
the unreserved rest of the 39-bit address range as available ASLR space. The
SDK sometimes exhausted its shared-memory address search. Android now reports
the reserved `LOW_SIZE` region, and 20 consecutive on-device startup checkpoints
passed after the change. This does not prove all later guest allocations fit.

## Next integration milestone

1. Continue NVN device and Vulkan resource integration in the activity. The
   dynamic TLS fix has passed SDK startup, constructors and RomFS mount on
   Thor. The activity supplies a real window; Choreographer signaling has
   been added to the VI event bridge.
2. Implement `nvnDeviceInitialize` and the functions actually requested through
   `nvnBootstrapLoader`, using the existing NVN-on-Metal semantics and Vulkan
   texture/shader building blocks. Keep unsupported calls explicit.
3. Draw one real game texture with its translated SPIR-V shader, then the first
   title-screen frame. The synthetic proof must not be reported as TOTK output.

The 17 GB RomFS is checked with SHA-256 at each loader start; a full diagnostic
run therefore takes about a minute after the startup checkpoint. The device
copy is `/data/local/tmp/totk-android-v143/romfs`.

The `game_activity` APK obtains a real 1920×1080 `ANativeWindow`. The dynamic
TLS mismatch is fixed by guarded in-memory TLSDESC trampolines; it now runs
SDK startup, constructors, nnMain, full RomFS verification, Vulkan and VI in
the activity process. See `game_activity/README.md` for evidence and limits.
The latest build adds Vulkan surface/present-queue setup and initial NVN
device dispatch. Missing functions stop at actual invocation with their name.

## Host-call ABI checkpoint

Android imports now use stable AArch64 thunks in `android_hle_stack.S` instead
of calling host replacements directly from a guest stack. Each guest thread
can switch to its reserved host stack; nested host calls stay there, and the
slot is restored after return. This is required before returning Vulkan-backed
function pointers from `nvnBootstrapLoader`. The Thor's standalone gate probe
passed with a guarded 4 KiB guest stack, a 64 KiB host stack, an 8 KiB host
frame and a nested wrapped call. The rebuilt game loader passed startup and
again reached `nvnDeviceInitialize` after full RomFS verification. Initial NVN device
functions now execute through these wrappers.

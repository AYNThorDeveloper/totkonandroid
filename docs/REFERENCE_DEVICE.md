# Reference device: AYN Thor

**The AYN Thor is the reference model for this Android project.** Local performance descriptions, controller instructions and compatibility observations refer to that device. Other Android devices are unverified, even when they have an ARM64 CPU and Vulkan support.

| Item | Reference |
|---|---|
| Device | AYN Thor |
| SoC | Snapdragon 8 Gen 2 |
| GPU | Qualcomm Adreno 740 |
| Architecture / graphics API | ARM64 / Vulkan |
| Local controls | Integrated controller; L3 + R3 opens video settings |
| Host used to build the export | macOS ARM64 |

I use Turnip 27 for local TOTK testing. The open-field figures in the README were recorded with Turnip V38, at around 35–40 fps in heavier sections. I haven’t established the internal render resolution for that run; the README includes the sampled frame times.

Driver binaries, device-specific private data and firmware are not distributed. For reports from another device, provide its exact model, SoC, Android version, driver and render/output settings, and state that it differs from the AYN Thor reference model.

Inherited upstream Apple documentation is retained for source context. It does not change this Android fork's reference device. Follow the current README.md and BUILD_ANDROID.md for setup.

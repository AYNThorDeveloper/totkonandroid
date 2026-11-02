# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `games/totk/third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `games/totk/third_party/d2r/LICENSE` |
| `games/totk/third_party/d2r_gfx` (NVN on Metal, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `games/totk/third_party/d2r_gfx/LICENSE` |
| texture layout sizes in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `games/totk/third_party/d2r_gfx/LICENSE.deko3d` |
| `games/totk/third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `games/totk/third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `games/totk/tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `games/totk/tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `games/totk/tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| `games/totk/prebuilt/macos-arm64/totk_xlate` (the shader translator, a native program) | built from `vendor/ryujinx` + the patches + `games/totk/tools/totk_xlate` with `publish_native.sh`; contains .NET runtime libraries compiled in by NativeAOT | MIT (Ryujinx, d2r-mobile, .NET) | `vendor/ryujinx/LICENSE.txt`, `games/totk/tools/totk_xlate/LICENSE` |
| audio effects in `games/totk/runtime/audio_reverb.c`, `games/totk/runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `games/totk/third_party/licenses/Ryujinx-Audio.txt` |

Fetched or linked at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | SPIR-V to Metal Shading Language | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | Base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| libopus (Homebrew), linked statically | Opus audio | BSD-3-Clause |
| libvpx (Homebrew), linked statically | VP9 movies | BSD-3-Clause |

## Android additions

The Android source snapshot includes Opus 1.5.2 (BSD-style, see `android/third_party/opus-1.5.2/COPYING`), libadrenotools (see `android/third_party/libadrenotools/LICENSE`), and its nested liblinkernsbypass source (see `android/third_party/libadrenotools/lib/linkernsbypass/LICENSE`). Preserve those notices in redistributions. The NDK C++ runtime is linked by the Android build and retains its own license. The shader translator binary is generated locally rather than shipped in this draft.

Android dependencies included as source:

| Code | Upstream | License |
|---|---|---|
| `android/third_party/libadrenotools` | https://github.com/bylaws/libadrenotools | BSD-2-Clause; see its LICENSE |
| `android/third_party/libadrenotools/lib/linkernsbypass` | https://github.com/bylaws/liblinkernsbypass | BSD-2-Clause; see its LICENSE |
| `android/third_party/opus-1.5.2` | https://opus-codec.org/ | BSD-3-Clause; see its COPYING and component notices |

The Android runtime is adapted from the local fork of [MacNativeGames](https://github.com/ly05010419/MacNativeGames). Original notices are retained. Dependencies retain their own licenses. No game or firmware license is granted by this repository. Prebuilt translator binaries described in inherited documentation are not distributed in this source-only export.

`android/third_party/libadrenotools/src/hook/kgsl.h` is a Linux UAPI header marked `GPL-2.0 WITH Linux-syscall-note`, not BSD-2-Clause. Its original notices are retained and the complete license and exception are supplied under that dependency's `licenses/` directory (copied from the Linux kernel's LICENSES tree). Optional libadrenotools developer tools are omitted; the driver-loader and hook sources required for these builds remain included.

The Opus release also contains GNU Autoconf/Automake/Libtool helper scripts under their stated GPL-2.0-or-later or GPL-3.0-or-later terms and special distribution exceptions. Their per-file notices and exceptions remain intact; full GPLv2 and GPLv3 texts are included under `android/third_party/opus-1.5.2/licenses/`. These helper licenses are distinct from the Opus codec's BSD license. The GPLv3 text is from https://www.gnu.org/licenses/gpl-3.0.txt; the GPLv2/UAPI exception texts are from the Linux kernel LICENSES tree.

The reconstructed NVN interface definitions in `games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h` retain their own MIT notice, copyright 2020 h1k421. They are distinct from Nintendo SDK binaries or SDK source, which are not distributed.

# Source origin and publication boundary

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

Prepared 2 November 2026 from the project owner's local Android development fork. The inherited project is MacNativeGames (https://github.com/ly05010419/MacNativeGames); its MIT notice is preserved in LICENSE. THIRD_PARTY_NOTICES.md identifies imported dependency trees and their license files. Android-specific runtime and Vulkan changes are included as source. Vendored dependency source is included directly; no submodule download is needed for it.

Publication contains text source, build scripts, project-authored UI markup, documentation, test code and hash/size metadata. No inherited Git history, APK, game executable, game data, firmware, private key, save, game artwork or shader binary/cache is included. Game-specific address and instruction guards are patch implementation details; complete game modules and generated relocation/symbol tables are produced from each user's own input outside the source distribution.

The original source is licensed software, not copyright-free material. Preserve its notices. Project and game names identify compatibility and do not imply endorsement. This source audit establishes file selection and retained notices; it is not a legal determination about every reverse-engineering technique.

Current setup instructions are README.md and BUILD_ANDROID.md. README.upstream.md and older platform documents describe the inherited Apple implementation and are retained for technical reference and attribution. Private session handoffs and historical visual reviews are omitted.

The documentation now includes original text-only SVG badges and one SHA-256-approved screenshot of the project-authored settings HTML. It was rendered with illustrative bridge values in an isolated browser, not captured from gameplay. These are the only approved visual documentation assets; no game artwork or official logo was added.

Presentation names and explanatory comments have been adapted for this fork. Original third-party copyright notices and the complete operative license terms are retained. Links to upstream projects identify source provenance; no periodic upstream synchronization is configured. Some build-time dependencies are fetched explicitly by the build tools.

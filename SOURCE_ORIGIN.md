# Source and credits

**AYN Thor** is the reference device: Snapdragon 8 Gen 2, Adreno 740, ARM64 and Vulkan. See [the hardware notes](docs/REFERENCE_DEVICE.md).

This project builds on [MacNativeGames](https://github.com/ly05010419/MacNativeGames). The Android runtime and Vulkan backend are adapted from that codebase. Its MIT notice is preserved in LICENSE, and THIRD_PARTY_NOTICES.md lists the other dependencies and their licenses.

Dependency source is included directly where needed. There is no scheduled upstream synchronization; updates are handled manually. Some build tools fetch pinned dependencies when you build.

Game files, firmware, keys, extracted modules, saves and game shader caches are not included. You provide your own input and generate the private runtime data locally. The reference metadata contains hashes and sizes used to check supported input, not game content.

The badges are original SVGs. The settings screenshot comes from the HTML settings menu with illustrative values; it contains no game imagery. The screenshot is the one approved binary documentation asset, checked by its digest in tools/publication_guard.py.

The license covers the code whose authors grant it. It grants no rights to Nintendo game or firmware content. Original third-party copyright notices and license terms are retained. This project is in no way affiliated with, endorsed by, or sponsored by Nintendo.

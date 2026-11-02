# Publication content review — 2 November 2026

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

The publication boundary was checked on the selected source export, not on the original development folder. Existing Git history was not copied.

## Excluded content

No APK/AAB, NSP/XCI/NCA, ExeFS module, RomFS image, firmware, key file, save, game artwork, video, game shader binary/cache, or generated game relocation/symbol table was selected. Source files were also searched for common token/private-key markers, embedded image encodings, generated Android game tables, Nintendo copyright notices, and confidential/proprietary markers. The final source guard rejects blocked file types, private output directories, symlinks, binary content and selected embedded-data/secret markers.

The selected files were also compared against actual nontrivial key values in the owner's local private key file: no literal matches were found. The key values were neither printed nor saved in the review output.

The source flag scan's one proprietary-instruction comment in Ryujinx describes an instruction that is not implemented; it is not an imported proprietary implementation. Collected game shader lists and boot-prefetch data are omitted; the distributed shader-list template is empty.

## Licensed source that remains

- Original MacNativeGames/d2r code and Ryujinx shader translator, with their MIT notices.
- Reconstructed NVN declarations with the original h1k421 MIT notice; no Nintendo SDK tree.
- Opus codec source and its licensed data tables, preserving release notices and GNU helper-script exceptions.
- libadrenotools and linkernsbypass source, preserving BSD notices and the separate Linux UAPI header's GPL-2.0 WITH Linux-syscall-note terms.
- Small libadrenotools patch bytes accompanied by their project assembly source; these are driver-loader support, not copied game instructions.
- Project runtime, Vulkan backend, tools, synthetic tests, UI markup and hash/size reference metadata.

Z-A additionally uses a small MIT-licensed storage-merge support shader adapted from PokeZ-ARecomp. Only GLSL and the original license are distributed; its compiled header is generated locally. It is not a game shader.

Unused libadrenotools developer tools, private session handoffs and historical visual reviews were omitted. Full license/exception texts were added where dependency headers and build helpers use terms different from the top-level dependency license. See THIRD_PARTY_NOTICES.md and SOURCE_ORIGIN.md.

## Meaning of this review

The retained code is licensed source, not copyright-free material. The review establishes the selected file boundary, detected content and retained notices. Automated pattern checks cannot prove the provenance of every line or settle the legal treatment of reverse engineering. No game or firmware distribution rights are granted. Users generate private game-derived output from their own input.

The documentation now includes original text-only SVG badges and one SHA-256-approved screenshot of the project-authored settings HTML. It was rendered with illustrative bridge values in an isolated browser, not captured from gameplay. These are the only approved visual documentation assets; no game artwork or official logo was added.

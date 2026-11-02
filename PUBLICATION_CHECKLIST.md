# Source publication checklist

**Reference device: AYN Thor.** See [reference configuration](docs/REFERENCE_DEVICE.md); observations on other hardware are unverified.

Completed for this export, 2 November 2026:

- [x] Separate source-only directory, fresh Git repository, no inherited history or remote.
- [x] No APKs, game containers, extracted game modules, firmware, keys, saves, artwork or game shader caches selected.
- [x] Runtime/backend source, dependency source, build/import/install helpers and current setup instructions included.
- [x] README performance observations and known bugs documented.
- [x] Dependency license notices reviewed and missing license/exception texts supplied.
- [x] Private handoffs and unused auxiliary tooling omitted.
- [x] Isolated dump preparation, Android build and local APK signature verification succeeded.
- [x] RomFS, graphics-settings and renderer host checks passed within the scope in VALIDATION.md.
- [x] Source guard and GitHub Actions source-check workflow included.

Publishing is a separate step: authenticate GitHub, select the account/repository names, configure the commit author, then commit and push only the selected source tree. Start with private visibility if a final account-side review is desired. No APK release should be attached.

Device/gameplay validation limits and the new NSP wrapper's test scope are explicit in VALIDATION.md. The project remains experimental; source publication does not mean its known gameplay bugs are resolved.

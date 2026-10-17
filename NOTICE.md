# Notice

_What the project license covers, and what it does not._

---

Copyright (c) 2026 LuKa (@LuKa_dev).

The code and documentation written for this project are free software: you can redistribute
them and/or modify them under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your option) any later
version ([LICENSE](LICENSE)). They are distributed WITHOUT ANY WARRANTY; see the license for
details. Keep this notice and the copyright lines in the source files when you redistribute.

## 🧩 Third-party material in this repository

The following parts belong to their owners. They are included for building, testing,
interoperability and research, and the project license does not relicense them.

| Path | Owner and terms |
| --- | --- |
| `java_patch/de/`, `java_patch_cn/` | Replacements of stock HMI classes: the stock parts remain the owners'; the project license covers our changes |
| `deploy/pf/stock/`, `deploy/**/stock/` | Stock firmware files of the head unit (Harman), as shipped or binary-patched at documented offsets |
| `maneuver_render/resources/fonts/` | Fonts of the head unit and the virtual cockpit (Audi AudiType and others); Source Han Sans is under the SIL Open Font License |
| `toolchain/qnx65-abi/include/screen/` | QNX Screen header (QNX Software Systems) |
| `toolchain/qnx65-abi/include/OMX/` | OpenMAX IL headers (The Khronos Group, MIT-style license in each file) |
| `toolchain/java/` | OSGi API jars (Apache License 2.0) and OW2 ASM (BSD 3-Clause), unchanged; see its README |
| `hook/coverart/stb_image.h` | stb_image by Sean Barrett (public domain / MIT) |
| `wireless/daemon/vendor/` | `iap2-link` and `iap2-csm` from LIVI by f-io (GPL-3.0-or-later), unchanged; see [provenance](docs/wireless-carplay/IAP2_VENDOR_PROVENANCE.md) |

Audi, MMI, Virtual Cockpit, Apple, CarPlay, iPhone and other names are trademarks of their
owners. This project is not affiliated with or endorsed by any of them.

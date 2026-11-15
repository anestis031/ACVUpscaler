# Third-party software

ACVUpscaler itself (the add-on in `src/` and the setup tool in `setup-app/`) is under the MIT licence, see
`LICENSE`. The release zip also contains unmodified binaries of other projects, each under its own licence.
None of them is part of this repository; `tools/package.ps1` takes them from the official sources below and
checks their SHA-256 before packing.

| Component | Version | What is shipped | Licence | Source |
|---|---|---|---|---|
| ReShade (build with full add-on support) | 6.8.0 | `payload/ReShade64.dll` | BSD 3-Clause (`licenses/ReShade-LICENSE.md`) | https://reshade.me, https://github.com/crosire/reshade/tree/v6.8.0 |
| OptiScaler | 0.9.4 | `payload/OptiScaler.dll`, `payload/OptiScaler.ini` | GPL-3.0 (`licenses/OptiScaler-LICENSE.txt`) | https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4 |
| AMD FidelityFX SDK | 2.4.0 | `payload/amd_fidelityfx_loader_dx12.dll`, `payload/amd_fidelityfx_upscaler_dx12.dll` (AMD's signed binaries) | AMD's SDK licence: redistribution in binary form with its notice, no reverse engineering (`licenses/FidelityFX-LICENSE.md`) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0 |

## OptiScaler source code (GPL-3.0)
`OptiScaler.dll` is the unmodified binary of the official v0.9.4 release. Its complete corresponding source is
the tag `v0.9.4` of https://github.com/optiscaler/OptiScaler
(https://github.com/optiscaler/OptiScaler/archive/refs/tags/v0.9.4.zip). ACVUpscaler does not link against
OptiScaler and is not derived from it: OptiScaler is a separate program that the setup tool can place next to
the game.

## Not included: NVIDIA DLSS
`nvngx_dlss.dll` is **not** part of this repository or of the release zip. NVIDIA's DLSS SDK licence does not
allow it to be redistributed on its own. When you choose DLSS, the setup tool downloads version 310.9.16
directly from NVIDIA's repository (https://github.com/NVIDIA/DLSS), checks its SHA-256 and places it in the
game folder. By using it you accept NVIDIA's licence: https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt

## Sharpening pass
The add-on's own sharpening pass (`src/fsr.cpp`) follows RCAS from AMD FidelityFX Super Resolution 1
(`ffx_fsr1.h`), MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.

## Headers used at build time
The add-on is built against the ReShade add-on API headers (BSD 3-Clause, some files MIT), Dear ImGui headers
(MIT) through ReShade, and the FidelityFX API headers (the MIT part of AMD's SDK licence). The setup tool uses the `windows-sys` crate
(MIT or Apache-2.0).

Assassin's Creed Valhalla is a trademark of Ubisoft. This project is not affiliated with or endorsed by
Ubisoft, AMD, NVIDIA, Intel, the ReShade author or the OptiScaler team. No game files are distributed.

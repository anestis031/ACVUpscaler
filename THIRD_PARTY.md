# Third-party software

ACVUpscaler's own code (the add-on in `src/` and the setup tool in `setup-app/`) is under the MIT licence, see
`LICENSE`. The release zip also contains unmodified binaries of other projects, each under its own licence, and
the add-on file contains two libraries of NVIDIA (below). None of them is part of this repository;
`tools/package.ps1` and `tools/fetch_deps.cmd` take them from the official sources.

| Component | Version | What is shipped | Licence | Source |
|---|---|---|---|---|
| ReShade (build with full add-on support) | 6.8.0 | `payload/ReShade64.dll` | BSD 3-Clause (`licenses/ReShade-LICENSE.md`) | https://reshade.me, https://github.com/crosire/reshade/tree/v6.8.0 |
| AMD FidelityFX SDK | 2.4.0 | `payload/amd_fidelityfx_loader_dx12.dll`, `payload/amd_fidelityfx_upscaler_dx12.dll` (AMD's signed binaries) | AMD's SDK licence: redistribution in binary form with its notice, no reverse engineering (`licenses/FidelityFX-LICENSE.md`) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0 |

Releases up to 1.0.0-beta.2 also shipped OptiScaler 0.9.4 (GPL-3.0, https://github.com/optiscaler/OptiScaler,
tag `v0.9.4`). It is no longer in the package.

## Not included: NVIDIA DLSS
`nvngx_dlss.dll` is **not** part of this repository or of the release zip. NVIDIA's DLSS SDK licence does not
allow it to be redistributed on its own. When you choose DLSS, the setup tool downloads version 310.9.16
directly from NVIDIA's repository (https://github.com/NVIDIA/DLSS), checks its SHA-256 and places it next to
the add-on. By using it you accept NVIDIA's licence: https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt

## Sharpening pass
The add-on's own sharpening pass (`src/fsr.cpp`) follows RCAS from AMD FidelityFX Super Resolution 1
(`ffx_fsr1.h`), MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.

## NVIDIA NGX SDK (inside the add-on)
The add-on calls DLSS through NVIDIA's NGX SDK. `ACVUpscaler.addon64` contains NVIDIA's static library
`nvsdk_ngx_s.lib` of the DLSS SDK 310.9.16, under NVIDIA's DLSS SDK licence
(`licenses/NVIDIA-DLSS-SDK-LICENSE.txt`), from https://github.com/NVIDIA/DLSS. That part of the add-on file is
not under the MIT licence of this project. This project uses NVIDIA DLSS; DLSS and Reflex are technologies and
trademarks of NVIDIA Corporation.

## NVIDIA NVAPI (inside the add-on)
The add-on's Reflex low latency mode calls the NVIDIA driver through NVAPI. `ACVUpscaler.addon64` contains
NVIDIA's `nvapi64.lib` (release R620), MIT licence, Copyright (c) NVIDIA CORPORATION & AFFILIATES
(`licenses/NVAPI-LICENSE.txt`), from https://github.com/NVIDIA/nvapi

## Headers used at build time
The add-on is built against the ReShade add-on API headers (BSD 3-Clause, some files MIT), Dear ImGui headers
(MIT) through ReShade, and the FidelityFX API headers (the MIT part of AMD's SDK licence). The setup tool uses the `windows-sys` crate
(MIT or Apache-2.0).

Assassin's Creed Valhalla is a trademark of Ubisoft. This project is not affiliated with or endorsed by
Ubisoft, AMD, NVIDIA, Intel, the ReShade author or the OptiScaler team. No game files are distributed.

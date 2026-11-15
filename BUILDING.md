# Building from source

## What you need

- Windows 10 or 11, 64-bit
- Visual Studio 2022 or the Build Tools, with the "Desktop development with C++" workload and a Windows SDK
- CMake and Ninja
- Git
- Rust (stable, `x86_64-pc-windows-msvc`) for the setup tool: https://rustup.rs

`tools\build.cmd` calls `vcvars64.bat` of the Visual Studio 2022 **Build Tools** and expects CMake and Ninja
where winget installs them. If yours are elsewhere, adjust the two paths at the top of that file.

## Steps

```bat
tools\fetch_deps.cmd
tools\build.cmd
cd setup-app
cargo test
cargo build --release
```

- `fetch_deps.cmd` clones the ReShade headers (v6.8.0) and the parts of AMD's FidelityFX SDK (v2.4.0) that are
  needed into `external\`. The add-on must be built against the ReShade API version of the ReShade that
  loads it.
- `build.cmd` builds `build\bin\ACVUpscaler.addon64`, runs the checksum test and copies AMD's two signed DLLs
  next to it.
- The setup tool ends up in `setup-app\target\release\ACVUpscalerSetup.exe`.

## The release zip

`tools\package.ps1` builds `dist\ACVUpscaler-<version>.zip`. Besides the two builds above it needs the
official downloads of the third-party programs in `external\` and refuses to pack anything whose SHA-256
differs from the values in the script:

- `external\reshade_setup\ReShade_Setup_6.8.0_Addon.exe` from https://reshade.me (the build with full add-on
  support)
- `external\OptiScaler_0.9.4\Optiscaler_0.9.4-final.20260718._MM.7z` from
  https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4

It uses the `tar.exe` of Windows 11 to read both.

## Layout of the source

- `src\`: the ReShade add-on (C++20). `pipelines.*` recognises the game's passes by shader checksum,
  `descriptors.*` and `frame.*` track what each pass has bound, `replace.*` skips the game's TAA and FSR 1 and
  puts the upscaler in their place, `fsr.*` talks to the FidelityFX API, `overlay.*` is the panel. `snapshot.*`,
  `record.*` and `autotest.*` are the developer tools behind `DevMode=1`.
- `setup-app\`: the setup tool (Rust, plain Win32 through `windows-sys`). `core.rs` holds the rules for what may
  be added to or removed from the game folder, with tests.
- `tests\`: the test of the shader checksum code.

@echo off
rem Fetches the third-party sources the add-on is built against into <project>\external, at the versions
rem this project was tested with:
rem   ReShade v6.8.0 (add-on API headers, Dear ImGui headers)   https://github.com/crosire/reshade
rem   AMD FidelityFX SDK v2.4.0 (API headers, signed DLLs)      https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
rem   NVIDIA DLSS SDK 310.9.16 (NGX headers, static library)    https://github.com/NVIDIA/DLSS
rem     Optional: without it the add-on is built without its own DLSS path. Headers, library, documents and the
rem     licence only; nvngx_dlss.dll itself is not fetched here.
rem   NVIDIA NVAPI R620 (headers, nvapi64.lib; MIT)             https://github.com/NVIDIA/nvapi
rem     Optional: without it the add-on is built without Reflex.
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "%ROOT%\external" mkdir "%ROOT%\external"

if not exist "%ROOT%\external\reshade\include\reshade.hpp" (
    git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade "%ROOT%\external\reshade" || goto :fail
    git -C "%ROOT%\external\reshade" submodule update --init --depth 1 deps/imgui || goto :fail
)

if not exist "%ROOT%\external\FidelityFX-SDK\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll" (
    git clone --depth 1 --branch v2.4.0 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK "%ROOT%\external\FidelityFX-SDK" || goto :fail
    git -C "%ROOT%\external\FidelityFX-SDK" sparse-checkout set docs Kits/FidelityFX/api/include Kits/FidelityFX/upscalers/include Kits/FidelityFX/signedbin || goto :fail
)

if not exist "%ROOT%\external\DLSS\include\nvsdk_ngx.h" (
    git clone --filter=blob:none --sparse --no-checkout https://github.com/NVIDIA/DLSS "%ROOT%\external\DLSS" || goto :fail
    git -C "%ROOT%\external\DLSS" sparse-checkout set include lib/Windows_x86_64/x64 doc || goto :fail
    git -C "%ROOT%\external\DLSS" checkout --quiet 9981c6350dc58c0ce21f024fd65e31e851c31689 || goto :fail
)

if not exist "%ROOT%\external\nvapi\nvapi.h" (
    git clone --filter=blob:none --sparse --no-checkout https://github.com/NVIDIA/nvapi "%ROOT%\external\nvapi" || goto :fail
    git -C "%ROOT%\external\nvapi" sparse-checkout set amd64 || goto :fail
    git -C "%ROOT%\external\nvapi" checkout --quiet 51e5aae4bc40c3ccd3fb7d4dfd94256e7bfc496e || goto :fail
)

echo Dependencies are in %ROOT%\external
exit /b 0

:fail
echo FETCH FAILED
exit /b 1

@echo off
rem Fetches the third-party sources the add-on is built against into <project>\external, at the versions
rem this project was tested with:
rem   ReShade v6.8.0 (add-on API headers, Dear ImGui headers)   https://github.com/crosire/reshade
rem   AMD FidelityFX SDK v2.4.0 (API headers, signed DLLs)      https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
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

echo Dependencies are in %ROOT%\external
exit /b 0

:fail
echo FETCH FAILED
exit /b 1

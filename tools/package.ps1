# Builds the release folder and zip:  dist\ACVUpscaler-<version>\  and  dist\ACVUpscaler-<version>.zip
#
# Third-party binaries are taken from the official downloads in external\ (never from a game folder) and
# their SHA-256 is checked against the values below before anything is packed:
#   external\reshade_setup\ReShade_Setup_6.8.0_Addon.exe   from https://reshade.me
#   external\FidelityFX-SDK\Kits\FidelityFX\signedbin\     from the FidelityFX SDK v2.4.0
# Needs tar.exe of Windows 11 (reads the ReShade setup), the MSVC build of the add-on and cargo.
param([string]$Version = "1.0.0-beta.3", [switch]$SkipBuild, [string]$Dist = "")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$tar = "$env:SystemRoot\System32\tar.exe"

function Assert-Hash([string]$Path, [string]$Expected) {
    if (-not (Test-Path $Path)) { throw "missing: $Path" }
    $actual = (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLower()
    if ($actual -ne $Expected) { throw "checksum of $Path is $actual, expected $Expected" }
}

if (-not $SkipBuild) {
    & cmd /c "`"$root\tools\build.cmd`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "the add-on did not build" }
    Push-Location "$root\setup-app"
    & cargo build --release
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "the setup tool did not build" }
    Pop-Location
}

if ($Dist -eq "") { $Dist = "$root\dist" }
$out = "$Dist\ACVUpscaler-$Version"
if (Get-Process -Name ACVUpscalerSetup -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dist\*" }) {
    throw "ACVUpscalerSetup.exe is running from $Dist. Close it first, or pack into another folder with -Dist."
}
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force "$out\payload", "$out\licenses" | Out-Null

# Ours
Copy-Item "$root\setup-app\target\release\ACVUpscalerSetup.exe" $out
Copy-Item "$root\build\bin\ACVUpscaler.addon64" "$out\payload"

# AMD FidelityFX SDK 2.4.0, signed binaries
$ffx = "$root\external\FidelityFX-SDK\Kits\FidelityFX\signedbin"
Assert-Hash "$ffx\amd_fidelityfx_loader_dx12.dll" "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"
Assert-Hash "$ffx\amd_fidelityfx_upscaler_dx12.dll" "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"
Copy-Item "$ffx\amd_fidelityfx_loader_dx12.dll", "$ffx\amd_fidelityfx_upscaler_dx12.dll" "$out\payload"

# ReShade 6.8.0 with full add-on support: the DLL is inside the official setup program
$reshadeSetup = "$root\external\reshade_setup\ReShade_Setup_6.8.0_Addon.exe"
Assert-Hash $reshadeSetup "afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445"
& $tar -xf $reshadeSetup -C "$out\payload" ReShade64.dll
Assert-Hash "$out\payload\ReShade64.dll" "0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7"

# Licences
Copy-Item "$root\LICENSE" "$out\licenses\ACVUpscaler-LICENSE.txt"
Copy-Item "$root\THIRD_PARTY.md" "$out\licenses\THIRD_PARTY.md"
Copy-Item "$root\external\reshade\LICENSE.md" "$out\licenses\ReShade-LICENSE.md"
Copy-Item "$root\external\FidelityFX-SDK\docs\license.md" "$out\licenses\FidelityFX-LICENSE.md"
# NVIDIA's NGX library is linked into the add-on when the DLSS SDK was there at build time
if (Test-Path "$root\external\DLSS\LICENSE.txt") { Copy-Item "$root\external\DLSS\LICENSE.txt" "$out\licenses\NVIDIA-DLSS-SDK-LICENSE.txt" }
# NVAPI is linked into the add-on when it was there at build time (Reflex)
if (Test-Path "$root\external\nvapi\License.txt") { Copy-Item "$root\external\nvapi\License.txt" "$out\licenses\NVAPI-LICENSE.txt" }

# Short text for people who open the zip without reading the repository
foreach ($doc in "README.md", "INSTALL.md", "UNINSTALL.md") {
    if (Test-Path "$root\$doc") { Copy-Item "$root\$doc" $out }
}

$zip = "$Dist\ACVUpscaler-$Version.zip"
Compress-Archive -Path $out -DestinationPath $zip
Get-ChildItem -Recurse -File $out | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($out.Length + 1) }
"zip: $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)

# Installing ACVUpscaler

## With the setup tool (recommended)

1. Extract the whole release zip to any folder **outside** the game folder. `ACVUpscalerSetup.exe` needs the
   `payload` folder next to it.
2. Close the game.
3. Run `ACVUpscalerSetup.exe`. It looks for the game in your Steam libraries (and in Ubisoft Connect); if it
   does not find it, press **Browse...** and pick the folder that contains `ACValhalla.exe`.
4. Choose:
   - **Upscaler**: DLSS (NVIDIA RTX only), FSR 3.1 (every card) or Off.
   - **DLSS model**: K is fast and the right choice for RTX 20 and 30 series cards; M is a little sharper and
     clearly slower on those cards.
   - **Steam overlay or OptiScaler menu** (DLSS only): the two cannot work at the same time.
   - **Render quality**: the FSR quality of the game (Ultra Quality, Quality, Balanced, Performance). With
     ACVUpscaler it only decides the resolution the game renders at. The tool writes it into the game's own
     settings file (`Documents\Assassin's Creed Valhalla\ACValhalla.ini`, keys `UpsampleType` and `FSRQuality`;
     a backup copy `ACValhalla.ini.acvupscaler-backup` is made the first time) and shows what you will get,
     for example "DLSS 310.9.16, model K. Balanced: rendered at 1512x848, shown at 2560x1440".
   - **Particle mask strength**: how strongly FSR 3.1 is told about rain and fire. 4 is the tested default.
   - **Sharper textures**: a texture LOD bias that matches the render resolution. Leave it on.
   - **Sharpening pass of the game on top**: the game sharpens the picture after its FSR 1 upscale (RCAS), and
     by default it still does so after FSR 3.1 or DLSS. Off gives a softer picture and saves that pass.
5. Press **Install / Apply**. The tool shows the list of files it will add, replace or remove and asks before
   it does anything. With DLSS it downloads `nvngx_dlss.dll` (about 59 MB) from NVIDIA's GitHub repository.
6. Start the game. FidelityFX Super Resolution is on in its graphics options with the quality you picked in
   the tool; the upscaling itself is done by FSR 3.1 or DLSS.
7. If you change the quality inside the game instead of in the tool, restart the game once afterwards (see
   "Texture sharpness" below). The tool states this under its summary.

To change a setting later, run the tool again, change it and press **Install / Apply**.

### Is it working?

In the game press **Home** to open ReShade and go to the **Add-ons** tab. The ACVUpscaler panel shows
`Status: active` and the resolutions, for example `1280x720 -> 2560x1440`. With DLSS and the OptiScaler menu
layout, **Insert** opens OptiScaler's menu, which names the upscaler in its first line.

### What the tool puts into the game folder

| Choice | Files |
|---|---|
| always | `dxgi.dll` (ReShade 6.8.0), `ReShade.ini`, folder `ACVUpscaler\` with the add-on, AMD's two FidelityFX DLLs, `ACVUpscaler.ini`, `install_manifest.txt` and the log |
| DLSS, Steam overlay | also `winmm.dll` (OptiScaler 0.9.4), `OptiScaler.ini`, `nvngx_dlss.dll` |
| DLSS, OptiScaler menu | `dxgi.dll` is OptiScaler and ReShade is `ReShade64.dll`; `OptiScaler.ini`, `nvngx_dlss.dll` |

The tool never replaces or deletes a file it did not put there. If a `dxgi.dll` or `winmm.dll` of another mod
is already in the game folder, it stops and tells you. An existing `ReShade.ini` is kept; only its
`AddonPath` is changed, and the old value comes back when you uninstall.

## By hand

Use this only if you cannot run the tool. All files are in the `payload` folder of the release zip.

FSR 3.1 only:
1. Copy `ReShade64.dll` into the game folder and rename it to `dxgi.dll`.
2. Create the folder `ACVUpscaler` in the game folder and copy `ACVUpscaler.addon64`,
   `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` into it.
3. Create `ReShade.ini` in the game folder with:
   ```ini
   [ADDON]
   AddonPath=.\ACVUpscaler
   ```
4. Create `ACVUpscaler\ACVUpscaler.ini` with:
   ```ini
   [ACVUpscaler]
   Mode=FSR31
   ```

DLSS with the Steam overlay, in addition:
1. Copy `OptiScaler.dll` into the game folder and rename it to `winmm.dll`.
2. Copy `OptiScaler.ini` into the game folder and set these values in it (each key exists once in its section):
   `Dx12Upscaler=dlss`, `EnableFfxInputs=true`, `EnableDlssInputs=false`, `EnableXeSSInputs=false`,
   `EnableFsr2Inputs=false`, `EnableFsr3Inputs=false`, `LoadReshade=false`, `TargetProcessName=ACValhalla.exe`,
   `CheckForUpdate=false`. For model K also `RenderPresetOverride=true` and `RenderPresetForAll=11` in the
   `[DLSS]` section.
3. Download `nvngx_dlss.dll` from https://github.com/NVIDIA/DLSS (folder `lib/Windows_x86_64/rel`) into the game
   folder.
4. Add `KeepContexts=1` to `ACVUpscaler\ACVUpscaler.ini`.

## Settings file of the add-on

`ACVUpscaler\ACVUpscaler.ini`, section `[ACVUpscaler]`. The setup tool writes all of them.

| Key | Values | Meaning |
|---|---|---|
| `Mode` | `FSR31`, `GameTAA` | On (FSR 3.1, or DLSS when OptiScaler is installed) or off |
| `ReactiveGain` | 0 to 64, default 4 | Strength of the particle mask given to FSR 3.1; 0 gives none |
| `LodBias` | 1 or 0 | Texture LOD bias from the render resolution |
| `GameSharpening` | 1 or 0 | 1: the game's sharpening pass runs on top of the upscaler. 0: it is skipped |
| `KeepContexts` | 0 or 1 | 1 with OptiScaler as `winmm.dll`: old upscaler instances are not destroyed |
| `DevMode` | 0 or 1 | Developer tools: keys F6 to F10, snapshots, detailed log |
| `RenderScale` | written by the add-on | Render scale of the last session, used for the LOD bias |

## Texture sharpness

At a lower render resolution the game would pick blurrier texture levels. The add-on corrects that with a
negative LOD bias. The game creates its samplers once at start, so the bias is taken from the render
resolution of the **previous** session: after changing the FSR quality in the game, play for a moment, then
restart the game once. The panel in the Add-ons tab tells you when a restart is due. When you set the quality
in the setup tool, the tool also tells the add-on the matching render scale, and no extra restart is needed.

## Troubleshooting

- **The game does not start or closes at the title screen.** Run the setup tool and press **Uninstall**, or
  switch to FSR 3.1 (which does not use OptiScaler) and try again. Then look at the logs below.
- **The panel says the shaders were not recognised.** Your game version differs from the tested one. The
  add-on does nothing in that case; the game runs with its own anti-aliasing.
- **No Steam overlay.** Choose "Keep the Steam overlay" in the setup tool.
- **Textures shimmer or look too sharp after changing the quality.** Restart the game once.
- **Logs:** `ACVUpscaler\acvupscaler.log`, `ReShade.log` and `OptiScaler.log` in the game folder.

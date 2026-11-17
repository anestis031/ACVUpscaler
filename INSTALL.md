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
   - **Render quality**: the FSR quality of the game (Ultra Quality, Quality, Balanced, Performance). With
     ACVUpscaler it only decides the resolution the game renders at. The tool writes it into the game's own
     settings file (`Documents\Assassin's Creed Valhalla\ACValhalla.ini`, keys `UpsampleType` and `FSRQuality`;
     a backup copy `ACValhalla.ini.acvupscaler-backup` is made the first time) and shows what you will get,
     for example "DLSS 310.9.16, model K. Balanced: rendered at 1512x848, shown at 2560x1440".
   - **Particle mask strength**: how strongly FSR 3.1 is told about rain and fire. 4 is the tested default.
     DLSS does not get the mask.
   - **Sharper textures**: a texture LOD bias that matches the render resolution. Leave it on.
   - **Sharpening pass of the game on top**: the game sharpens the picture after its FSR 1 upscale (RCAS), and
     by default it still does so after FSR 3.1 or DLSS. On top of DLSS that is more sharpening than the picture
     needs. Switch it off to set the strength yourself:
   - **Own sharpening instead**: 0 to 100. The add-on then runs the same kind of pass with your strength; 0 is
     no sharpening at all, 84 is what the game does.
   - **NVIDIA Reflex low latency**: off by default, NVIDIA cards only. The README explains what this limited
     form does.
   - **Characters in full detail at a distance**: raises a setting of the game that its menu does not show
     (`Character` in the game's settings file, from 2 to 4). Unticking it puts back the value of the backup copy.
     The game keeps the value and does a little more work per frame with it; a difference in the picture could
     not be shown in recordings.

   Reflex and the two sharpening settings can also be changed while playing: **Home** key, **Add-ons** tab.
   What you set there is kept, and the tool shows it the next time.
5. Press **Install / Apply**. The tool shows the list of files it will add, replace or remove and asks before
   it does anything. With DLSS it downloads `nvngx_dlss.dll` (about 59 MB) from NVIDIA's GitHub repository.
6. Start the game. FidelityFX Super Resolution is on in its graphics options with the quality you picked in
   the tool; the upscaling itself is done by FSR 3.1 or DLSS.
7. If you change the quality inside the game instead of in the tool, restart the game once afterwards (see
   "Texture sharpness" below). The tool states this under its summary.

To change a setting later, run the tool again, change it and press **Install / Apply**.

### Coming from 1.0.0-beta.2 or older

Run the new tool and press **Install / Apply**. If you had DLSS, which those versions ran through OptiScaler,
the tool moves `nvngx_dlss.dll` next to the add-on without downloading it again and removes OptiScaler's files
(`winmm.dll` or `ReShade64.dll`, `OptiScaler.ini`). DLSS then uses the same files as FSR 3.1, and the choice
between the Steam overlay and OptiScaler's menu is gone.

### Is it working?

In the game press **Home** to open ReShade and go to the **Add-ons** tab. The ACVUpscaler panel shows
`Status: active` and, in the line `Upscaler:`, what runs and at which resolutions, for example
`DLSS preset K, 1512x848 -> 2560x1440`. If DLSS could not start, the same line names FSR 3.1 and the reason.

### What the tool puts into the game folder

| Choice | Files |
|---|---|
| always | `dxgi.dll` (ReShade 6.8.0), `ReShade.ini`, folder `ACVUpscaler\` with the add-on, AMD's two FidelityFX DLLs, `ACVUpscaler.ini`, `install_manifest.txt` and the log |
| DLSS | also `ACVUpscaler\nvngx_dlss.dll` (downloaded from NVIDIA) |

The tool never replaces or deletes a file it did not put there. If a `dxgi.dll` of another mod is already in
the game folder, it stops and tells you. An existing `ReShade.ini` is kept; only its `AddonPath` is changed,
and the old value comes back when you uninstall.

## By hand

Use this only if you cannot run the tool. All files are in the `payload` folder of the release zip.

FSR 3.1:
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

DLSS, in addition:
1. Download `nvngx_dlss.dll`, version 310.9.16, from https://github.com/NVIDIA/DLSS (folder
   `lib/Windows_x86_64/rel`) into the folder `ACVUpscaler`.
2. Add `Upscaler=DLSS` to `ACVUpscaler\ACVUpscaler.ini`.

## Settings file of the add-on

`ACVUpscaler\ACVUpscaler.ini`, section `[ACVUpscaler]`. The setup tool writes all of them.

| Key | Values | Meaning |
|---|---|---|
| `Mode` | `FSR31`, `GameTAA` | The replacement of the game's TAA and FSR 1: on or off |
| `Upscaler` | `FSR31`, `DLSS` | Which upscaler runs when it is on. With `DLSS`, FSR 3.1 takes over where DLSS cannot start |
| `DlssPreset` | `K`, `M`, `Default` | The DLSS model |
| `ReactiveGain` | 0 to 64, default 4 | Strength of the particle mask given to FSR 3.1; 0 gives none |
| `LodBias` | 1 or 0 | Texture LOD bias from the render resolution |
| `GameSharpening` | 1 or 0 | 1: the game's sharpening pass runs on top of the upscaler. 0: it is skipped |
| `Sharpness` | 0 to 100, default 0 | Strength of the add-on's own sharpening pass. Used with `GameSharpening=0`, and at native resolution |
| `Reflex` | 0 or 1, default 0 | NVIDIA Reflex low latency, limited form |
| `KeepContexts` | 0 or 1 | Only for OptiScaler placed by hand as `winmm.dll` (below). The tool writes 0 |
| `DevMode` | 0 or 1 | Developer tools: keys F6 to F10, snapshots, detailed log, log files of DLSS |
| `RenderScale` | written by the add-on | Render scale of the last session, used for the LOD bias |

## OptiScaler

OptiScaler is no longer part of the package, and DLSS does not need it. It can still be placed by hand for its
other upscalers: with `Upscaler=FSR31` the add-on calls AMD's FSR 3.1 API, which OptiScaler takes over. As
`winmm.dll` behind ReShade it needs `KeepContexts=1`, which the setup tool resets to 0 on every Apply. This
combination was last tested with DLSS in the game's benchmark; it is not looked after.

## Texture sharpness

At a lower render resolution the game would pick blurrier texture levels. The add-on corrects that with a
negative LOD bias. The game creates its samplers once at start, so the bias is taken from the render
resolution of the **previous** session: after changing the FSR quality in the game, play for a moment, then
restart the game once. The panel in the Add-ons tab tells you when a restart is due. When you set the quality
in the setup tool, the tool also tells the add-on the matching render scale, and no extra restart is needed.

## Troubleshooting

- **The game does not start or closes at the title screen.** Run the setup tool and press **Uninstall**, or
  switch to FSR 3.1 and try again. Then look at the logs below.
- **The panel says the shaders were not recognised.** Your game version differs from the tested one. The
  add-on does nothing in that case; the game runs with its own anti-aliasing.
- **The panel names FSR 3.1 although DLSS was chosen.** The same line says why: no NVIDIA RTX card, a driver
  that is too old, or `nvngx_dlss.dll` missing from the `ACVUpscaler` folder (run the tool again).
- **Reflex says "not available".** It needs an NVIDIA card and driver.
- **Textures shimmer or look too sharp after changing the quality.** Restart the game once.
- **Logs:** `ACVUpscaler\acvupscaler.log` and `ReShade.log` in the game folder.

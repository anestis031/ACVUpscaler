# ACVUpscaler

A temporal upscaler for **Assassin's Creed Valhalla** (PC, DirectX 12). It replaces the game's own
anti-aliasing and its FSR 1 upscale with **FSR 3.1**, or with **DLSS** on NVIDIA RTX cards.

Valhalla only ships FSR 1, a spatial upscaler: it never sees more than the current frame, which is why the
game looks soft and unstable below native resolution. The game does produce everything a temporal upscaler
needs (motion vectors, depth, a jittered camera) for its own TAA. ACVUpscaler takes those and hands them to a
modern upscaler instead.

*Ελληνικά: [README.el.md](README.el.md)*

> **Beta.** Tested on one PC: the Steam version of the game as of November 2026, an RTX 3060, 2560x1440,
> Windows 11. Other game versions, resolutions and cards are untested. Single-player only, at your own risk.

## What you get

### Speed

The game's own benchmark on one PC (RTX 3060, i7-11700K, 2560x1440), one run per setting; runs of the same
setting differ by about 1 fps. Average fps / lowest 1 %:

| | Native 2560x1440 | Ultra Quality 1976x1112 | Quality 1712x960 | Balanced 1512x848 | Performance 1280x720 |
|---|---|---|---|---|---|
| Game alone (TAA, FSR 1) | 64 / 47 | 83 / 62 | 93 / 69 | 100 / 70 | 111 / 76 |
| FSR 3.1 | 56 / 44 | 73 / 55 | 81 / 60 | 90 / 65 | 98 / 66 |
| DLSS 310.9.16, model K | 57 / 42 | 70 / 54 | 78 / 57 | 83 / 61 | 90 / 63 |
| DLSS 310.9.16, model M | not measured | 47 / 38 | 55 / 44 | 61 / 48 | 72 / 53 |

- At the same render resolution a frame takes about 1.7 ms longer with FSR 3.1 and 2.2 ms longer with DLSS K
  than with the game's FSR 1 (Ultra Quality). DLSS K at Balanced runs as fast as FSR 1 at Ultra Quality.
- Model M is heavy on this card: about 9 ms per frame more than FSR 1 at Ultra Quality. Model K is the one to
  use on RTX 20 and 30 series cards.
- Native: with FidelityFX Super Resolution off in the game, the upscaler replaces only the anti-aliasing.

### Picture

Every setting was recorded in the benchmark and compared frame by frame with a recording at 200 % resolution
scale (method and all numbers: [docs/TECHNICAL.md](docs/TECHNICAL.md)). What the scores say, and what they
cannot say:

- Within every upscaler the picture gets worse with each step down in render resolution, as expected.
- At native resolution FSR 3.1, DLSS K and the game's TAA score the same.
- At Ultra Quality single frames of FSR 1, FSR 3.1 and DLSS K score the same. Below that FSR 1 falls off
  faster: at Quality it scores like FSR 3.1 and DLSS K at Performance, and at Performance far below them.
- With DLSS, the picture is closer to the reference with the game's sharpening pass switched off (an option of
  the setup tool); with it the picture is sharper than the reference.
- **Not measured: stability in motion** (shimmer, crawling edges, ghosting). The scores look at single frames,
  and stability is where a temporal upscaler differs most from FSR 1. Judge that with your own eyes.

**Comparison video** (4K, 60 fps; watch it at 2160p): all settings side by side in the benchmark, in groups of
similar frame rate: https://www.youtube.com/watch?v=j24VcwrQKlU

## Requirements

- Assassin's Creed Valhalla on PC (the Steam version was tested; the setup tool also looks for a Ubisoft
  Connect installation, which is untested)
- Windows 10 or 11, 64-bit
- For FSR 3.1: any DirectX 12 graphics card
- For DLSS: an NVIDIA RTX card with a current driver

## Install

1. Download `ACVUpscaler-<version>.zip` from the Releases page and extract all of it.
2. Close the game. Run `ACVUpscalerSetup.exe`.
3. Choose the upscaler and press **Install / Apply**. The tool lists every file it is going to add before it
   does anything.
4. Start the game. In the graphics options turn **FidelityFX Super Resolution** on and pick a quality: that
   setting now decides the render resolution, the upscaling itself is done by FSR 3.1 or DLSS.

Details, the manual way and troubleshooting: [INSTALL.md](INSTALL.md). Removing it: [UNINSTALL.md](UNINSTALL.md).

## How it works

- A [ReShade](https://reshade.me) add-on recognises the game's TAA and FSR 1 passes by the checksums of their
  shaders, skips them, and calls AMD's FidelityFX API (FSR 3.1) with the game's colour, motion vectors,
  depth and jitter. The result is written where the game expects its own upscaled image.
- For DLSS, [OptiScaler](https://github.com/optiscaler/OptiScaler) sits in the same process, intercepts those
  FSR 3.1 calls and runs DLSS instead.
- Nothing in the game's files or its executable is modified. Everything is loaded through the usual `dxgi.dll`
  (and for DLSS `winmm.dll`) files next to the game, and removed again by deleting them.

## Known limits

- **One game version.** If an update changes the shaders, the add-on notices that it does not recognise
  them, does nothing, and says so in its panel (ReShade overlay, Home key, Add-ons tab).
- **Steam overlay or OptiScaler menu, not both** (DLSS only). The setup tool lets you choose.
- **Texture sharpness follows one start behind.** The game creates its texture samplers once at start, so
  after changing the FSR quality in the game, restart it once.
- **Effects after the upscale point** (bloom, depth of field, motion blur) still run at the render resolution.
- **Rain and other particles** have no motion vectors. FSR 3.1 gets a mask for them from the game; with DLSS,
  OptiScaler does not pass that mask on by default.
- With the Steam overlay layout the add-on keeps old DLSS instances alive instead of destroying them (a
  workaround for a crash); this costs some video memory after each change of resolution.
- Frame generation is not included.

## Safety

- **Single-player only.** ReShade's add-on build and OptiScaler are not meant for online games.
- The game runs through Ubisoft Connect. This project changes no game file and no executable, but it does
  load third-party DLLs into the game process. Whether that is acceptable for your account is your decision.
- The DLSS file is not distributed here. The setup tool downloads it from NVIDIA's own repository and checks
  its checksum; by using it you accept NVIDIA's licence.

## Building from source

See [BUILDING.md](BUILDING.md).

## Licence and credits

ACVUpscaler is under the MIT licence ([LICENSE](LICENSE)). It would not exist without ReShade (crosire),
OptiScaler, AMD's FidelityFX SDK and NVIDIA's DLSS; what is redistributed and under which terms is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

Assassin's Creed Valhalla is a trademark of Ubisoft. This project is not affiliated with or endorsed by
Ubisoft, AMD, NVIDIA or any of the projects above.

# Technical notes

What the add-on relies on in the game's frame. All of it was found by frame captures and verified with dumps
made from inside the add-on on the Steam version of November 2026. Another game version may differ.

## Passes

The passes are recognised by the checksum stored in the header of their DXBC shaders (`src/dxbc.hpp`,
`src/pipelines.cpp`), never by position in the frame.

- The game's TAA runs **after tonemapping**, on 8-bit colour, as the last scene pass before the UI. It is three
  draws on one command list (full reprojection, camera-only reprojection, de-dither tiles).
- With the game's FSR 1 on, two compute dispatches follow on the same command list: EASU (upscale to the
  display size) and RCAS (sharpening). Everything before them runs at the render resolution.

## Inputs of the TAA draws

Descriptor table of root parameter 2:

| Slot | Content |
|---|---|
| +0 | constant buffer of the TAA; row 3 `.xy` is the jitter in render pixels |
| +3 (t0) | current colour, `R8G8B8A8_UNORM`, complete frame |
| +4 (t1) | velocity, `R8G8B8A8_UNORM`, packed |
| +5 (t2) | view-space distance, `R32_FLOAT` |
| +6 (t3) | `R8_UNORM` mask of effects drawn without motion vectors (rain, fire) |
| +7, +8 | colour history and mask history of the game's TAA |
| +9 (t6) | de-dithered colour, only valid inside the de-dither tiles |

Root parameter 1, slot +0 holds the view constants: rows 26 to 29 are the projection matrix.
Render targets: RT0 is the TAA result, RT1 the new history, RT2 the new mask history.
Sampled textures rest in the state `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`.

## Motion vectors

12 bits per axis in RGB: `qx = R * 16 + (G >> 4)`, `qy = (G & 15) * 256 + B`, `e = q * 2 / 4094 - 1`,
`m = sign(e) * 0.2 * e^4`. `m` is `uvCurrent - uvPrevious`, so the upscaler gets `-m` with a motion vector
scale of (width, height). RGB code 0 means "no vector" (sky with a still camera). The vectors contain no jitter.

## Depth

t2 is the distance from the camera, not the depth buffer. With projection row 2 = (0, 0, a, b), the reversed-Z
depth the upscaler wants is `b / distance - a`. Near plane = `b` (0.1), far plane = `b / a + b`. The vertical
field of view follows from projection `[1][1] = cot(fov / 2)`.

## Jitter

Eight phases, in render pixels, with the convention FSR documents: it is passed on unchanged. Flipping either
axis made a still image measurably softer.

## What the add-on does with it (`src/replace.cpp`, `src/fsr.cpp`)

- Game FSR 1 on: the three TAA draws are skipped and t0 is copied into RT0. At the EASU dispatch a small
  compute pass decodes velocity, distance and mask, then `ffxDispatch` upscales t0 to the display size straight
  into EASU's output texture. EASU is skipped, the game's RCAS still runs. With `GameSharpening=0` the same
  happens at the RCAS dispatch, into RCAS's output texture, and both EASU and RCAS are skipped.
- Game FSR 1 off: the upscaler runs at the TAA draws in native anti-aliasing mode and writes RT0 and RT1.
- Colour is tonemapped 8-bit, so the context has the non-linear colour space flag and the dispatch the sRGB
  flag, with pre-exposure 1 and no auto exposure. Depth is inverted.
- The reactive mask is `min(t3 * gain, 0.9)`.
- Texture LOD bias: the game creates all samplers once at start with bias 0. In ReShade's `init_sampler` event
  the add-on writes the anisotropic heap samplers again with `log2(render width / display width)` added.
  (`create_sampler` cannot be used: it also fires for static samplers, and changing those makes ReShade rebuild
  root signatures, after which pipelines fail to create.)
- `ffxDestroyContext` is called from `destroy_effect_runtime` after the queue is idle, and for replaced contexts
  eight presents after the replacement. Called from `destroy_device` it never returned.

## DLSS through OptiScaler

OptiScaler hooks library loading, recognises AMD's FidelityFX DLLs by file name wherever they are loaded from,
and detours the `ffx*` functions. The add-on's calls are therefore redirected without any change on its side.
Two layouts work:

- OptiScaler as `dxgi.dll`, loading ReShade as `ReShade64.dll`: OptiScaler wraps the swap chain, its menu works,
  the Steam overlay does not show.
- OptiScaler as `winmm.dll` behind ReShade as `dxgi.dll`: no swap chain wrapper, the Steam overlay works,
  OptiScaler's menu cannot draw. In this layout the game ended right after OptiScaler released the DLSS features
  of replaced contexts, so with `KeepContexts=1` the add-on never calls `ffxDestroyContext`.

`NvngxDlssPath` in `OptiScaler.ini` must be the path of the DLL file, not of its folder; the setup tool avoids it
by placing the DLL next to OptiScaler.

## Measurements (November 2026, RTX 3060, 2560x1440, the game's benchmark)

Speed: one benchmark run per setting without any recording; the numbers are in the README.

Picture: every setting was recorded once (desktop capture, 60 fps) and compared with a recording at 200 %
resolution scale with the game's FSR off. 160 sample frames per recording, aligned in time, 64 pixel tiles
matched with motion compensation, the worst 20 % of the tiles dropped. **Detail** is the correlation of the
fine detail (pixel minus its 5x5 mean) with the reference; **sharpness** is relative to the reference (1 = as
sharp); VMAF and SSIM are computed on the matched tiles.

| Setting | Avg fps | 1 % low | Detail | VMAF | SSIM | Sharpness |
|---|---|---|---|---|---|---|
| Second recording of the reference (ceiling) | 24 | | 0.656 | 83.2 | 0.837 | 0.98 |
| FSR 3.1 native | 56.3 | 43.7 | 0.603 | 77.6 | 0.808 | 0.83 |
| Game TAA native | 64.2 | 47.1 | 0.598 | 78.1 | 0.813 | 0.79 |
| DLSS K native | 57.3 | 42.4 | 0.585 | 76.9 | 0.801 | 0.80 |
| DLSS K Ultra Quality, `GameSharpening=0` | 69.4 | 51.1 | 0.585 | 76.4 | 0.804 | 0.78 |
| DLSS M Ultra Quality | 47.0 | 38.4 | 0.572 | 77.6 | 0.744 | 1.41 |
| FSR 3.1 Ultra Quality | 73.1 | 54.9 | 0.561 | 76.7 | 0.773 | 1.14 |
| DLSS M Balanced | 61.4 | 48.2 | 0.560 | 76.3 | 0.751 | 1.28 |
| DLSS M Quality | 55.0 | 43.6 | 0.559 | 77.0 | 0.745 | 1.33 |
| DLSS K Ultra Quality | 70.4 | 53.5 | 0.553 | 76.5 | 0.743 | 1.37 |
| Game FSR 1 Ultra Quality | 83.3 | 62.0 | 0.552 | 76.5 | 0.783 | 0.89 |
| DLSS M Performance | 71.6 | 53.0 | 0.538 | 75.3 | 0.745 | 1.24 |
| FSR 3.1 Quality | 81.3 | 60.0 | 0.535 | 75.9 | 0.770 | 1.05 |
| DLSS K Quality | 77.8 | 57.1 | 0.525 | 76.2 | 0.734 | 1.37 |
| DLSS K Performance | 89.5 | 63.3 | 0.502 | 74.4 | 0.730 | 1.28 |
| FSR 3.1 Performance | 97.6 | 66.4 | 0.502 | 74.5 | 0.773 | 0.89 |
| Game FSR 1 Quality | 92.7 | 69.0 | 0.502 | 74.7 | 0.777 | 0.75 |
| Game FSR 1 Performance | 110.8 | 75.8 | 0.406 | 73.1 | 0.766 | 0.53 |

How far to trust it:

- Two runs of the benchmark never show the same picture (people, smoke, leaves, water), so nothing can score
  above the ceiling. A setting recorded twice differed by 0.012 in detail and 0.9 in VMAF: differences below
  about 0.015 and 1 mean nothing. One recording per setting otherwise; a single recording can be off by more.
- The three Balanced settings of FSR 1, FSR 3.1 and DLSS K are missing: the benchmark runs in two variants with
  different camera paths, and their recordings never got the variant of the reference.
- **Stability in motion is not measured**: every sample is a single frame.
- The reference is the game's own TAA at 200 %, which favours pictures that look like the game's TAA.
  Sharpening lowers both detail and SSIM, and the settings arrive with very different sharpness.
- The benchmark is a slow flight in clear weather: no fast camera turns, no rain, no fog.

## Developer tools (`DevMode=1`)

F9 logs one frame, F8 dumps the TAA inputs and the upscaler's textures, F10 records bursts of 24 consecutive
frames with their jitter and projection, F6 runs the upscaler with four strengths of the reactive mask, F7
switches the replacement on and off. Recorded bursts can be fed through the upscaler again offline, which is how
the models were compared.

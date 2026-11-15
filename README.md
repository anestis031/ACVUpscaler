# ACVUpscaler

Replaces the anti-aliasing and the FSR 1 upscale of **Assassin's Creed Valhalla** (PC, DirectX 12) with
**FSR 3.1**, or with **DLSS** on NVIDIA RTX cards.

> **Beta.** Tested on one PC: Steam version of the game as of November 2026, RTX 3060, 2560x1440, Windows 11.
> Single-player only, at your own risk.

Comparison video (4K, watch at 2160p): https://www.youtube.com/watch?v=j24VcwrQKlU

## Install

1. Download `ACVUpscaler-<version>.zip` from the Releases page and extract it outside the game folder.
2. Close the game and run `ACVUpscalerSetup.exe`.
3. Choose the upscaler and the render quality, press **Install / Apply**. The tool lists every file before it
   adds it.
4. Start the game. To check: **Home** key, **Add-ons** tab, `Status: active`.

To remove it, run the tool again and press **Uninstall**. Details: [INSTALL.md](INSTALL.md),
[UNINSTALL.md](UNINSTALL.md).

## Method: how FSR 3.1 and DLSS got into the game

1. **The idea.** Valhalla ships only FSR 1, which sees one frame at a time. For its own TAA the game already
   produces what a temporal upscaler needs: motion vectors, depth and a jittered camera. The task was to find
   them and hand them to a modern upscaler.
2. **Finding the inputs.** Frame captures showed the TAA pass, the two FSR 1 passes, and the textures and
   constants they read.
3. **Decoding them.** The motion vectors are packed into an 8-bit texture, the "depth" is distance from the
   camera, the jitter sits in the TAA's constants. Each one was decoded and then verified with dumps made from
   inside the running game, never assumed.
4. **Proof at native resolution.** FSR 3.1 first replaced only the game's TAA, with no upscaling. A stable,
   sharp picture there proved the inputs right.
5. **Upscaling.** With FSR 1 on, the game renders at a lower resolution and upscales at the end of the frame.
   The add-on recognises those passes by the checksums of their shaders, skips them and runs FSR 3.1 in their
   place, writing into the texture the game expects.
6. **DLSS.** The add-on only speaks AMD's FSR 3.1 API. [OptiScaler](https://github.com/optiscaler/OptiScaler)
   intercepts those calls and runs DLSS with the same inputs, so one integration gives both.
7. **Measuring.** Every setting ran through the game's benchmark for speed, was recorded, and was compared
   frame by frame with a recording at 200 % resolution scale.

No game file and no executable is modified: everything loads through [ReShade](https://reshade.me) as an
add-on. The details are in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Results

The game's benchmark, RTX 3060, i7-11700K, 2560x1440. Average fps / lowest 1 %:

| | Native | Ultra Quality | Quality | Balanced | Performance |
|---|---|---|---|---|---|
| Game alone (TAA, FSR 1) | 64 / 47 | 83 / 62 | 93 / 69 | 100 / 70 | 111 / 76 |
| FSR 3.1 | 56 / 44 | 73 / 55 | 81 / 60 | 90 / 65 | 98 / 66 |
| DLSS, model K | 57 / 42 | 70 / 54 | 78 / 57 | 83 / 61 | 90 / 63 |
| DLSS, model M | not measured | 47 / 38 | 55 / 44 | 61 / 48 | 72 / 53 |

- FSR 3.1 costs about 1.7 ms per frame more than FSR 1, DLSS K about 2.2 ms. Model M is heavy on this card:
  use K on RTX 20 and 30 series.
- Single frames at Ultra Quality score the same for FSR 1, FSR 3.1 and DLSS K. Below that FSR 1 loses detail
  faster.
- **Stability in motion was not measured**, and that is where temporal upscalers differ most from FSR 1. The
  video is there to judge it.

## Limits

- One game version. After a game update that changes the shaders the add-on does nothing and says so.
- With DLSS: Steam overlay or OptiScaler menu, not both.
- Bloom, depth of field and motion blur still run at the render resolution.
- No frame generation.

## Safety and licence

- Single-player only. The mod loads third-party DLLs (ReShade, OptiScaler) into a game that runs through
  Ubisoft Connect; whether that is acceptable for your account is your decision.
- The DLSS file is not distributed here: the setup tool downloads it from NVIDIA's repository.
- MIT licence ([LICENSE](LICENSE)); third-party parts are listed in [THIRD_PARTY.md](THIRD_PARTY.md). Building
  from source: [BUILDING.md](BUILDING.md).

Assassin's Creed Valhalla is a trademark of Ubisoft. This project is not affiliated with or endorsed by
Ubisoft, AMD, NVIDIA or the projects named above.

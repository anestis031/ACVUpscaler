# Changelog

## 1.0.0-beta.1

First public version.

- FSR 3.1 in place of the game's TAA and FSR 1 upscale. Tested at all four FSR quality settings of the game
  and at native resolution.
- DLSS through OptiScaler 0.9.4, with the choice between the Steam overlay and OptiScaler's menu.
- Texture LOD bias from the render resolution.
- Particle mask (rain, fire) from the game as reactive mask for FSR 3.1.
- Setup tool that installs, changes settings and uninstalls, and only ever touches its own files.
- The upscaler writes straight into the game's texture (no copy), and the game's sharpening pass on top can
  be switched off.

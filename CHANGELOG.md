# Changelog

## 1.0.0-beta.2

- Camera cuts in cutscenes: the frame of the cut is left to the game, and the upscaler starts without history
  after it.
- Own sharpening pass with a strength from 0 to 100, as an alternative to the game's fixed one. Both can be
  changed while playing, in the add-on's panel.

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

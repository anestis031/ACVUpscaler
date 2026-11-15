# Removing ACVUpscaler

## With the setup tool

1. Close the game.
2. Run `ACVUpscalerSetup.exe` and press **Uninstall**. It lists what it will remove and asks first.

The tool removes exactly the files listed in `ACVUpscaler\install_manifest.txt`, the logs, and an empty
`ReShadePreset.ini` that ReShade created. A file that was changed after the installation (for example a
ReShade you updated yourself) is left where it is and named in the tool's log. If a `ReShade.ini` existed
before, it stays and its `AddonPath` gets its old value back.

The game's own settings file in `Documents\Assassin's Creed Valhalla` is not touched by uninstalling: the FSR
quality you chose stays as it is, like any setting made in the game's menu. The copy
`ACValhalla.ini.acvupscaler-backup` next to it is the file as it was before the tool first changed it.

## By hand

Close the game and delete from the game folder:

- `dxgi.dll`
- `winmm.dll`, `ReShade64.dll`, `nvngx_dlss.dll`, `OptiScaler.ini`, `OptiScaler.log` (present with DLSS only)
- `ReShade.ini`, `ReShade.log`, `ReShadePreset.ini` (keep them if you use ReShade for something else, and
  remove only the `AddonPath` line from `ReShade.ini`)
- the folder `ACVUpscaler`

None of these files belongs to the game. Steam's "Verify integrity of game files" does not remove them and is
not needed, but it is a safe way to make sure the game's own files are untouched.

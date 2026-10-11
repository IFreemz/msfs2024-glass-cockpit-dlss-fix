# Glass Cockpit DLSS Fix for MSFS 2024

A ReShade add-on that removes **DLSS ghosting and smearing from glass cockpit displays** in Microsoft Flight
Simulator 2024: speed and altitude tapes, moving maps, ECAM pages and so on stay sharp and readable in DLSS
Quality, Balanced and Performance.

It works with every aircraft that uses the simulator's standard cockpit display shader. That has been tested on the
Fenix A320 family, the iniBuilds A330 and the default DA62, and should cover most glass cockpits.

> Unofficial community mod. Not affiliated with Microsoft, Asobo, NVIDIA or any aircraft developer.
> **Beta:** please report problems (see [Troubleshooting](#troubleshooting)).

## Why the screens smear

DLSS builds each frame from the history of previous frames, guided by motion vectors. A cockpit screen does not
move, but its *content* does, so DLSS keeps blending old numbers into new ones. The game's own hint for this
(the animated-texture mask) isn't enough with current DLSS models, and no DLSS preset (J, K, M) fixes it.

## How the fix works

The same idea that DLSS mods for other games use for HUDs: **draw the screen content after the upscaler**.

1. When the sim draws a cockpit screen, the add-on draws the same mesh once more into a small private map that
   records which part of which screen image lands on each pixel.
2. Right after DLSS, it samples the sim's own clean screen image at full output resolution (with the frame's camera
   jitter removed) and writes it over the DLSS result, so the moving content never goes through DLSS history.
3. Brightness is matched to the sim every frame, so night lighting and the brightness knobs still work. Dirt, sun
   glare and shadows on the glass are taken from the DLSS image itself, limited to what this frame actually shows,
   so they stay sharp while trails of old screen content are removed.

It works at any resolution and any DLSS mode, and costs one extra draw per screen plus a few small GPU passes.

## Requirements

- Microsoft Flight Simulator 2024 (Steam or Microsoft Store/Xbox app), DirectX 12
- NVIDIA RTX GPU with **DLSS Super Resolution** selected in the sim's graphics settings
- [ReShade](https://reshade.me) 6.7 or newer **with full add-on support** (the "with full add-on support" download)

## Installation

1. Install ReShade **with full add-on support** for MSFS 2024 (DirectX 10/11/12). You don't need any effects.
2. Download `glass-cockpit-dlss-fix.addon64` from the [latest release](../../releases/latest).
3. Copy it into the same folder as ReShade's DLL (the folder containing `FlightSimulator2024.exe`):
   - Steam: `...\steamapps\common\Microsoft Flight Simulator 2024\`
   - Microsoft Store / Xbox app: `...\Microsoft Flight Simulator 2024\Content\`
4. Start the sim with DLSS selected and load a flight.

To check that it's working, press **Ctrl+Shift+F11** in the cockpit: every screen it fixes turns green-tinted.
Press it again to turn the tint off.

**Uninstall:** delete `glass-cockpit-dlss-fix.addon64` (and the `.ini`/`.log` files next to it).

## Hotkeys

| Keys | Action |
|---|---|
| Ctrl+Shift+F9 | Fix on/off, for comparison (remembered between sessions) |
| Ctrl+Shift+F10 | Flip the jitter direction. Only needed if the screen text shimmers when the camera is still (remembered) |
| Ctrl+Shift+F11 | Green tint on fixed screens (debug, not remembered) |

## Troubleshooting

The add-on writes `glass-cockpit-dlss-fix.log` next to itself, and keeps the previous session as
`glass-cockpit-dlss-fix.old.log`. **Please attach both to any bug report.**

The log lists your GPU, driver, ReShade version, the DLSS files the sim loaded and other ReShade add-ons, followed by
milestones in order:

1. `milestone DLSS runtime found`
2. `milestone cockpit display shader found`
3. `milestone first cockpit screen recorded`
4. `milestone first DLSS frame`
5. `milestone first overlay frame drawn (fix is active)`

The first missing milestone shows where it stopped. Lines starting with `WARNING` or `ERROR` explain common
problems in plain words, e.g. DLSS not selected, a non-NVIDIA GPU, or a sim update that changed the display shader.

| Problem | Try |
|---|---|
| Nothing changes, no green tint | Check that ReShade is the *full add-on support* build, the file is next to the ReShade DLL and DLSS is selected. Then read the log. |
| Screen text shimmers when the camera is still | Press Ctrl+Shift+F10 once |
| Crash at startup | Remove the add-on, start once, put it back, then send both logs |

## Known limitations

- NVIDIA DLSS Super Resolution only (no FSR/TAA, no AMD/Intel GPUs).
- Not yet tested with DLSS Frame Generation or VR.
- A thin band at each screen's edge (about one render pixel) still comes from DLSS.
- Anything that passes in front of a screen after the cockpit is drawn (e.g. hands in VR) would be covered by it.
- Sim updates that change the cockpit display shader stop the fix (the sim then just looks like stock DLSS) until
  the add-on is updated.

## Building from source

Visual Studio 2022 with the C++ x64 tools and the Windows 10/11 SDK (`dxc`):

```powershell
git clone --recursive https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix
cd msfs2024-glass-cockpit-dlss-fix
powershell -ExecutionPolicy Bypass -File scripts\build.ps1                 # -> build\glass-cockpit-dlss-fix.addon64
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test -ReShadeDll <path to ReShade64.dll>
powershell -ExecutionPolicy Bypass -File scripts\test.ps1                  # offline GPU test, no sim needed
```

The offline test runs the add-on inside a fake `FlightSimulator2024.exe` with a real ReShade, stand-in display
shaders and a fake DLSS that leaves worst-case ghosts. It checks the result pixel by pixel against an unjittered
reference.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

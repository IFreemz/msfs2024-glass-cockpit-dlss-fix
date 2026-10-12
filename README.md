# Glass Cockpit DLSS Fix for MSFS 2024 (DLSS, FSR and VR)

A ReShade add-on that removes **upscaler ghosting and smearing from glass cockpit displays** in Microsoft Flight
Simulator 2024: speed and altitude tapes, moving maps, ECAM pages and so on stay sharp and readable with **DLSS or
FSR**, in every quality mode, on a monitor and in VR.

![Fenix A320 PFD in DLSS Performance: 8 s of stock DLSS smearing the altitude readout, then 8 s with the fix](docs/comparison.webp)

*Fenix A320 at 4K, DLSS Performance (1080p internal, upscaled to 4K), climbing at about 6,700 ft/min.
[Watch in full quality on YouTube](https://www.youtube.com/watch?v=Hcz8hwFndoc).*

It works with every aircraft that uses the simulator's standard cockpit display shader. Tested on the Fenix A320
family, iniBuilds A220, A330, A340, A350 and A380 and the default DA62, and by community testers on the PMDG 737 and 777
and the default Cessna Longitude. It should cover most glass cockpits.

> Unofficial community mod. Not affiliated with Microsoft, Asobo, NVIDIA, AMD or any aircraft developer.
> **Beta:** please report problems (see [Troubleshooting](#troubleshooting)).

## Why the screens smear

DLSS and FSR build each frame from the history of previous frames, guided by motion vectors. A cockpit screen does
not move, but its *content* does, so the upscaler keeps blending old numbers into new ones. The game's own hint for
this (the animated-texture mask) isn't enough, and no DLSS preset (J, K, M) fixes it.

## How the fix works

The same idea that DLSS mods for other games use for HUDs: **draw the screen content after the upscaler**.

1. When the sim draws a cockpit screen, the add-on draws the same mesh once more into a small private map that
   records which part of which screen image lands on each pixel.
2. Right after DLSS or FSR, it samples the sim's own clean screen image at full output resolution (with the frame's
   camera jitter removed) and writes it over the upscaled result, so the moving content never goes through the
   upscaler's history.
3. Brightness is matched to the sim every frame, so night lighting and the brightness knobs still work. Dirt, sun
   glare and shadows on the glass are taken from the upscaled image itself, limited to what this frame actually
   shows, so they stay sharp while trails of old screen content are removed.

In VR the sim draws every view (both eyes, plus the sharp centre views with foveated rendering) in one go and then
upscales them one by one; the add-on records each view separately and learns from the sim which upscale belongs to
which view.

It works at any resolution and in any quality mode, and costs one extra draw per screen plus a few small GPU passes:
no measurable FPS difference with the fix on or off in testing.

## Requirements

- Microsoft Flight Simulator 2024, DirectX 12. Tested on the Microsoft Store / Xbox app (Game Pass) version. The Steam
  version runs the same game and should work the same way: reports welcome.
- **DLSS Super Resolution** (NVIDIA RTX) or **FSR** selected in the sim's graphics settings. FSR also runs on AMD and
  Intel GPUs; confirmed working on an AMD RX 6800 XT (0.7.1 or newer), Intel reports welcome.
- [ReShade](https://reshade.me) 6.7 or newer **with full add-on support** (the "with full add-on support" download)

## Installation

1. Install ReShade **with full add-on support** for MSFS 2024 (DirectX 10/11/12). You don't need any effects.
2. Download `glass-cockpit-dlss-fix.addon64` from the [latest release](../../releases/latest).
3. Copy it into the same folder as ReShade's DLL (the folder containing `FlightSimulator2024.exe`):
   - Steam: `...\steamapps\common\Microsoft Flight Simulator 2024\`
   - Microsoft Store / Xbox app: `...\Microsoft Flight Simulator 2024\Content\`
4. Start the sim with DLSS or FSR selected and load a flight.

To check that it's working, open the ReShade menu (**Home** key) and go to the **Glass Cockpit DLSS Fix** tab: it
says "Working: fixing N screens" and shows a checklist. Or press **Ctrl+Shift+F11** in the cockpit: every screen it
fixes turns green-tinted. Press it again to turn the tint off.

**Uninstall:** delete `glass-cockpit-dlss-fix.addon64` (and the `.ini`/`.log` files next to it).

## Settings

All settings are in the add-on's tab in the ReShade menu (**Home**) and are remembered between sessions.

| Setting | What it does |
|---|---|
| Fix enabled (Ctrl+Shift+F9) | Fix on/off, for comparison |
| Line smoothing (0-50 %, default 35 %) | Smooths stair steps on thin slanted lines in the screens, like the upscaler did. 0 = sharpest, 50 = softest. Ctrl+click to type a value |
| Green tint (Ctrl+Shift+F11) | Tints every screen the fix covers (for checking, not remembered) |
| Flip jitter direction (Ctrl+Shift+F10) | Only needed if the screen text shimmers when the camera is still |

## Troubleshooting

Open the ReShade menu (**Home**) and the **Glass Cockpit DLSS Fix** tab first. It shows what works and what doesn't
(GPU, upscaler running, cockpit shader found, screens recorded, fix drawn) and explains any problem in plain words.
It has an **Open log folder** button and a **Copy report** button for bug reports.

The add-on also writes `glass-cockpit-dlss-fix.log` next to itself, and keeps the previous session as
`glass-cockpit-dlss-fix.old.log`. **Please attach both to any bug report.**

The log lists your GPU, driver, ReShade version, the DLSS/FSR files the sim loaded and other ReShade add-ons,
followed by milestones in order:

1. `milestone DLSS runtime found` / `FSR runtime found`
2. `milestone cockpit display shader found`
3. `milestone first DLSS frame` / `milestone first FSR frame`
4. `milestone first cockpit screen recorded`
5. `milestone first overlay frame drawn (fix is active)`

In VR also `milestone VR cockpit display shader found`, `milestone first VR cockpit screen recorded` and
`vr view order` lines.

The first missing milestone shows where it stopped. Lines starting with `WARNING` or `ERROR` explain common
problems in plain words, e.g. no upscaler selected, or a sim update that changed the display shader.

| Problem | Try |
|---|---|
| Nothing changes, no green tint | Check that ReShade is the *full add-on support* build, the file is next to the ReShade DLL and DLSS or FSR is selected. Then read the tab and the log. |
| Some screens of an aircraft stay normal | Send both logs and the aircraft name |
| Screen lines look jagged / too soft | Adjust Line smoothing |
| Screen text shimmers when the camera is still | Press Ctrl+Shift+F10 once |
| Crash at startup | Remove the add-on, start once, put it back, then send both logs |

## Other upscaling mods

- **OptiScaler:** should work when OptiScaler's upscaler is set to DLSS, since the fix runs right after DLSS
  (untested, reports welcome). With OptiScaler's own FSR or XeSS selected it does not apply.
- **OptiShade:** untested. It ships its own modified ReShade, which needs full add-on support for this fix to load.

## Known limitations

- DLSS Super Resolution and FSR only (not TAA). Works with DLSS Frame Generation (tested up to 6x Multi Frame
  Generation).
- VR: tested with DLSS on a Quest 3 through Virtual Desktop, with and without foveated rendering, and by a community
  tester on a Pimax Crystal Light. FSR in VR and other headsets are untested, reports welcome.
- A thin band at each screen's edge (about one render pixel) still comes from the upscaler.
- Anything that passes in front of a screen after the cockpit is drawn (e.g. hands in VR) would be covered by it.
- Sim updates that change the cockpit display shader stop the fix (the sim then just looks like stock DLSS/FSR)
  until the add-on is updated.

## Building from source

The full source is in this repository. See [BUILDING.md](BUILDING.md) to build the add-on yourself.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

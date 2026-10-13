# Glass Cockpit DLSS Fix for MSFS 2024 (DLSS, FSR and VR)

A ReShade add-on that removes **upscaler ghosting and smearing from glass cockpit displays** in Microsoft Flight
Simulator 2024: speed and altitude tapes, moving maps, ECAM pages and so on stay sharp and readable with **DLSS or
FSR**, in every quality mode, on a monitor and in VR.

![Fenix A320 PFD in DLSS Performance: 8 s of stock DLSS smearing the altitude readout, then 8 s with the fix](docs/comparison.webp)

*Fenix A320 at 4K, DLSS Performance (1080p internal, upscaled to 4K), climbing at about 6,700 ft/min.
[Watch in full quality on YouTube](https://www.youtube.com/watch?v=Hcz8hwFndoc).*

It works with every aircraft that uses the simulator's cockpit display shaders, so it should cover most glass
cockpits. See [Compatibility](#compatibility) for the tested aircraft, VR headsets and GPUs.

> Unofficial community mod. Not affiliated with Microsoft, Asobo, NVIDIA, AMD or any aircraft developer.
> **Beta:** please report problems (see [Troubleshooting](#troubleshooting)).

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Settings](#settings)
- [Compatibility](#compatibility)
- [Troubleshooting](#troubleshooting)
- [How the fix works](#how-the-fix-works)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [License](#license)

## Requirements

- Microsoft Flight Simulator 2024, DirectX 12 (Steam or Microsoft Store / Xbox app).
- **DLSS Super Resolution** (NVIDIA RTX) or **FSR** (any GPU) selected in the sim's graphics settings.
- [ReShade](https://reshade.me) 6.7 or newer **with full add-on support** (the "with full add-on support" download)

## Installation

1. Install ReShade **with full add-on support** for MSFS 2024 (DirectX 10/11/12). You don't need any effects.
   In the ReShade setup, use **Browse...** and pick `FlightSimulator2024.exe`.
   Microsoft Store / Xbox app: the game folder is usually `C:\XboxGames\Microsoft Flight Simulator 2024\Content\`. To
   find it, open the Xbox app, right-click the game, then **Manage → Files → Browse**. Try `FlightSimulator2024.exe`
   first; if Windows refuses ("You don't have permission to open this file"), pick **`gamelaunchhelper.exe`** in the
   same folder instead: ReShade lands in the same place and loads with the sim. If setup says `dxgi.dll` already
   exists and doesn't belong to ReShade, another mod (e.g. OptiScaler or DLSS Enabler) is installed as `dxgi.dll`; see
   [Other mods](#other-mods) to load ReShade alongside it.
2. Download `glass-cockpit-dlss-fix.addon64` from the [latest release](../../releases/latest).
3. Copy it into the same folder as ReShade's DLL (the folder containing `FlightSimulator2024.exe`):
   - Steam: `...\steamapps\common\Microsoft Flight Simulator 2024\`
   - Microsoft Store / Xbox app: `...\Microsoft Flight Simulator 2024\Content\`
4. Start the sim with DLSS or FSR selected and load a flight.

To check that it's working, open the ReShade menu (**Home** key) and go to the **Glass Cockpit DLSS Fix** tab: it
says "Working: fixing N screens" and shows a checklist. Or press **Ctrl+Shift+F11** in the cockpit: every screen it
fixes turns green-tinted. Press it again to turn the tint off.

**Update:** replace the `.addon64` file with the new one. **Uninstall:** delete `glass-cockpit-dlss-fix.addon64`
(and the `.ini`/`.log` files next to it).

## Settings

All settings are in the add-on's tab in the ReShade menu (**Home**) and are remembered between sessions.

| Setting | What it does |
|---|---|
| Fix enabled (Ctrl+Shift+F9) | Fix on/off, for comparison |
| Line smoothing (0-50 %, default 35 %) | Smooths stair steps on thin slanted lines in the screens, like the upscaler did. 0 = sharpest, 50 = softest. Ctrl+click to type a value |
| Green tint (Ctrl+Shift+F11) | Tints every screen the fix covers (for checking, not remembered) |

Under **Troubleshooting → Advanced** (collapsed; only change these if asked to in a bug report, "Reset to defaults"
puts them back):

| Setting | What it does |
|---|---|
| Flip jitter direction | Only needed if the screen text shimmers when the camera is still |
| Glass layer (0-100 %, default 100 %) | How much of the sim's glass effects (dirt, glare, reflections, shadows) is kept on fixed screens. Lower values can help if quickly changing numbers leave a milky trail |

Under **Troubleshooting → Experimental fixes** (collapsed, **all off by default**; for problems that only some
aircraft have, "Reset experimental fixes" turns them all off again):

| Setting | What it does |
|---|---|
| Detail clamp (0-100 %) | Keeps the fixed screens within the brightness the sim itself draws around each pixel. Hides texture detail too thin for the sim to show, e.g. a **dashed light line where a screen's picture meets its bezel** (FSS E-Jets: try 100 %). **Can make screens flicker**, the more the lower the DLSS render resolution: almost none with DLAA and Quality, more with Balanced and Performance. Lower it if that bothers you, or if small text looks duller than with the fix off |
| Edge fade (0-250) | Fades screen edges into the sim's own picture, in hundredths of a render pixel (100 = 1 pixel). Can calm jaggy or crawling screen edges. Above about 200 it can flicker |
| Angle softening (0-100 %) | Draws screens seen at a steep angle softer, like the sim does. Screens seen straight on are unchanged |
| Edge stretcher (0-400) | Extends screens outwards over their frame, in hundredths of a render pixel, continuing their picture. For screens that don't quite reach their frame |

The tab shows "Experimental fixes changed" while any of them is on.

ReShade presets and effects keep working as usual on top of the fix.

## Compatibility

### Aircraft

| Aircraft | Status |
|---|---|
| Fenix A320 family | Tested |
| iFly 737 MAX 8 | Tested (0.7.5 or newer; the EFB tablet is left to DLSS/FSR, see [Known limitations](#known-limitations)) |
| iniBuilds A220 | Tested |
| iniBuilds A330 | Tested |
| iniBuilds A340 | Tested (0.7.2 or newer; in VR 0.7.3 or newer) |
| iniBuilds A350 | Tested |
| iniBuilds A380 | Tested |
| PMDG 737 | Tested by the community (also in VR) |
| PMDG 777 | Tested by the community (also in VR) |
| FlightFX C750 | Tested by the community (0.7.6 or newer) |
| Default DA62 | Tested |
| Default Cessna Longitude | Tested by the community (in VR) |
| FSS E-Jets (ERJ-170/175/190/195) | Tested (a dashed line at the screens' edges: set Detail clamp to 100 %, see [Settings](#settings)) |
| Default F/A-18E Super Hornet | Tested (0.8.2 or newer keeps its dark screens fixed in strong sun) |

Other glass cockpits should work too. If some screens of an aircraft don't turn green with Ctrl+Shift+F11, please
report it (see [Troubleshooting](#troubleshooting)).

### VR

No special install for VR: install ReShade for MSFS 2024 exactly as for a monitor (DirectX 12, full add-on support).
The fix works inside the sim's own rendering, so it covers the headset views automatically. ReShade's OpenXR / VR
option is only for showing ReShade's own effects and menu in the headset and isn't needed.

| Headset | Status |
|---|---|
| Meta Quest 3 through Virtual Desktop | Tested with DLSS and FSR, foveated rendering on and off |
| Pimax Crystal Light | Tested by the community (DLSS) |

Other headsets and runtimes should work the same; reports welcome.

### GPUs and upscalers

| Setup | Status |
|---|---|
| NVIDIA RTX, DLSS Super Resolution | Tested, including DLSS Frame Generation up to 6x Multi Frame Generation |
| NVIDIA, FSR | Tested |
| AMD, FSR | Tested by the community on an RX 6800 XT and an RX 7800 XT (0.7.6 or newer runs much lighter on AMD and Intel) |
| Intel, FSR | Untested, reports welcome |
| Any GPU, upscaler chosen in OptiScaler | Tested on NVIDIA with DLSS, FSR 2/3 and XeSS, and by the community on an AMD RX 9070 XT with DLSS selected and FSR 4 underneath (0.8.0 or newer), see [Other mods](#other-mods) |

### Game versions

Tested on the Microsoft Store / Xbox app (Game Pass) version and by the community on Steam.

### Other mods

- **ReShade presets and effects:** work as usual.
- **DLSS 5 / Neural Rendering add-ons:** tested alongside the RenoDX DLSS 5 add-on.
- **OptiShade:** the OptiShade developer is building this fix into OptiShade.
- **Multi Frame Generation unlocks for RTX 40:**
  - **RenoDX MFG unlock add-on:** tested, works together with this fix.
  - **[RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) by dashdogy:** tested with v1.4.2 (installed as
    `winmm.dll` next to an ASI loader's `version.dll`; `version.dll` alone works too). It redirects DLSS, and 0.7.5 or
    newer follows that redirect; older versions log "could not hook DLSS evaluate". Its Backspace menu doesn't show up
    when ReShade is loaded, with or without this fix, so set the multiplier in the sim's frame generation setting or
    in its `RTXMFG-Universal.json`.
- **OptiScaler:** works with any upscaler selected in OptiScaler (DLSS, FSR 2/3/4, XeSS; 0.8.0 or newer for anything
  but DLSS), including OptiScaler's DLSS 5 Neural Rendering. On AMD and Intel GPUs this also lets you select DLSS in
  the sim and have OptiScaler run FSR or XeSS (0.8.0 or newer; tested on NVIDIA, and by the community on an AMD RX 9070 XT
  with FSR 4). If ReShade
  says "There were errors loading some add-ons", update to 0.7.4 or newer. If another mod also needs to be
  `dxgi.dll`, load ReShade through an ASI loader or through OptiScaler's `LoadReshade` option instead.
- **DLSS Enabler:** built on OptiScaler, so it should work the same way (0.8.0 or newer). Not tested yet, reports
  welcome.

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
| No "Glass Cockpit DLSS Fix" tab in the ReShade menu | The add-on isn't loaded. If the ReShade menu has no **Add-ons** tab, reinstall ReShade with the *full add-on support* download. Check that the file is next to the ReShade DLL, and that **Settings → Add-on search path** in the ReShade menu points to that folder. |
| Nothing changes, no green tint | Check that ReShade is the *full add-on support* build, the file is next to the ReShade DLL and DLSS or FSR is selected. Then read the tab and the log. |
| Picture freezes or turns black with Frame Generation on (fine when alt-tabbed) | Update to 0.8.1 or newer (0.8.0 only) |
| Tab says "Only DLSS Frame Generation is running" | Frame Generation is on but Anti-Aliasing isn't DLSS or FSR (e.g. TAA). Set Anti-Aliasing to DLSS (any mode, DLAA too) or FSR; Frame Generation can stay on |
| Some screens of an aircraft stay normal | Look at those screens in the cockpit, press **Ctrl+Shift+F12** (writes a short diagnostic to the log), then send both logs and the aircraft name |
| Screen lines look jagged / too soft | Adjust Line smoothing |
| Dashed light line along the edge of a screen's picture (e.g. FSS E-Jets) | Set **Detail clamp** under Troubleshooting → Experimental fixes to 100 % (can add some flicker on DLSS Balanced/Performance) |
| Dark screens drop out of the fix (green tint goes away) in strong sun | Update to 0.8.2 or newer |
| Screen text shimmers when the camera is still | Tick **Flip jitter direction** in the Advanced section under Troubleshooting. If it's already ticked (the tab says "Advanced settings changed"), click **Reset to defaults** |
| Screens flicker in brightness (G-Sync / FreeSync monitor) | VRR flicker from a changing frame rate, most visible on dark screens. Turn on V-Sync, or cap the frame rate a few fps below your refresh rate |
| Crash at startup | Remove the add-on, start once, put it back, then send both logs |

## How the fix works

DLSS and FSR build each frame from the history of previous frames, guided by motion vectors. A cockpit screen does
not move, but its *content* does, so the upscaler keeps blending old numbers into new ones. The game's own hint for
this (the animated-texture mask) isn't enough, and no DLSS preset (J, K, M) fixes it.

The fix uses the same idea that DLSS mods for other games use for HUDs: **draw the screen content after the
upscaler**.

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

## Known limitations

- A screen whose picture the add-on can't redraw (e.g. the iFly 737 MAX EFB tablet) is left to DLSS/FSR, so it looks
  like stock.
- A thin band at each screen's edge (about one render pixel) still comes from the upscaler (Edge fade and Edge
  stretcher under Experimental fixes can help on some aircraft).
- Anything that passes in front of a screen after the cockpit is drawn (e.g. hands in VR) would be covered by it.
- Sim updates that change the cockpit display shader stop the fix (the sim then just looks like stock DLSS/FSR)
  until the add-on is updated.

## Building from source

The full source is in this repository. See [BUILDING.md](BUILDING.md) to build the add-on yourself.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

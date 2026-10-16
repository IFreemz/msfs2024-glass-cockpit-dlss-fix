# Glass Cockpit DLSS Fix for MSFS 2024 (DLSS, FSR and VR)

Removes **upscaler ghosting and smearing from glass cockpit displays** in Microsoft Flight Simulator 2024: speed and
altitude tapes, moving maps, ECAM pages and so on stay sharp and readable with **DLSS or FSR**, in every quality mode,
on a monitor and in VR.

It comes in two versions with the same fix inside; pick one:

- **ReShade add-on** (`glass-cockpit-dlss-fix.addon64`): for ReShade users. Status and settings in a tab of the ReShade
  menu.
- **Standalone** (`dxgi.dll`): no ReShade needed, one file next to the sim. Settings in a text file.

![Fenix A320 PFD in DLSS Performance: 8 s of stock DLSS smearing the altitude readout, then 8 s with the fix](docs/comparison.webp)

*Fenix A320 at 4K, DLSS Performance (1080p internal, upscaled to 4K), climbing at about 6,700 ft/min.
[Watch in full quality on YouTube](https://www.youtube.com/watch?v=Hcz8hwFndoc).*

It works with every aircraft that uses the simulator's cockpit display shaders, so it should cover most glass
cockpits. See [Compatibility](#compatibility) for the tested aircraft, VR headsets and GPUs.

> Unofficial community mod. Not affiliated with Microsoft, Asobo, NVIDIA, AMD or any aircraft developer.
> Please report problems (see [Troubleshooting](#troubleshooting)). After 1.0.0, updates will be small hotfixes only
> for a while. The standalone version is the newer of the two: reports on how it works for you are especially welcome.

**About this project:** Glass Cockpit DLSS Fix is developed by IFreemz. It was first released here on 11 October 2026
(v0.6.0), and every version since has been published from this repository. Official downloads: the
[GitHub releases](https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix/releases) (and [OptiShade](https://github.com/GamingWithGravy/OptiShade), which bundles
the official add-on). Other packages, rebuilds or pages are unofficial; for those, please contact their authors.

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Settings](#settings)
- [Before you report a problem: what to try](#before-you-report-a-problem-what-to-try)
- [Compatibility](#compatibility)
- [Troubleshooting](#troubleshooting)
- [How the fix works](#how-the-fix-works)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [License](#license)
- [Official downloads and redistribution](#official-downloads-and-redistribution)

## Requirements

- Microsoft Flight Simulator 2024, DirectX 12 (Steam or Microsoft Store / Xbox app). **MSFS 2020 is not supported**
  (see [Game versions](#game-versions)).
- **DLSS Super Resolution** (NVIDIA RTX) or **FSR** (any GPU) selected in the sim's graphics settings.
- ReShade add-on only: [ReShade](https://reshade.me) 6.7 or newer **with full add-on support** (the "with full add-on
  support" download)

## Installation

Install **one** of the two versions. If both are installed, the standalone version stays off.

The game folder is the one that contains `FlightSimulator2024.exe`:

- Steam: `...\steamapps\common\MSFS2024\`. To find it, right-click the game in Steam, then **Manage → Browse local
  files**.
- Microsoft Store / Xbox app: usually `C:\XboxGames\Microsoft Flight Simulator 2024\Content\`. To find it, open the
  Xbox app, right-click the game, then **Manage → Files → Browse**.

### ReShade add-on

1. Install ReShade **with full add-on support** for MSFS 2024 (DirectX 10/11/12). You don't need any effects.
   In the ReShade setup, use **Browse...** and pick `FlightSimulator2024.exe` in the game folder.
   Microsoft Store / Xbox app: try `FlightSimulator2024.exe` first; if Windows refuses ("You don't have permission to
   open this file"), pick **`gamelaunchhelper.exe`** in the same folder instead: ReShade lands in the same place and
   loads with the sim. If setup says `dxgi.dll` already exists and doesn't belong to ReShade, another mod (e.g.
   OptiScaler or DLSS Enabler) is installed as `dxgi.dll`; see [Other mods](#other-mods) to load ReShade alongside it.
2. Download `glass-cockpit-dlss-fix.addon64` from the [latest release](../../releases/latest).
3. Copy it into the same folder as ReShade's DLL (the game folder).
4. Start the sim with DLSS or FSR selected and load a flight.

To check that it's working, open the ReShade menu (**Home** key) and go to the **Glass Cockpit DLSS Fix** tab: it
says "Working: fixing N screens" and shows a checklist. Or press **Ctrl+Shift+F11** in the cockpit: every screen it
fixes turns green-tinted. Press it again to turn the tint off.

**Update:** replace the `.addon64` file with the new one. **Uninstall:** delete `glass-cockpit-dlss-fix.addon64`
(and the `glass-cockpit-dlss-fix.*` files next to it).

### Standalone (no ReShade)

1. Download `dxgi.dll` from the [latest release](../../releases/latest).
2. Copy it into the game folder.
3. Start the sim with DLSS or FSR selected and load a flight.

If Windows asks whether to replace an existing `dxgi.dll` there, **don't**: another mod (usually ReShade or
OptiScaler) is already using that name. See [Other mods](#other-mods).

To check that it's working, press **Ctrl+Shift+F11** in the cockpit: every screen it fixes turns green-tinted. Press it
again to turn the tint off. The file `glass-cockpit-dlss-fix-status.txt` next to the DLL also says
"Working: fixing N screens" and shows a checklist.

**Switching from the add-on?** Delete `glass-cockpit-dlss-fix.addon64`. If you used ReShade only for the fix,
uninstall ReShade too (its `dxgi.dll` is in the same folder). Your settings carry over: both versions use the same
`glass-cockpit-dlss-fix.ini`.

**Update:** replace `dxgi.dll` with the new one. **Uninstall:** delete `dxgi.dll` and the `glass-cockpit-dlss-fix.*`
files next to it.

## Settings

The add-on has all settings in its tab in the ReShade menu (**Home**). Both versions keep them in
`glass-cockpit-dlss-fix.ini` next to the add-on or DLL, which lists every setting (the tables below say what each one
does); with the standalone version, edit it with Notepad while the sim runs: saved changes apply within a second.

| Setting (ReShade tab / `.ini`) | What it does |
|---|---|
| Fix enabled / `enabled` (Ctrl+Shift+F9) | Fix on/off, for comparison |
| Line smoothing / `line_smoothing` (0-50 %, default 35 %) | Smooths stair steps on thin slanted lines in the screens, like the upscaler did. 0 = sharpest, 50 = softest. In the tab, Ctrl+click to type a value. Tick **Extra** next to it (`extra_smoothing=1`) to go up to 100 % (blurs the whole screen; for screens with very jagged lines) |
| Green tint (Ctrl+Shift+F11) | Tints every screen the fix covers (for checking, not remembered) |

Under **Troubleshooting → Advanced** in the tab, `[advanced]` and `jitter_positive` in the `.ini` (only change these if
asked to in a bug report; "Reset to defaults" in the tab puts them back):

| Setting (ReShade tab / `.ini`) | What it does |
|---|---|
| Flip jitter direction / `jitter_positive=0` | Only needed if the screen text shimmers when the camera is still |
| Glass layer / `glass_strength` (0-100 %, default 100 %) | How much of the sim's glass effects (dirt, glare, reflections, shadows) is kept on fixed screens. Lower values can help if quickly changing numbers leave a milky trail |

Under **Troubleshooting → Experimental fixes** in the tab, `[experimental]` in the `.ini` (**all off by default except
UV seam fix, Full-size sampling, Stable screen edges and Occlusion check**; for problems that only some aircraft have,
"Reset experimental fixes" in the tab puts them all back to
default):

| Setting (ReShade tab / `.ini`) | What it does |
|---|---|
| Full-size sampling / `full_size_sampling` (on) | Reads the screens only from their full-size picture, with more samples where a screen is small on your monitor. Off: also from the smaller copies the GPU keeps for distant screens, which can shimmer when you zoom out. **Ctrl+Shift+F7** switches it in the sim |
| Stable screen edges / `stable_edges` (on) | Keeps the pixels along each screen's edge from shimmering: where a screen ends is learnt over a few frames instead of judged anew every frame. Turn it off if a screen's edge looks cut off or too wide |
| Occlusion check / `occlusion_check` (on) | Leaves the sim's own picture wherever something sits in front of a screen (a sidestick, a hand, the yoke), using the sim's depth buffer. Off: a screen can be drawn over things in front of it, e.g. the A380 EFB over the sidestick |
| Calm glass at symbols / `calm_glass` (off) | Right next to symbols and digits, draws the glass effects (dirt, glare, reflections) without their finest detail, which can shimmer there. On some screens it can add shimmer instead, so it's off by default |
| Dark screen fix / `dark_screen_fix` (off) | Keeps very dark screens fixed in strong sunlight (e.g. the F/A-18E Super Hornet). Off by default since 0.8.3, because it can also let the fix draw over a screen whose picture it can't redraw |
| UV seam fix / `seam_fix` (on) | Removes thin dashed lines where a screen's picture meets a part of the same screen that uses another area of its image (FSS E-Jets: the line between the picture and its bezel band), and keeps those edges smooth on low DLSS modes. If a screen edge looks worse with it on, turn it off and please report the aircraft |
| Soft-edged line smoothing / `soft_smoothing` (off) | Line smoothing weighted towards each pixel's centre over a wider area: smoother screens with less shimmer while the view moves. Plain Line smoothing works well on most aircraft; this is an alternative if screens still shimmer while you move your head. A matter of taste; needs Line smoothing above 0 |
| Temporal anti-aliasing / `temporal_aa` (off) | Smooths screen text and lines over several frames, like the sim's own TAA, without the trails (the history is dropped wherever a screen's picture changes). Plain Line smoothing works well on most aircraft; this is an alternative if you want sharper text that still doesn't stair-step, with Line smoothing at 0-20. Uses about 100 MB more video memory at 4K. Not together with Soft-edged line smoothing |
| Edge trim / `edge_trim` (0-400) | The reverse of the edge stretcher: leaves the outer edge of every screen to the sim's own picture, in hundredths of a render pixel. For screens whose edge shows a sliver of something that isn't there, or jagged or flickering edges: **50 calms most flickering or shimmering screen edges**, up to 150 for aircraft with stronger edge problems. Within that band the upscaler's trails come back |
| Detail clamp / `detail_clamp` (0-100 %) | Keeps the fixed screens within the brightness the sim itself draws around each pixel. Hides texture detail too thin for the sim to show. **Can make screens flicker**, the more the lower the DLSS render resolution: almost none with DLAA and Quality, more with Balanced and Performance. Lower it if that bothers you, or if small text looks duller than with the fix off |
| Edge fade / `edge_fade` (0-250) | Fades screen edges into the sim's own picture, in hundredths of a render pixel (100 = 1 pixel). Can calm jaggy or crawling screen edges. Above about 200 it can flicker |
| Angle softening / `angle_softening` (0-100 %) | Draws screens seen at a steep angle softer, like the sim does. Screens seen straight on are unchanged |
| Edge stretcher / `edge_stretch` (0-400) | Extends screens outwards over their frame, in hundredths of a render pixel, continuing their picture. For screens that don't quite reach their frame |

The tab and the status file say "Experimental fixes changed" while any of them is on.

ReShade presets and effects keep working as usual on top of the fix.

## Before you report a problem: what to try

Most visual problems on a particular aircraft can be fixed with a setting. Please try the ones below before opening an
issue: it's quicker for you, and if none of them helps, your report tells me exactly what was already ruled out.

### How to try a setting

1. In the cockpit, open the ReShade menu (**Home** key) and go to the **Glass Cockpit DLSS Fix** tab.
2. Open **Troubleshooting → Experimental fixes** (or **Advanced**). Hover over any option to see what it does.
3. Change one thing at a time and look at the problem screen for a few seconds. Changes take effect immediately and
   are remembered between sessions.
4. To compare with stock DLSS/FSR, toggle the whole fix with **Ctrl+Shift+F9**. **Ctrl+Shift+F11** tints every screen
   the fix covers green, so you can see which screens it handles.
5. Made it worse? **Reset experimental fixes** (or **Reset to defaults** under Advanced) puts everything back.

With the standalone version, change the same settings in `glass-cockpit-dlss-fix.ini` (their names are in the
[Settings](#settings) tables) with Notepad while the sim runs: saved changes apply within a second. To put everything
back, delete the `.ini`; the next start writes a fresh one.

The tab and the status file show "Experimental fixes changed" while any of them differs from the default, so you can
always see that something was changed.

### Which setting for which problem

| What you see | Try first | Then |
|---|---|---|
| Screen edges flicker, shimmer or crawl, especially while moving | **Edge trim** 50 | Edge trim up to 150; Edge fade 100 |
| A sliver of something that isn't there along a screen's edge, or the edge looks cut off or too wide | **Edge trim** 50 | Untick Stable screen edges |
| A thin dashed or light line where a screen's picture meets its frame | Keep **UV seam fix** ticked | Detail clamp 100 |
| A screen doesn't quite reach its frame (a gap at the edge) | **Edge stretcher** 100 | Up to 300 |
| A screen is drawn over something in front of it (sidestick, hand, yoke) | Keep **Occlusion check** ticked | Report it with a capture |
| Small screens shimmer when you zoom out or lean back | Keep **Full-size sampling** ticked | Line smoothing 40-50 |
| Digits or symbols shimmer or sparkle, mostly in sun glare | **Calm glass at symbols** | Glass layer 50-70 % |
| Quickly changing numbers leave a milky trail | **Glass layer** 50-70 % | Calm glass at symbols |
| Text and lines look too jagged | **Line smoothing** 40-50 | Soft-edged line smoothing, or Temporal anti-aliasing with Line smoothing 0-20 |
| Text and lines look too soft or blurry | **Line smoothing** 15-25 | Untick Soft-edged line smoothing and Temporal anti-aliasing |
| A very dark screen drops out of the fix (green tint goes away) in strong sun | **Dark screen fix** | |
| Screens seen from a steep angle look sharper than the rest of the cockpit | **Angle softening** 50 | |
| Screen text shimmers even when the camera is completely still | **Flip jitter direction** (Advanced) | |

If a screen is never fixed at all (no green tint), settings won't help: see [What to send with a bug report](#what-to-send-with-a-bug-report).

### The options in detail

**Edge trim** (0-400, off by default). Leaves a thin band at the outer edge of every screen to the sim's own picture.
The fix redraws each screen exactly up to where it ends; on some aircraft that edge moves a little with the camera
jitter every frame and flickers, or shows a sliver of the frame next to it. Edge trim hands that band back to DLSS/FSR,
which draws it smoothly. **50 calms most flickering or shimmering edges**; go up to 150 for aircraft with stronger
edge problems. The values are hundredths of a render pixel (100 = one pixel at the sim's internal resolution). Side
effect: within that band, DLSS/FSR's trails come back, so keep it as low as works.

**Stable screen edges** (on by default). Learns over a few frames where each screen ends, instead of judging it anew
every frame, so the pixels along the edges don't shimmer. Untick it if a screen's edge looks cut off or a little too
wide.

**Occlusion check** (on by default). Uses the sim's depth buffer to find anything sitting in front of a screen (a
sidestick, a yoke, your hands in VR, an EFB arm) and leaves those pixels to the sim's own picture, so the fix never
draws a screen over them. If something in front of a screen still gets painted over, please report it with a capture.

**Full-size sampling** (on by default, Ctrl+Shift+F7 to toggle). Reads the screen pictures only at full size, with
more samples where a screen is small on your monitor. Off, the fix also uses the smaller copies the GPU keeps for
distant screens, like the sim does, which can shimmer when you zoom out or lean back.

**Calm glass at symbols** (off by default). Right next to digits, symbols and lines, the glass effects (dirt, glare,
reflections) are drawn without their finest detail. That detail comes from the upscaler's picture and can shimmer
along the edges of symbols, especially in sun glare. Try it if digits sparkle or shimmer while the rest of the screen
is fine. Side effect: on some screens it can add shimmer instead, and something drawn over a screen (a highlight, a
cursor) can look a little softer; untick it again if so.

Plain **Line smoothing** (on the main tab) works well on most aircraft. The next two options are alternatives that
solve slightly different problems; try them only if Line smoothing alone doesn't look right to you.

**Soft-edged line smoothing** (off by default). A different flavour of Line smoothing: weighted towards each pixel's
centre over a wider area. Try it if screens still shimmer while you move your head. Screens look smoother with less
shimmer while the view moves, at the cost of slightly softer text. A matter of taste; it needs Line smoothing above 0.

**Temporal anti-aliasing** (off by default). Smooths screen text and lines over several frames, like the sim's own TAA,
but without the trails: the history is thrown away wherever a screen's picture changes. Try it with Line smoothing at
0-20 if you want the sharpest text that still doesn't stair-step. Uses about 100 MB more video memory at 4K. It and
Soft-edged line smoothing are alternatives: ticking one unticks the other.

**Glass layer** (Advanced, 0-100 %, default 100 %). How much of the sim's glass effects (dirt, glare, reflections,
shadows) is kept on fixed screens. 100 % looks the most like the sim; lower values give cleaner, flatter screens and
can help if quickly changing numbers leave a milky trail or digits shimmer in the sun. At 0 % the screens are drawn
with no glass effects at all.

The older options (UV seam fix, Dark screen fix, Detail clamp, Edge fade, Angle softening, Edge stretcher, Flip jitter
direction) are described in the [Settings](#settings) tables above.

## Compatibility

Both versions run the same fix; the results below were tested with the ReShade add-on unless noted. The standalone
version gets into the sim differently, so please report how it works for you.

### Aircraft

| Aircraft | Status |
|---|---|
| Fenix A320 family | Tested |
| iFly 737 MAX 8 | Tested (1.0.0 or newer covers every screen: CDUs, radio panels, transponder, ADF, MCP digits and both EFB tablets) |
| iniBuilds A220 | Tested |
| iniBuilds A330 | Tested |
| iniBuilds A340 | Tested (0.7.2 or newer; in VR 0.7.3 or newer) |
| iniBuilds A350 | Tested |
| iniBuilds A380 | Tested (1.0.0 or newer: the EFB stays fixed when only part of it is in view, and screens are no longer drawn over the sidestick) |
| PMDG 737 | Tested by the community (also in VR) |
| PMDG 777 | Tested by the community (also in VR) |
| FlightFX C750 | Tested by the community (0.7.6 or newer) |
| Default DA62 | Tested |
| Default Cessna Longitude | Tested by the community (in VR) |
| FSS E-Jets (ERJ-170/175/190/195) | Tested (0.8.3 or newer removes the dashed line at the screens' edges) |
| Default F/A-18E Super Hornet | Tested (if its dark screens drop out of the fix in strong sun, turn on Dark screen fix, see [Settings](#settings)) |

Other glass cockpits should work too. If some screens of an aircraft don't turn green with Ctrl+Shift+F11, please
report it (see [Troubleshooting](#troubleshooting)).

### VR

No special install for VR: install either version exactly as for a monitor (for the add-on: ReShade for DirectX 12 with
full add-on support). The fix works inside the sim's own rendering, so it covers the headset views automatically.
ReShade's OpenXR / VR option is only for showing ReShade's own effects and menu in the headset and isn't needed.

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

**MSFS 2020 is not supported.** The fix finds the cockpit screens by how MSFS 2024 draws them, and I don't have
2020 to test or port it. The source is open (GPL-3.0 from 1.0.0), so a 2020 port by someone who has it is welcome.

### Other mods

- **ReShade presets and effects:** work as usual with the add-on. With the standalone version, ReShade can't be
  `dxgi.dll` at the same time: load it through an ASI loader (as `ReShade64.asi`) or through OptiScaler's
  `LoadReshade` option. Its effects then work as usual on top of the fix. (If you use ReShade anyway, the add-on is the
  simpler choice.)
- **DLSS 5 / Neural Rendering add-ons:** tested alongside the RenoDX DLSS 5 add-on. **Freeze warning:** with
  `renodx-dlss.addon64` installed, the sim can freeze when a Fenix aircraft loads. This happens with this fix removed
  too, so it's between RenoDX and the Fenix; if you get it, remove `renodx-dlss.addon64` (OptiScaler or OptiShade also
  offer Neural Rendering).
- **Extra DLSS passes:** if a mod or the sim runs a second DLSS pass at the internal resolution, 0.8.3 or newer keeps
  the fix on the sim's real upscale (the one with the largest output).
- **Standalone DLSS Glass Cockpit Refresh** ([wesimglide.org](https://wesimglide.org/otherdata/tools/Standalone-DLSS-Glass-Cockpit-Refresh.html)):
  a third-party package of this add-on with ReShade built in, by WeSimGlide. For problems with it, follow the support
  steps on its page. Don't use it together with either version of this fix (the fix would run twice).
- **[OptiShade](https://github.com/GamingWithGravy/OptiShade)** by Gravy: bundles the official, unmodified add-on (an OptiShade update with 1.0.0 is coming
  soon). Use one or the other, not both: with OptiShade, don't also install the `.addon64` or the `dxgi.dll` from here.
  Problems with OptiShade itself (installing, its presets and effects, updates) go to
  [OptiShade's issues](https://github.com/GamingWithGravy/OptiShade/issues); problems with the fixed screens come here.
- **Multi Frame Generation unlocks for RTX 40:**
  - **RenoDX MFG unlock add-on:** tested, works together with this fix.
  - **[RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) by dashdogy:** tested with v1.4.2 (installed as
    `winmm.dll` next to an ASI loader's `version.dll`; `version.dll` alone works too), so it can stay as it is with
    either version. It redirects DLSS, and 0.7.5 or newer follows that redirect; older versions log "could not hook
    DLSS evaluate". Its Backspace menu doesn't show up when ReShade is loaded, with or without this fix, so set the
    multiplier in the sim's frame generation setting or in its `RTXMFG-Universal.json`.
- **OptiScaler:** works with any upscaler selected in OptiScaler (DLSS, FSR 2/3/4, XeSS; 0.8.0 or newer for anything
  but DLSS), including OptiScaler's DLSS 5 Neural Rendering. On AMD and Intel GPUs this also lets you select DLSS in
  the sim and have OptiScaler run FSR or XeSS (0.8.0 or newer; tested on NVIDIA, and by the community on an AMD RX 9070 XT
  with FSR 4). If ReShade says "There were errors loading some add-ons", update to 0.7.4 or newer. OptiScaler is
  usually installed as `dxgi.dll`:
  - with the add-on, load ReShade through an ASI loader or through OptiScaler's `LoadReshade` option instead;
  - with the standalone version, rename OptiScaler's `dxgi.dll` to another name it supports (e.g. `winmm.dll` or
    `version.dll`) and keep the fix as `dxgi.dll`.
- **DLSS Enabler:** built on OptiScaler, so it should work the same way (0.8.0 or newer). Not tested yet, reports
  welcome.
- **Standalone version through an ASI loader instead of as `dxgi.dll`:** don't. An ASI loader usually loads the DLL
  after the sim has set up its graphics, and then the fix can't see what it needs: screens will most likely stay
  unfixed (the log says so).

## Troubleshooting

First look at what the fix reports about itself: the add-on's **Glass Cockpit DLSS Fix** tab in the ReShade menu
(**Home**), or with the standalone version `glass-cockpit-dlss-fix-status.txt` next to the DLL (updated every 10 seconds
while the sim runs). Both show what works and what doesn't (GPU, upscaler running, cockpit shader found, screens
recorded, fix drawn), explain any problem in plain words and give a report for bug reports (the tab's **Copy report**
button, the end of the status file). The tab also has an **Open log folder** button.

For something you can *see* on fixed screens (flickering edges, shimmer, lines, blur, things drawn over), first try
the settings in [Before you report a problem: what to try](#before-you-report-a-problem-what-to-try).

### What to send with a bug report

| Problem | Send |
|---|---|
| The fix doesn't work at all, the tab or status file shows a problem, a screen is never fixed, a crash or freeze | **Both logs**: `glass-cockpit-dlss-fix.log` and `glass-cockpit-dlss-fix.old.log` (next to the add-on or DLL; button **Open log folder** in the tab). For screens that are never fixed, press **Ctrl+Shift+F12** while looking at them first |
| Something you can see on fixed screens: wrong or changing colours, flicker, lines, jagged or blurry edges, trails | A **capture**: with the problem visible, press **Ctrl+Shift+F8** (or **Capture screens** in the tab), keep the screens in view for a few seconds, then attach the `glass-capture-....zip` that appears next to the logs. It already contains both logs |

Always mention which version you use (add-on or standalone), the aircraft, your DLSS/FSR mode, Frame Generation on/off
and other mods (OptiScaler, ReShade, RenoDX, MFG unlocks, OptiShade...).

**Using OptiShade?** Report problems with the fixed screens here, and problems with OptiShade itself (installing, its
presets and effects, updates) on [OptiShade's issues page](https://github.com/GamingWithGravy/OptiShade/issues). Not sure which? If the problem goes
away when you toggle the fix off with **Ctrl+Shift+F9**, it belongs here.

The **logs** are written every session: `glass-cockpit-dlss-fix.log` next to the add-on or DLL, and the previous
session as `glass-cockpit-dlss-fix.old.log` (if the sim crashed or you restarted it, the problem is in the `.old.log`).
The **capture** saves a few frames as pictures, cropped to the cockpit screens: what the sim drew, the upscaled frame
before and after the fix, and the frames sent to your monitor, plus a summary. It is only made when you press it and
costs nothing otherwise; nothing is uploaded anywhere.

The log lists which version it is, your GPU, driver, ReShade version (add-on), the DLSS/FSR files the sim loaded and
other mods, followed by milestones in order:

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
| Add-on: no "Glass Cockpit DLSS Fix" tab in the ReShade menu | The add-on isn't loaded. If the ReShade menu has no **Add-ons** tab, reinstall ReShade with the *full add-on support* download. Check that the file is next to the ReShade DLL, and that **Settings → Add-on search path** in the ReShade menu points to that folder. |
| Standalone: no `glass-cockpit-dlss-fix.log` appears | The DLL isn't loaded. Check that it is named exactly `dxgi.dll` and is in the same folder as `FlightSimulator2024.exe` (Windows may hide the `.dll` extension: it must not be `dxgi.dll.dll`) |
| Standalone: log says the device was not seen, or "loaded after the sim created its graphics device" | The DLL was loaded too late, usually because it was loaded as `.asi` or another mod renamed it. Install it as `dxgi.dll` |
| Nothing changes, no green tint | Check that DLSS or FSR is selected (add-on: that ReShade is the *full add-on support* build and the file is next to the ReShade DLL). Then read the tab or status file, and the log. |
| Picture freezes or turns black with Frame Generation on (fine when alt-tabbed) | Update to 0.8.1 or newer (0.8.0 only) |
| Sim freezes when a Fenix aircraft loads, with RenoDX's DLSS add-on installed | Remove `renodx-dlss.addon64`; see [Other mods](#other-mods) |
| "Only DLSS Frame Generation is running" | Frame Generation is on but Anti-Aliasing isn't DLSS or FSR (e.g. TAA). Set Anti-Aliasing to DLSS (any mode, DLAA too) or FSR; Frame Generation can stay on |
| Some screens of an aircraft stay normal | Look at those screens in the cockpit, press **Ctrl+Shift+F12** (writes a short diagnostic to the log), then send both logs and the aircraft name |
| Screen lines look jagged / too soft | Adjust Line smoothing |
| Dashed light line along the edge of a screen's picture (e.g. FSS E-Jets) | Update to 0.8.3 or newer and keep **UV seam fix** on (`seam_fix=1`). If a line remains, try **Detail clamp** (can add some flicker on DLSS Balanced/Performance) |
| Dark screens drop out of the fix (green tint goes away) in strong sun | Turn on **Dark screen fix** (`dark_screen_fix=1`) |
| Fixed screens shimmer with Frame Generation on (steady with it off) | Update to 1.0.0 or newer |
| Screens change colour for a moment while the camera or aircraft moves (most often with Frame Generation) | Update to 0.8.3 or newer (1.0.0 or newer in VR) |
| A screen is drawn over something in front of it (a sidestick, a hand, the yoke) | Make sure **Occlusion check** is on (`occlusion_check=1`, the default) |
| Screen edges flicker or shimmer, or show a sliver of something that isn't there | Set **Edge trim** to 50 (`edge_trim=50`; up to 150 for aircraft with stronger edge problems) |
| No log after flying a Fenix aircraft, or the log is called `glass-cockpit-dlss-fix-<number>.log` | Fixed in 1.0.0: a helper program the sim starts with some aircraft (e.g. Fenix) kept the log file open after the sim quit, so the next session couldn't write one. 1.0.0 logs under a numbered name when that happens; send that file |
| Screen text shimmers when the camera is still | Turn on **Flip jitter direction** (`jitter_positive=0`). If it's already on (the tab or status file says "Advanced settings changed"), turn it off again (in the tab: **Reset to defaults**) |
| Screens flicker in brightness (G-Sync / FreeSync monitor) | VRR flicker from a changing frame rate, most visible on dark screens. Turn on V-Sync, or cap the frame rate a few fps below your refresh rate |
| Crash at startup | Remove the add-on or `dxgi.dll`, start once, put it back, then send both logs |

## How the fix works

DLSS and FSR build each frame from the history of previous frames, guided by motion vectors. A cockpit screen does
not move, but its *content* does, so the upscaler keeps blending old numbers into new ones. The game's own hint for
this (the animated-texture mask) isn't enough, and no DLSS preset (J, K, M) fixes it.

The fix uses the same idea that DLSS mods for other games use for HUDs: **draw the screen content after the
upscaler**.

1. When the sim draws a cockpit screen, the fix draws the same mesh once more into a small private map that records
   which part of which screen image lands on each pixel.
2. Right after DLSS or FSR, it samples the sim's own clean screen image at full output resolution (with the frame's
   camera jitter removed) and writes it over the upscaled result, so the moving content never goes through the
   upscaler's history.
3. Brightness is matched to the sim every frame, so night lighting and the brightness knobs still work. Dirt, sun
   glare and shadows on the glass are taken from the upscaled image itself, limited to what this frame actually
   shows, so they stay sharp while trails of old screen content are removed.

In VR the sim draws every view (both eyes, plus the sharp centre views with foveated rendering) in one go and then
upscales them one by one; the fix records each view separately and learns from the sim which upscale belongs to which
view.

To see what the sim draws, the fix hooks the few Direct3D 12 functions it needs: device and pipeline creation, root
signatures, views, the command list calls that bind pipelines, descriptor tables and render targets, the draws, and the
swapchain's present. Everything else goes straight to Windows. The two versions differ only in how the fix gets into
the sim: ReShade loads the add-on and tells it about the sim's graphics device and swapchain; the standalone version is
loaded by Windows as the sim's `dxgi.dll` (it passes every call on to Windows' own `dxgi.dll`) and sees the device being
created.

It works at any resolution and in any quality mode, and costs one extra draw per screen plus a few small GPU passes:
no measurable FPS difference with the fix on or off in testing.

## Known limitations

- A screen whose picture the fix can't redraw is left to DLSS/FSR, so it looks like stock.
- A thin band at each screen's edge (about one render pixel) still comes from the upscaler (Edge fade and Edge
  stretcher under Experimental fixes can help on some aircraft).
- Anything that passes in front of a screen after the cockpit is drawn (e.g. hands in VR) is left to the upscaler by
  the Occlusion check (on by default); with it off, the screen is drawn over it.
- Standalone: no in-game menu. Settings are in the `.ini` file, status in the status file.
- Sim updates that change the cockpit display shader stop the fix (the sim then just looks like stock DLSS/FSR)
  until it is updated.

## Building from source

The full source is in this repository. See [BUILDING.md](BUILDING.md) to build both versions yourself.

## License

From 1.0.0: GPL-3.0-only with additional attribution terms, see [NOTICE.md](NOTICE.md) and [LICENSE](LICENSE).
Versions up to 0.8.3 remain MIT. Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Official downloads and redistribution

The only official downloads are the [GitHub releases](https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix/releases)
(each release lists the SHA-256 checksum of the add-on file). OptiShade bundles the official add-on with explicit permission (see [NOTICE.md](NOTICE.md)).

You may redistribute and modify the add-on under the GPL-3 terms. In short, for builds that contain 1.0.0 or later code:

- Keep the licence, NOTICE.md and the credit "Glass Cockpit DLSS Fix by IFreemz" with the link to this repository.
- If you change the code or build it into another program, that program must be released under GPL-3 as a whole,
  with its source available.
- Mark modified or repackaged builds as unofficial, under a different name or version, and handle their support
  yourself.

Requests (not licence conditions): please also credit the project visibly on your own download page and in your
program's interface, link to the official releases, and let me know about your build so I can point people to the
right place for support.

If an antivirus flags a download, check that it came from the official releases and that its SHA-256 matches.

**Using AI agents or other automated tools to build on this project** (a wrapper, an installer, a bundle): see
[AGENTS.md](AGENTS.md) for the licence terms in short.

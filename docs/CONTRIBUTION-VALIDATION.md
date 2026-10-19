# Contribution scope and offline reproduction

Unofficial contribution build, based on **Glass Cockpit DLSS Fix by IFreemz**:
https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix.
Base: `e8a26fa9b99dba6b74213e999f871da933384da0` (standalone v1.0.1).
The original defaults, licenses and notices are retained. Nothing here installs into MSFS or changes OFXR settings.

## First accepted dark screen under glare

The fitting pass can skip a screen that only qualifies in the subsequent solve pass. The composite then has no fresh
residual samples, but replaces the valid upscaler image using an inadequate glass estimate. The guard
leaves the upscaler result for that frame and resumes correction once residual samples exist.

Reproduction uses a mostly dark display, sparse light lines, strong spatial glare, and Dark screen fix enabled.
`--first-glare` additionally checks normally informational warmup frames, and verifies correction resumes on a
later scrolling frame. Test both desktop and stereo, with Dark screen fix off and on.

| First accepted screen | Published v1.0.1 | Rebuilt unmodified source | Same source with residual guard |
|---|---:|---:|---:|
| Desktop | 0.587132 | 0.587132 | 0.007674 |
| Stereo | 0.586385 | 0.586385 | 0.007680 |

Review follow-up: the guard uses **total usable weight after the outline fallback**, not a count of non-outline
samples. Small/high-contrast displays can legitimately have only outline samples. A new GPU regression compiles the
actual production residual-compositing block with synthetic empty, outline-only, regular and mixed samples. The
original contribution fails the outline-only case (leaves the upscaler image); the corrected guard passes all four.
Only texture inputs are substituted in that test; the fallback and guard logic are taken directly from the shader.

Values are normalized interior error; the existing quality limit is 0.02. Those A/B measurements were made on an
RTX 4090. This demonstrates one experimental-setting rendering regression, **not** a startup crash or a claim that
all cockpit displays exhibit it. Dark screen fix remains off by default.

## Settings saved between polls

The unit test uses the actual settings code without starting engine threads/hooks. It writes an external INI edit,
then immediately uses a native setter before the one-second reload. It checks the untouched field survives, clamps
use the newly loaded Extra setting, enabling invalidates recorded bindings, a stale experimental UI snapshot changes
only its edited fields, and Reset still resets every experimental field.

This merges edits already present when a native operation begins. It does not promise atomic cross-process editing
if another writer changes the file during the subsequent save; same-field edits remain last-writer-wins.

## Green-tint control

[External-controls protocol](EXTERNAL-CONTROLS.md). The unit test uses real child processes for on/off in both
directions, controller crash/exit and reconnect, including a native on-click after owner exit but before the next
poll (with the previous external state both off and on). The original contribution fails that ordering; clearing
stale attachment in the native setter preserves the new request. `--external-tint` uses the actual engine worker and rendered pixels
to check on/off/detach in desktop and stereo. Normal operation still needs no external controller.

## Commands

From the repository root, with the MSVC/SDK prerequisites in `BUILDING.md`:

```powershell
powershell -NoProfile -File scripts/build.ps1 -Test
powershell -NoProfile -File scripts/test.ps1
python -B tests/motion_quality.py
```

Verified locally on 2026-10-18: both variants compiled and every standalone check below passed on an RTX 4090.

The build compiles the standalone DLL and ReShade add-on. The tests run two unit suites, a four-case GPU residual regression, four normal/copied-vtable
Present/Present1 startup probes, the existing 16 graphics scenarios and capture check, two tint-rendering scenarios,
and four dark-glare scenarios. The optional Python check asserts 13 motion, occlusion, brightness, history and
texture-replacement measurements with finite values and quality limits.

The ReShade add-on rendering suite requires a real ReShade DLL with add-on support, supplied via `-ReShadeDll` as
described in `BUILDING.md`. If absent, the script reports that add-on runtime checks were skipped. Compiling the
add-on and testing shared code is not a substitute for those checks. Offline results are not an MSFS/VR flight test.

## Review follow-ups, deliberately not changed here

- **GPU diagnostic completion:** capture, cost timing and pixel-probe readbacks are delayed by presents. Queue fences
  would prove completion on the submitting queue; elapsed frames do not, especially if a queue stalls. The ordinary
  capture fixture passes but does not simulate that failure. A fence/resource-lifetime change needs its own tests.
- **Loader-lock initialization:** `DllMain` starts filesystem/settings/thread work. Deferral needs startup-order tests
  to avoid missing early D3D12 device creation. Current startup probes have not reproduced a deadlock.
- **Multiple Present implementations:** the current single trampoline pair handles the tested normal/copied tables.
  Supporting distinct native/proxy implementations and retrying partial hook-install failures needs targeted
  wrapper tests; it is separate from the already-handled copied-table lookup issue.

These observations are review topics, not proven causes of a user's crash. They are included to invite upstream
feedback without mixing unverified hook/lifetime rewrites into the three tested changes.

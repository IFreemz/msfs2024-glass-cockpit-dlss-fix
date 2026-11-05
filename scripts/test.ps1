# Runs the offline test (build with scripts\build.ps1 -Test first). Needs an NVIDIA GPU with SM 6.6.
# Writes build\test\overlay-*.png and build\test\glass-cockpit-dlss-fix.log.
$ErrorActionPreference = 'Stop'
$test = Join-Path (Split-Path $PSScriptRoot) 'build\test'
Push-Location $test
try {
    & .\FlightSimulator2024.exe
    if ($LASTEXITCODE) { throw "Offline test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --fsr
    if ($LASTEXITCODE) { throw "Offline FSR test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --vr
    if ($LASTEXITCODE) { throw "Offline VR test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --screen-t0
    if ($LASTEXITCODE) { throw "Offline screen-in-t0 variant test failed ($LASTEXITCODE)" }
    # ReShade unloads and reloads the add-on when another device comes and goes first (OptiScaler does this).
    & .\FlightSimulator2024.exe --reload
    if ($LASTEXITCODE) { throw "Offline add-on reload test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --vr --screen-t0
    if ($LASTEXITCODE) { throw "Offline VR screen-in-t0 variant test failed ($LASTEXITCODE)" }
    # Something in front of the screen (a hand, the yoke), in VR at its own spot in each eye, with the depth as the sim gives
    # it there (one of the two calls gets both eyes' depth in one texture): the fix must leave it to the upscaler in each eye.
    & .\FlightSimulator2024.exe --occluder
    if ($LASTEXITCODE) { throw "Offline occlusion test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --vr --occluder
    if ($LASTEXITCODE) { throw "Offline VR occlusion test failed ($LASTEXITCODE)" }
    # Screen textures identified by descriptor tracking only (how AMD/Intel GPUs run).
    & .\FlightSimulator2024.exe --track
    if ($LASTEXITCODE) { throw "Offline descriptor tracking test failed ($LASTEXITCODE)" }
    # A virtual painting drawn by a translucent material the fix doesn't know (Fenix A320 readouts, "FCU readouts: New").
    & .\FlightSimulator2024.exe --painted
    if ($LASTEXITCODE) { throw "Offline painted screen test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --painted --track
    if ($LASTEXITCODE) { throw "Offline painted screen descriptor tracking test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --painted --depthless
    if ($LASTEXITCODE) { throw "Offline painted screen without depth test failed ($LASTEXITCODE)" }
    # The same as a decal with no render target, written through a rasterizer-ordered UAV (how the sim draws those readouts).
    & .\FlightSimulator2024.exe --painted --rov
    if ($LASTEXITCODE) { throw "Offline painted decal test failed ($LASTEXITCODE)" }
    # The painting drawn every frame from a copy of a gauge, both black again before the upscaler (the Fenix A320's readouts):
    # the fix must copy the painting while the decal reads it.
    & .\FlightSimulator2024.exe --painted --rov --source
    if ($LASTEXITCODE) { throw "Offline painted decal from a gauge test failed ($LASTEXITCODE)" }
    # The same with the painting's picture in alpha only, its colour black (how the Fenix's readouts painting holds it),
    # lit across by a shadow edge, as the sun lights the Fenix's FCU.
    & .\FlightSimulator2024.exe --painted --rov --source --alpha
    if ($LASTEXITCODE) { throw "Offline painted decal in alpha only test failed ($LASTEXITCODE)" }
    # The same with the painting made anew near the end, as the Fenix makes its readouts painting anew every few seconds to a
    # minute: the fix must read the new one in alpha only at once, not leave it to the upscaler until it finds that out again.
    & .\FlightSimulator2024.exe --painted --rov --source --alpha --recreate
    if ($LASTEXITCODE) { throw "Offline painted decal made anew test failed ($LASTEXITCODE)" }
    # The same tinted amber by its material, as the Fenix's readouts are: one gain for all channels can't explain that
    # picture well, so the fix must score it per channel or leave a good match to the upscaler.
    & .\FlightSimulator2024.exe --painted --rov --source --alpha --amber
    if ($LASTEXITCODE) { throw "Offline amber painted decal test failed ($LASTEXITCODE)" }
    # Strong sun on the matched readout, the panel under it sunlit across a shadow edge (the Fenix's FCU and clock in the
    # sun): every frame the fix draws must be as good as the upscaler's picture and not flicker, else it is left to it.
    & .\FlightSimulator2024.exe --painted --rov --source --alpha --sun
    if ($LASTEXITCODE) { throw "Offline painted decal in strong sun test failed ($LASTEXITCODE)" }
    # The same in milder sun, amber: the readout still matches well enough to be drawn (the Fenix's FCU and clock in part
    # sun), and it must not flicker more than a little beyond the upscaler's picture (its shadows shimmered when its glass
    # layer was taken afresh every frame).
    & .\FlightSimulator2024.exe --painted --rov --source --alpha --amber --mild-sun
    if ($LASTEXITCODE) { throw "Offline amber painted decal in mild sun test failed ($LASTEXITCODE)" }
    # A painted readout drawn only where its content changed lately (it is a lit material the brightness match can only
    # approximate; still, the upscaler's picture of it is right): the upscaler's while still, drawn while it changes and for
    # the hold after, then faded back. The other painted runs draw it in every frame, to test how it is drawn.
    & .\FlightSimulator2024.exe --painted --rov --source --alpha --gated
    if ($LASTEXITCODE) { throw "Offline painted decal drawn where it changed test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --fsr --vr --track
    if ($LASTEXITCODE) { throw "Offline FSR VR descriptor tracking test failed ($LASTEXITCODE)" }
    # Another mod (the RTX 40 MFG unlock) has redirected DLSS evaluate in a way that can't be hooked directly.
    & .\FlightSimulator2024.exe --hotpatch
    if ($LASTEXITCODE) { throw "Offline redirected DLSS test failed ($LASTEXITCODE)" }
    # OptiScaler answers the sim's DLSS (or FSR) with FSR from its own FidelityFX files (AMD GPUs, or FSR chosen in OptiScaler).
    & .\FlightSimulator2024.exe --fsr --optiscaler-fsr
    if ($LASTEXITCODE) { throw "Offline OptiScaler FSR test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --fsr --optiscaler-fsr --vr
    if ($LASTEXITCODE) { throw "Offline OptiScaler FSR VR test failed ($LASTEXITCODE)" }
    # DLSS selected in the sim, OptiScaler answers it with FSR (AMD GPUs, or FSR chosen in OptiScaler on NVIDIA).
    & .\FlightSimulator2024.exe --ngx-proxy
    if ($LASTEXITCODE) { throw "Offline DLSS answered by OptiScaler test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --ngx-proxy --vr
    if ($LASTEXITCODE) { throw "Offline DLSS answered by OptiScaler VR test failed ($LASTEXITCODE)" }
    # OptiScaler answers the sim's FSR with DLSS inside the FSR dispatch: the fix must be drawn once per view.
    # Another mod (RenoDX's DLSS add-on) runs an extra DLSS pass at the same render size first: the fix goes on the sim's own.
    & .\FlightSimulator2024.exe --extra-pass
    if ($LASTEXITCODE) { throw "Offline extra DLSS pass test failed ($LASTEXITCODE)" }
    # DLSS Frame Generation: the sim runs the upscaler three times per frame into the same output; the fix must be on the last.
    & .\FlightSimulator2024.exe --fg-repeat
    if ($LASTEXITCODE) { throw "Offline frame generation repeat test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --fsr --vr --nested
    if ($LASTEXITCODE) { throw "Offline nested upscaler VR test failed ($LASTEXITCODE)" }
    # The screen capture tool writes its pictures (and what the brightness match read); the capture folders it makes are removed.
    & .\FlightSimulator2024.exe --motion --only still-edges-on --capture
    if ($LASTEXITCODE) { throw "Offline screen capture test failed ($LASTEXITCODE)" }
    Get-ChildItem -Filter 'glass-capture-*' | Remove-Item -Recurse -Force
} finally { Pop-Location }

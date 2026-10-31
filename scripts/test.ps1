# Runs the offline test (build with scripts\build.ps1 -Test first). Needs an NVIDIA GPU with SM 6.6.
# Runs every case in build\test (dxgi.dll) and, if it was built, build\test-addon (the ReShade add-on under ReShade).
# Each writes overlay-*.png and glass-cockpit-dlss-fix.log in its folder.
$ErrorActionPreference = 'Stop'
$build = Join-Path (Split-Path $PSScriptRoot) 'build'
$cases = @(
    @{ Args = @(); Name = 'Offline test' }
    @{ Args = @('--fsr'); Name = 'Offline FSR test' }
    @{ Args = @('--vr'); Name = 'Offline VR test' }
    @{ Args = @('--screen-t0'); Name = 'Offline screen-in-t0 variant test' }
    # Another device is created and released before the sim's own (OptiScaler does this at startup). ReShade then
    # unloads the add-on and loads it again.
    @{ Args = @('--reload'); Name = 'Offline early device test' }
    @{ Args = @('--vr', '--screen-t0'); Name = 'Offline VR screen-in-t0 variant test' }
    # Screen textures identified by descriptor tracking only (how AMD/Intel GPUs run).
    @{ Args = @('--track'); Name = 'Offline descriptor tracking test' }
    # A virtual painting drawn by a translucent material the fix doesn't know (Fenix A320 readouts, "FCU readouts: New").
    @{ Args = @('--painted'); Name = 'Offline painted screen test' }
    @{ Args = @('--painted', '--track'); Name = 'Offline painted screen descriptor tracking test' }
    @{ Args = @('--painted', '--depthless'); Name = 'Offline painted screen without depth test' }
    # The same as a decal with no render target, written through a rasterizer-ordered UAV (how the sim draws those readouts).
    @{ Args = @('--painted', '--rov'); Name = 'Offline painted decal test' }
    # The painting drawn every frame from a copy of a gauge, both black again before the upscaler (the Fenix A320's readouts):
    # the fix must copy the painting while the decal reads it.
    @{ Args = @('--painted', '--rov', '--source'); Name = 'Offline painted decal from a gauge test' }
    # The same with the painting's picture in alpha only, its colour black: the alpha is black laid over the lit backing,
    # the digits are where it is low (how the Fenix's readouts painting holds it).
    @{ Args = @('--painted', '--rov', '--source', '--alpha'); Name = 'Offline painted decal in alpha only test' }
    @{ Args = @('--fsr', '--vr', '--track'); Name = 'Offline FSR VR descriptor tracking test' }
    # Another mod (the RTX 40 MFG unlock) has redirected DLSS evaluate in a way that can't be hooked directly.
    @{ Args = @('--hotpatch'); Name = 'Offline redirected DLSS test' }
    # OptiScaler answers the sim's DLSS (or FSR) with FSR from its own FidelityFX files (AMD GPUs, or FSR chosen in OptiScaler).
    @{ Args = @('--fsr', '--optiscaler-fsr'); Name = 'Offline OptiScaler FSR test' }
    @{ Args = @('--fsr', '--optiscaler-fsr', '--vr'); Name = 'Offline OptiScaler FSR VR test' }
    # DLSS selected in the sim, OptiScaler answers it with FSR (AMD GPUs, or FSR chosen in OptiScaler on NVIDIA).
    @{ Args = @('--ngx-proxy'); Name = 'Offline DLSS answered by OptiScaler test' }
    @{ Args = @('--ngx-proxy', '--vr'); Name = 'Offline DLSS answered by OptiScaler VR test' }
    # Another mod (RenoDX's DLSS add-on) runs an extra DLSS pass at the same render size first: the fix goes on the sim's own.
    @{ Args = @('--extra-pass'); Name = 'Offline extra DLSS pass test' }
    # DLSS Frame Generation: the sim runs the upscaler three times per frame into the same output; the fix must be on the last.
    @{ Args = @('--fg-repeat'); Name = 'Offline frame generation repeat test' }
    # OptiScaler answers the sim's FSR with DLSS inside the FSR dispatch: the fix must be drawn once per view.
    @{ Args = @('--fsr', '--vr', '--nested'); Name = 'Offline nested upscaler VR test' }
    # The screen capture tool writes its pictures (and what the brightness match read); the capture folders it makes are
    # removed afterwards.
    @{ Args = @('--motion', '--only', 'still-edges-on', '--capture'); Name = 'Offline screen capture test' }
)
# The standalone as winmm.dll runs the first three cases: only how it is loaded differs.
$hosts = @(@{ Folder = 'test'; Name = 'dxgi.dll' }, @{ Folder = 'test-winmm'; Name = 'standalone as winmm.dll'; Cases = 3 },
    @{ Folder = 'test-addon'; Name = 'ReShade add-on' }) |
    Where-Object { Test-Path -LiteralPath (Join-Path $build "$($_.Folder)\FlightSimulator2024.exe") }
if (-not $hosts) { throw 'No test host: build with scripts\build.ps1 -Test first' }
foreach ($testHost in $hosts) {
    Write-Host "== $($testHost.Name) (build\$($testHost.Folder))"
    Push-Location (Join-Path $build $testHost.Folder)
    try {
        foreach ($case in $(if ($testHost.Cases) { $cases | Select-Object -First $testHost.Cases } else { $cases })) {
            $arguments = $case.Args
            & .\FlightSimulator2024.exe @arguments
            if ($LASTEXITCODE) { throw "$($case.Name) failed with the $($testHost.Name) ($LASTEXITCODE)" }
        }
        Get-ChildItem -Filter 'glass-capture-*' | Remove-Item -Recurse -Force
    } finally { Pop-Location }
}
if (-not ($hosts | Where-Object { $_.Folder -eq 'test-addon' })) { Write-Warning 'The ReShade add-on was not tested (no build\test-addon: see scripts\build.ps1 -ReShadeDll)' }

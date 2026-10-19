# Runs the offline test (build with scripts\build.ps1 -Test first). Needs an NVIDIA GPU with SM 6.6.
# Runs every case in build\test (dxgi.dll) and, if it was built, build\test-addon (the ReShade add-on under ReShade).
# Each writes overlay-*.png and glass-cockpit-dlss-fix.log in its folder.
$ErrorActionPreference = 'Stop'
$build = Join-Path (Split-Path $PSScriptRoot) 'build'
# Unit tests never start the graphics engine or touch the simulator's installation.
& (Join-Path $build 'unit\external_tint.exe')
if ($LASTEXITCODE) { throw 'External tint protocol test failed' }
$settingsDirectory = Join-Path $build ('unit\settings-' + [guid]::NewGuid().ToString('N'))
& (Join-Path $build 'unit\settings_merge.exe') $settingsDirectory
if ($LASTEXITCODE) { throw 'Settings merge test failed' }
$resolvedSettings = [IO.Path]::GetFullPath($settingsDirectory)
if (-not $resolvedSettings.StartsWith([IO.Path]::GetFullPath((Join-Path $build 'unit')) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
Remove-Item -LiteralPath $resolvedSettings -Recurse -Force
foreach ($mode in 'normal', 'clone', 'normal-present1', 'clone-present1') {
    & (Join-Path $build 'startup\FlightSimulator2024.exe') (Join-Path $build 'startup\dxgi.dll') $mode
    if ($LASTEXITCODE) { throw "Standalone startup probe failed: $mode" }
}
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
    @{ Args = @('--external-tint'); Name = 'Offline external tint rendering test' }
    @{ Args = @('--external-tint', '--vr'); Name = 'Offline external tint VR rendering test' }
)
$hosts = @(@{ Folder = 'test'; Name = 'dxgi.dll' }, @{ Folder = 'test-addon'; Name = 'ReShade add-on' }) |
    Where-Object { Test-Path -LiteralPath (Join-Path $build "$($_.Folder)\FlightSimulator2024.exe") }
if (-not $hosts) { throw 'No test host: build with scripts\build.ps1 -Test first' }
foreach ($testHost in $hosts) {
    Write-Host "== $($testHost.Name) (build\$($testHost.Folder))"
    Push-Location (Join-Path $build $testHost.Folder)
    try {
        foreach ($case in $cases) {
            $arguments = $case.Args
            & .\FlightSimulator2024.exe @arguments
            if ($LASTEXITCODE) { throw "$($case.Name) failed with the $($testHost.Name) ($LASTEXITCODE)" }
        }
        # Dark, sparse display under strong glare: even the first accepted frame must keep its glass.
        # The regular fixture runs with author defaults; these four checks also cover Dark screen fix ON.
        $ini = Join-Path (Get-Location).Path 'glass-cockpit-dlss-fix.ini'
        $previousIni = if (Test-Path -LiteralPath $ini) { [IO.File]::ReadAllBytes($ini) } else { $null }
        $previousEnv = $env:GLASS_FIXTURE_SETTINGS
        try {
            $env:GLASS_FIXTURE_SETTINGS = '1'
            foreach ($dark in 0, 1) {
                [IO.File]::WriteAllText($ini, "[overlay]`nenabled=1`nline_smoothing=35`n[experimental]`ndark_screen_fix=$dark`n")
                foreach ($view in @(@{ Args = @('--first-glare') }, @{ Args = @('--first-glare', '--vr') })) {
                    $arguments = $view.Args
                    & .\FlightSimulator2024.exe @arguments
                    if ($LASTEXITCODE) { throw "First-recognition glare test failed (dark=$dark, $arguments)" }
                }
            }
        } finally {
            $env:GLASS_FIXTURE_SETTINGS = $previousEnv
            if ($null -ne $previousIni) { [IO.File]::WriteAllBytes($ini, $previousIni) }
            elseif (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini }
        }
        $testRoot = [IO.Path]::GetFullPath((Get-Location).Path) + '\'
        Get-ChildItem -Directory -Filter 'glass-capture-*' | ForEach-Object {
            if (-not ([IO.Path]::GetFullPath($_.FullName).StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase))) { throw 'Unsafe capture cleanup path' }
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
    } finally { Pop-Location }
}
if (-not ($hosts | Where-Object { $_.Folder -eq 'test-addon' })) { Write-Warning 'The ReShade add-on was not tested (no build\test-addon: see scripts\build.ps1 -ReShadeDll)' }

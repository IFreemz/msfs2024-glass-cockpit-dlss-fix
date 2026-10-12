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
    # Screen textures identified by descriptor tracking only (how AMD/Intel GPUs run).
    & .\FlightSimulator2024.exe --track
    if ($LASTEXITCODE) { throw "Offline descriptor tracking test failed ($LASTEXITCODE)" }
    & .\FlightSimulator2024.exe --fsr --vr --track
    if ($LASTEXITCODE) { throw "Offline FSR VR descriptor tracking test failed ($LASTEXITCODE)" }
} finally { Pop-Location }

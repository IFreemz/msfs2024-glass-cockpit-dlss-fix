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
} finally { Pop-Location }

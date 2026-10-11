# Builds and zips a release: dist\glass-cockpit-dlss-fix-<version>.zip (add-on, README, LICENSE, notices).
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
if ($LASTEXITCODE) { throw 'Build failed' }
$source = Get-Content -LiteralPath (Join-Path $root 'src\glass_cockpit_dlss_fix.cpp') -Raw
if ($source -notmatch "overlay_version = `"$([regex]::Escape($Version))`"") { throw "src version does not match $Version" }
$stage = Join-Path $root "dist\glass-cockpit-dlss-fix-$Version"
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build\glass-cockpit-dlss-fix.addon64') -Destination $stage -Force
foreach ($file in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md') { Copy-Item -LiteralPath (Join-Path $root $file) -Destination $stage -Force }
Copy-Item -LiteralPath (Join-Path $root 'third_party\minhook\LICENSE.txt') -Destination (Join-Path $stage 'LICENSE-MinHook.txt') -Force
$zip = "$stage.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "Created $zip"

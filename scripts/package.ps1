# Builds a release: dist\glass-cockpit-dlss-fix.addon64, the single file attached to the GitHub release.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$source = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
if ($source -notmatch "GLASS_VERSION_STRING `"$([regex]::Escape($Version))`"") { throw "src\version.h does not match $Version" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
if ($LASTEXITCODE) { throw 'Build failed' }
$dist = Join-Path $root 'dist'
if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build\glass-cockpit-dlss-fix.addon64') -Destination $dist
Write-Host "Created $(Join-Path $dist 'glass-cockpit-dlss-fix.addon64') ($Version)"

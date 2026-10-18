# Builds a release: the files attached to the GitHub release, dist\glass-cockpit-dlss-fix.addon64 (the ReShade add-on)
# and dist\glass-cockpit-dlss-fix-standalone.zip (the standalone dxgi.dll, which users drop next to the sim's exe, with
# its README.txt, LICENSE and NOTICE.md).
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

$staging = Join-Path $dist 'standalone'
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build\dxgi.dll') -Destination $staging
Copy-Item -LiteralPath (Join-Path $root 'docs\standalone-README.txt') -Destination (Join-Path $staging 'README.txt')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE'), (Join-Path $root 'NOTICE.md') -Destination $staging
$zip = Join-Path $dist 'glass-cockpit-dlss-fix-standalone.zip'
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip
Remove-Item -LiteralPath $staging -Recurse -Force
Write-Host "Created $zip ($Version)"
foreach ($file in Get-ChildItem -LiteralPath $dist -File) { Write-Host "SHA-256 $($file.Name): $((Get-FileHash -LiteralPath $file.FullName).Hash.ToLower())" }

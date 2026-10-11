# Builds build\glass-cockpit-dlss-fix.addon64 with the MSVC and Windows SDK tools found by vswhere.
#   -Test     also builds the offline test host in build\test (run scripts\test.ps1 afterwards)
#   -ReShadeDll path to a ReShade64.dll/.asi with add-on support, copied into the test host (default: installed MSFS copy)
param([switch]$Test, [string]$ReShadeDll = 'C:\Program Files (x86)\Steam\steamapps\common\Microsoft Flight Simulator 2024\ReShade64.dll')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio with the C++ x64 tools is required' }
$tools = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$compiler = Join-Path $tools.FullName 'bin\Hostx64\x64\cl.exe'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$dxc = Join-Path $sdkRoot "bin\$($sdk.Name)\x64\dxc.exe"
$env:INCLUDE = @((Join-Path $tools.FullName 'include'), (Join-Path $sdk.FullName 'ucrt'), (Join-Path $sdk.FullName 'shared'), (Join-Path $sdk.FullName 'um'), (Join-Path $sdk.FullName 'winrt')) -join ';'
$env:LIB = @((Join-Path $tools.FullName 'lib\x64'), (Join-Path $sdkRoot "Lib\$($sdk.Name)\ucrt\x64"), (Join-Path $sdkRoot "Lib\$($sdk.Name)\um\x64")) -join ';'
$env:PATH = (Split-Path $compiler) + ';' + $env:PATH

$minhook = Join-Path $root 'third_party\minhook'
$reshadeInclude = Join-Path $root 'third_party\reshade\include'
if (-not (Test-Path (Join-Path $reshadeInclude 'reshade.hpp'))) { throw 'Submodules missing: run "git submodule update --init"' }
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
Push-Location $build
try {
    & $compiler /nologo /c /TC /W3 /MD /O2 (Join-Path $minhook 'src\buffer.c') (Join-Path $minhook 'src\hook.c') (Join-Path $minhook 'src\trampoline.c') (Join-Path $minhook 'src\hde\hde64.c')
    if ($LASTEXITCODE) { throw 'MinHook compilation failed' }
    # Shaders -> one generated header (16 display-slot variants of the map shader + 5 compute passes).
    $headers = @()
    foreach ($slot in 0..15) {
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -WX -O3 -Fh "map_ps_$slot.h" -Vn "map_ps_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "Map shader $slot failed" }
        $headers += "map_ps_$slot.h"
    }
    foreach ($pass in 'zero', 'fit', 'solve', 'composite', 'clear') {
        & $dxc -T cs_6_6 -HV 2021 -E $pass -WX -O3 -Fh "cs_$pass.h" -Vn "cs_$pass" (Join-Path $root 'src\overlay.hlsl')
        if ($LASTEXITCODE) { throw "Compute pass $pass failed" }
        $headers += "cs_$pass.h"
    }
    Set-Content -LiteralPath 'overlay_shaders.h' -Value (@('#pragma once') + ($headers | ForEach-Object { "#include `"$_`"" }))
    & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /LD /I $build /I $reshadeInclude /I (Join-Path $minhook 'include') (Join-Path $root 'src\glass_cockpit_dlss_fix.cpp') (Join-Path $root 'src\vtable_slots.cpp') buffer.obj hook.obj trampoline.obj hde64.obj /link d3d12.lib dxgi.lib version.lib /OUT:glass-cockpit-dlss-fix.addon64
    if ($LASTEXITCODE) { throw 'Add-on compilation failed' }

    if ($Test) {
        # Fake FlightSimulator2024.exe + fake _nvngx.dll + ReShade + the add-on, with stand-in display shaders.
        $testHost = Join-Path $build 'test'
        New-Item -ItemType Directory -Path $testHost -Force | Out-Null
        & $dxc -T vs_6_6 -HV 2021 -E main -WX -O3 -Fo (Join-Path $testHost 'fixture_vs.cso') (Join-Path $root 'tests\fixture_vs.hlsl')
        if ($LASTEXITCODE) { throw 'Test vertex shader failed' }
        & $dxc -T ps_6_6 -HV 2021 -E main -D REFERENCE=0 -WX -O3 -Fo (Join-Path $testHost 'fixture_ps.cso') (Join-Path $root 'tests\fixture_ps.hlsl')
        if ($LASTEXITCODE) { throw 'Test pixel shader failed' }
        & $dxc -T ps_6_6 -HV 2021 -E main -D REFERENCE=1 -WX -O3 -Fo (Join-Path $testHost 'reference_ps.cso') (Join-Path $root 'tests\fixture_ps.hlsl')
        if ($LASTEXITCODE) { throw 'Reference shader failed' }
        & $compiler /nologo /W4 /WX /MD /O2 /LD (Join-Path $root 'tests\fake_nvngx.cpp') /link /OPT:NOICF /DEF:"$root\tests\fake_nvngx.def" /OUT:"$testHost\_nvngx.dll"
        if ($LASTEXITCODE) { throw 'Fake NGX compilation failed' }
        & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /Fe:"$testHost\FlightSimulator2024.exe" (Join-Path $root 'tests\fixture.cpp') /link d3d12.lib dxgi.lib ole32.lib windowscodecs.lib
        if ($LASTEXITCODE) { throw 'Test host compilation failed' }
        Copy-Item -LiteralPath 'glass-cockpit-dlss-fix.addon64' -Destination $testHost -Force
        if (-not (Test-Path -LiteralPath $ReShadeDll)) { throw "ReShade not found at $ReShadeDll (pass -ReShadeDll <path to ReShade64.dll/.asi with add-on support>)" }
        Copy-Item -LiteralPath $ReShadeDll -Destination (Join-Path $testHost 'ReShade64.asi') -Force
    }
} finally { Pop-Location }

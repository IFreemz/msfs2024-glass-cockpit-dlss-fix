# Builds both versions of the fix with the MSVC and Windows SDK tools found by vswhere:
#   build\dxgi.dll                        standalone, installed next to FlightSimulator2024.exe
#   build\glass-cockpit-dlss-fix.addon64  ReShade add-on
#   -Test        also builds the offline test hosts (run scripts\test.ps1 afterwards): build\test for dxgi.dll, and
#                build\test-addon for the add-on if a ReShade DLL with add-on support is found
#   -ReShadeDll  that ReShade64.dll/.asi, copied into build\test-addon (default: installed MSFS copy, if there)
param([switch]$Test, [string]$ReShadeDll)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio with the C++ x64 tools is required' }
$tools = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$compiler = Join-Path $tools.FullName 'bin\Hostx64\x64\cl.exe'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
# The newest SDK with dxc. Only version folders: the Windows Driver Kit adds Include\wdf, which sorts after them by name.
$sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Where-Object { $_.Name -match '^\d+(\.\d+){3}$' -and (Test-Path (Join-Path $sdkRoot "bin\$($_.Name)\x64\dxc.exe")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'A Windows SDK with dxc is required (see BUILDING.md)' }
$dxc = Join-Path $sdkRoot "bin\$($sdk.Name)\x64\dxc.exe"
$rc = Join-Path $sdkRoot "bin\$($sdk.Name)\x64\rc.exe"
$env:INCLUDE = @((Join-Path $tools.FullName 'include'), (Join-Path $sdk.FullName 'ucrt'), (Join-Path $sdk.FullName 'shared'), (Join-Path $sdk.FullName 'um'), (Join-Path $sdk.FullName 'winrt')) -join ';'
$env:LIB = @((Join-Path $tools.FullName 'lib\x64'), (Join-Path $sdkRoot "Lib\$($sdk.Name)\ucrt\x64"), (Join-Path $sdkRoot "Lib\$($sdk.Name)\um\x64")) -join ';'
$env:PATH = (Split-Path $compiler) + ';' + $env:PATH

$minhook = Join-Path $root 'third_party\minhook'
$reshadeInclude = Join-Path $root 'third_party\reshade\include'
$imgui = Join-Path $root 'third_party\imgui'
if (-not (Test-Path (Join-Path $minhook 'include\MinHook.h')) -or -not (Test-Path (Join-Path $reshadeInclude 'reshade.hpp'))) {
    throw 'Submodules missing: run "git submodule update --init"'
}
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
Push-Location $build
try {
    & $compiler /nologo /c /TC /W3 /MD /O2 (Join-Path $minhook 'src\buffer.c') (Join-Path $minhook 'src\hook.c') (Join-Path $minhook 'src\trampoline.c') (Join-Path $minhook 'src\hde\hde64.c')
    if ($LASTEXITCODE) { throw 'MinHook compilation failed' }
    # Shaders -> one generated header (32 display-slot variants of the map shader, flat/brightness/VR/screen-t0, + compute passes).
    $headers = @()
    foreach ($slot in 0..31) {
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -WX -O3 -Fh "map_ps_$slot.h" -Vn "map_ps_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "Map shader $slot failed" }
        $headers += "map_ps_$slot.h"
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -D BRIGHTNESS=1 -WX -O3 -Fh "map_ps_b_$slot.h" -Vn "map_ps_b_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "Brightness map shader $slot failed" }
        $headers += "map_ps_b_$slot.h"
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -D VR=1 -WX -O3 -Fh "map_ps_vr_$slot.h" -Vn "map_ps_vr_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "VR map shader $slot failed" }
        $headers += "map_ps_vr_$slot.h"
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -D VR=1 -D BRIGHTNESS=1 -WX -O3 -Fh "map_ps_vr_b_$slot.h" -Vn "map_ps_vr_b_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "VR brightness map shader $slot failed" }
        $headers += "map_ps_vr_b_$slot.h"
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -D SCREEN_T0=1 -WX -O3 -Fh "map_ps_t0_$slot.h" -Vn "map_ps_t0_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "Screen-t0 map shader $slot failed" }
        $headers += "map_ps_t0_$slot.h"
        & $dxc -T ps_6_6 -HV 2021 -E main -D "DISPLAY_SLOT=$slot" -D VR=1 -D SCREEN_T0=1 -WX -O3 -Fh "map_ps_vr_t0_$slot.h" -Vn "map_ps_vr_t0_$slot" (Join-Path $root 'src\display_map.hlsl')
        if ($LASTEXITCODE) { throw "VR screen-t0 map shader $slot failed" }
        $headers += "map_ps_vr_t0_$slot.h"
    }
    # Screen capture: the screen texture's colour at the moment the sim draws it (flat; standard and screen-t0 variants).
    & $dxc -T ps_6_6 -HV 2021 -E main -D DISPLAY_SLOT=0 -D COLOUR=1 -WX -O3 -Fh 'map_colour_ps.h' -Vn 'map_colour_ps' (Join-Path $root 'src\display_map.hlsl')
    if ($LASTEXITCODE) { throw 'Colour capture shader failed' }
    & $dxc -T ps_6_6 -HV 2021 -E main -D DISPLAY_SLOT=0 -D COLOUR=1 -D SCREEN_T0=1 -WX -O3 -Fh 'map_colour_ps_t0.h' -Vn 'map_colour_ps_t0' (Join-Path $root 'src\display_map.hlsl')
    if ($LASTEXITCODE) { throw 'Screen-t0 colour capture shader failed' }
    $headers += 'map_colour_ps.h', 'map_colour_ps_t0.h'
    foreach ($pass in 'zero', 'fit', 'solve', 'composite', 'clear', 'glass_filter') {
        & $dxc -T cs_6_6 -HV 2021 -E $pass -WX -O3 -Fh "cs_$pass.h" -Vn "cs_$pass" (Join-Path $root 'src\overlay.hlsl')
        if ($LASTEXITCODE) { throw "Compute pass $pass failed" }
        $headers += "cs_$pass.h"
    }
    Set-Content -LiteralPath 'overlay_shaders.h' -Value (@('#pragma once') + ($headers | ForEach-Object { "#include `"$_`"" }))
    # Version resources (file properties in Explorer, and ReShade's add-on list: name, author, version).
    & $rc /nologo /I (Join-Path $root 'src') /I (Join-Path $sdk.FullName 'um') /I (Join-Path $sdk.FullName 'shared') /fo version.res (Join-Path $root 'src\version.rc')
    if ($LASTEXITCODE) { throw 'Version resource failed' }
    & $rc /nologo /D GLASS_RESHADE_ADDON /I (Join-Path $root 'src') /I (Join-Path $sdk.FullName 'um') /I (Join-Path $sdk.FullName 'shared') /fo version_addon.res (Join-Path $root 'src\version.rc')
    if ($LASTEXITCODE) { throw 'Add-on version resource failed' }
    # The fix is compiled once and linked into both versions; only how it gets loaded differs (src\host.hpp).
    $sources = 'glass_cockpit_dlss_fix.cpp', 'status_report.cpp', 'vtable_slots.cpp', 'dxgi_proxy.cpp', 'settings_window.cpp', 'reshade_addon.cpp', 'status_panel.cpp' | ForEach-Object { Join-Path $root "src\$_" }
    & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /c /I $build /I (Join-Path $minhook 'include') /I $reshadeInclude /I $imgui $sources
    if ($LASTEXITCODE) { throw 'Compilation failed' }
    $fix = 'glass_cockpit_dlss_fix.obj', 'status_report.obj', 'vtable_slots.obj', 'buffer.obj', 'hook.obj', 'trampoline.obj', 'hde64.obj'
    # Standalone. No dxgi.lib: this DLL is dxgi.dll and passes its exports through to the system's (src\dxgi_proxy.cpp,
    # src\dxgi.def). /NOIMPLIB: an import library named dxgi.lib here would be linked into the test host and the add-on
    # instead of the SDK's.
    Remove-Item -LiteralPath 'dxgi.lib' -ErrorAction SilentlyContinue
    & $compiler /nologo /LD $fix dxgi_proxy.obj settings_window.obj version.res /link /DEF:"$root\src\dxgi.def" /NOIMPLIB d3d12.lib version.lib user32.lib gdi32.lib shell32.lib ole32.lib windowscodecs.lib gdiplus.lib delayimp.lib /DELAYLOAD:gdiplus.dll /OUT:dxgi.dll
    if ($LASTEXITCODE) { throw 'dxgi.dll link failed' }
    # ReShade add-on.
    & $compiler /nologo /LD $fix reshade_addon.obj status_panel.obj version_addon.res /link /NOIMPLIB d3d12.lib dxgi.lib version.lib user32.lib shell32.lib ole32.lib windowscodecs.lib /OUT:glass-cockpit-dlss-fix.addon64
    if ($LASTEXITCODE) { throw 'Add-on link failed' }

    if ($Test) {
        $unit = Join-Path $build 'unit'
        New-Item -ItemType Directory -Path $unit -Force | Out-Null
        & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /Fe:"$unit\external_tint.exe" (Join-Path $root 'tests\external_tint.cpp')
        if ($LASTEXITCODE) { throw 'External tint test compilation failed' }
        # Compile the production residual-compositing block against synthetic samples on the GPU.
        $overlay = [IO.File]::ReadAllText((Join-Path $root 'src\overlay.hlsl'))
        $begin = $overlay.IndexOf('    float3 smooth = 0, m1 = 0, m2 = 0, scene_low = 1e9, scene_high = -1e9;')
        $last = '    float3 colour = content + glass;'
        $end = $overlay.IndexOf($last, [Math]::Max(0, $begin))
        if ($begin -lt 0 -or $end -lt $begin) { throw 'Could not locate production residual block for regression test' }
        $block = $overlay.Substring($begin, $end + $last.Length - $begin).Replace('residual[q]', 'test_residual(q)')
        $template = [IO.File]::ReadAllText((Join-Path $root 'tests\residual_guard.hlsl'))
        [IO.File]::WriteAllText((Join-Path $unit 'residual_guard.hlsl'), $template.Replace('    // COMPOSITE_RESIDUAL_BLOCK', $block))
        & $dxc -T cs_6_6 -HV 2021 -E main -WX -O3 -Fo "$unit\residual_guard.cso" "$unit\residual_guard.hlsl"
        if ($LASTEXITCODE) { throw 'Residual regression shader compilation failed' }
        & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /Fe:"$unit\residual_guard.exe" (Join-Path $root 'tests\residual_guard.cpp') /link d3d12.lib
        if ($LASTEXITCODE) { throw 'Residual regression host compilation failed' }
        # Includes the real settings implementation, without starting the engine/hook worker.
        & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /I $build /I (Join-Path $minhook 'include') /I $reshadeInclude /I $imgui /Fe:"$unit\settings_merge.exe" (Join-Path $root 'tests\settings_merge.cpp') status_report.obj vtable_slots.obj buffer.obj hook.obj trampoline.obj hde64.obj /link d3d12.lib dxgi.lib version.lib user32.lib shell32.lib ole32.lib windowscodecs.lib
        if ($LASTEXITCODE) { throw 'Settings merge test compilation failed' }
        $startup = Join-Path $build 'startup'
        New-Item -ItemType Directory -Path $startup -Force | Out-Null
        & $compiler /nologo /std:c++20 /EHsc /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Fe:"$startup\FlightSimulator2024.exe" (Join-Path $root 'tests\startup.cpp') /link d3d12.lib dxgi.lib user32.lib
        if ($LASTEXITCODE) { throw 'Startup probe compilation failed' }
        Copy-Item -LiteralPath 'dxgi.dll' -Destination $startup -Force
        # Fake FlightSimulator2024.exe + fake _nvngx.dll + the fix as its dxgi.dll, with stand-in display shaders.
        $testHost = Join-Path $build 'test'
        # Recreated: an older build put the add-on and ReShade here, and the test host would load those instead.
        if (Test-Path -LiteralPath $testHost) { Remove-Item -LiteralPath $testHost -Recurse -Force }
        New-Item -ItemType Directory -Path $testHost -Force | Out-Null
        & $dxc -T vs_6_6 -HV 2021 -E main -WX -O3 -Fo (Join-Path $testHost 'fixture_vs.cso') (Join-Path $root 'tests\fixture_vs.hlsl')
        if ($LASTEXITCODE) { throw 'Test vertex shader failed' }
        & $dxc -T ps_6_6 -HV 2021 -E main -D REFERENCE=0 -WX -O3 -Fo (Join-Path $testHost 'fixture_ps.cso') (Join-Path $root 'tests\fixture_ps.hlsl')
        if ($LASTEXITCODE) { throw 'Test pixel shader failed' }
        & $dxc -T ps_6_6 -HV 2021 -E main -D REFERENCE=1 -WX -O3 -Fo (Join-Path $testHost 'reference_ps.cso') (Join-Path $root 'tests\fixture_ps.hlsl')
        if ($LASTEXITCODE) { throw 'Reference shader failed' }
        # VR stand-ins (instanced stereo into layered targets) and the per-eye references.
        & $dxc -T vs_6_6 -HV 2021 -E main -D VR=1 -D REFERENCE=0 -WX -O3 -Fo (Join-Path $testHost 'fixture_vr_vs.cso') (Join-Path $root 'tests\fixture_vs.hlsl')
        if ($LASTEXITCODE) { throw 'VR test vertex shader failed' }
        foreach ($eye in 0, 1) {
            & $dxc -T vs_6_6 -HV 2021 -E main -D VR=1 -D REFERENCE=1 -D "REFERENCE_EYE=$eye" -WX -O3 -Fo (Join-Path $testHost "reference_vr_vs_eye$eye.cso") (Join-Path $root 'tests\fixture_vs.hlsl')
            if ($LASTEXITCODE) { throw 'VR reference vertex shader failed' }
        }
        # Display shader variant with the screen in t0 (A340), and its reference.
        foreach ($reference in 0, 1) {
            $prefix = if ($reference) { 'reference_t0' } else { 'fixture_t0' }
            & $dxc -T ps_6_6 -HV 2021 -E main -D SCREEN_T0=1 -D "REFERENCE=$reference" -WX -O3 -Fo (Join-Path $testHost "$($prefix)_ps.cso") (Join-Path $root 'tests\fixture_ps.hlsl')
            if ($LASTEXITCODE) { throw 'Screen-t0 test pixel shader failed' }
            $prefix = if ($reference) { 'reference_vr_t0' } else { 'fixture_vr_t0' }
            & $dxc -T ps_6_6 -HV 2021 -E main -D VR=1 -D SCREEN_T0=1 -D "REFERENCE=$reference" -WX -O3 -Fo (Join-Path $testHost "$($prefix)_ps.cso") (Join-Path $root 'tests\fixture_ps.hlsl')
            if ($LASTEXITCODE) { throw 'VR screen-t0 test pixel shader failed' }
        }
        foreach ($reference in 0, 1) {
            $prefix = if ($reference) { 'reference_vr' } else { 'fixture_vr' }
            & $dxc -T ps_6_6 -HV 2021 -E main -D VR=1 -D "REFERENCE=$reference" -WX -O3 -Fo (Join-Path $testHost "$($prefix)_ps.cso") (Join-Path $root 'tests\fixture_ps.hlsl')
            if ($LASTEXITCODE) { throw 'VR test pixel shader failed' }
        }
        & $compiler /nologo /W4 /WX /MD /O2 /LD /hotpatch (Join-Path $root 'tests\fake_nvngx.cpp') /link /OPT:NOICF /FUNCTIONPADMIN:16 /DEF:"$root\tests\fake_nvngx.def" /OUT:"$testHost\_nvngx.dll"
        if ($LASTEXITCODE) { throw 'Fake NGX compilation failed' }
        & $compiler /nologo /W4 /WX /MD /O2 /LD (Join-Path $root 'tests\fake_ffx.cpp') /link /OUT:"$testHost\amd_fidelityfx_dx12.dll"
        # The same fake FSR under the name and folder OptiScaler uses for its own FidelityFX upscaler (--optiscaler-fsr).
        New-Item -ItemType Directory -Force (Join-Path $testHost 'OptiScaler') | Out-Null
        if ($LASTEXITCODE) { throw 'Fake FidelityFX compilation failed' }
        Copy-Item "$testHost\amd_fidelityfx_dx12.dll" (Join-Path $testHost 'OptiScaler\amd_fidelityfx_upscaler_dx12.dll') -Force
        # OptiScaler as the sim's DLSS, answering with FSR (--ngx-proxy).
        & $compiler /nologo /W4 /WX /MD /O2 /LD (Join-Path $root 'tests\fake_proxy.cpp') /link /DEF:"$root\tests\fake_proxy.def" /OUT:"$testHost\OptiScaler.asi"
        if ($LASTEXITCODE) { throw 'Fake OptiScaler compilation failed' }
        & $compiler /nologo /std:c++20 /EHsc /W4 /WX /MD /O2 /permissive- /Fe:"$testHost\FlightSimulator2024.exe" (Join-Path $root 'tests\fixture.cpp') /link d3d12.lib dxgi.lib ole32.lib windowscodecs.lib
        if ($LASTEXITCODE) { throw 'Test host compilation failed' }
        # The test host imports dxgi.dll, so Windows loads the fix from its folder at start, as the sim does.
        Copy-Item -LiteralPath 'dxgi.dll' -Destination $testHost -Force

        # The same test host and stand-ins with ReShade and the add-on instead (the host loads ReShade64.asi when the
        # add-on is next to it). Needs a ReShade DLL with add-on support.
        $addonHost = Join-Path $build 'test-addon'
        if (Test-Path -LiteralPath $addonHost) { Remove-Item -LiteralPath $addonHost -Recurse -Force }
        if (-not $ReShadeDll) {
            $default = 'C:\Program Files (x86)\Steam\steamapps\common\Microsoft Flight Simulator 2024\ReShade64.dll'
            if (Test-Path -LiteralPath $default) { $ReShadeDll = $default }
        } elseif (-not (Test-Path -LiteralPath $ReShadeDll)) { throw "ReShade not found at $ReShadeDll" }
        if ($ReShadeDll) {
            New-Item -ItemType Directory -Path (Join-Path $addonHost 'OptiScaler') -Force | Out-Null
            Get-ChildItem -LiteralPath $testHost -File | Where-Object { $_.Extension -in '.cso', '.exe', '.asi' -or $_.Name -in '_nvngx.dll', 'amd_fidelityfx_dx12.dll' } |
                Copy-Item -Destination $addonHost
            Copy-Item (Join-Path $testHost 'OptiScaler\amd_fidelityfx_upscaler_dx12.dll') (Join-Path $addonHost 'OptiScaler')
            Copy-Item -LiteralPath 'glass-cockpit-dlss-fix.addon64' -Destination $addonHost
            Copy-Item -LiteralPath $ReShadeDll -Destination (Join-Path $addonHost 'ReShade64.asi')
        } else {
            Write-Warning 'No ReShade DLL with add-on support found: the add-on test host is skipped (pass -ReShadeDll <path to ReShade64.dll>)'
        }
    }
} finally { Pop-Location }

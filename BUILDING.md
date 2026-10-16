# Building from source

You only need this to change the fix. To use it, download the release instead (see the [README](README.md)).

Requirements: Visual Studio 2022 with the C++ x64 tools, and a Windows SDK newer than 10.0.19041 (provides `dxc` and
`rc`; tested with 10.0.28000). The `dxc` in 10.0.19041 and older can't compile the shaders (no HLSL 2021 or Shader Model
6.6). The build uses the newest SDK installed.

```powershell
git clone https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix
cd msfs2024-glass-cockpit-dlss-fix
git submodule update --init --depth 1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

This builds both versions from the same source:

- `build\glass-cockpit-dlss-fix.addon64`, the ReShade add-on;
- `build\dxgi.dll`, the standalone version (copy it next to `FlightSimulator2024.exe` to try it).

The fix itself (`src\glass_cockpit_dlss_fix.cpp`) is compiled once and linked into both; they differ only in how they
get loaded (`src\host.hpp`): `src\reshade_addon.cpp` and the overlay tab in `src\status_panel.cpp` for the add-on,
`src\dxgi_proxy.cpp` for the standalone version. In both, the fix follows the sim through its own hooks on the
Direct3D 12 runtime.

## Offline test

No sim needed, but an NVIDIA GPU with Shader Model 6.6 is. To test the add-on too, pass the path of a ReShade DLL with
full add-on support (e.g. the one from your MSFS folder); without it only the standalone version is tested:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test -ReShadeDll "<path to ReShade64.dll>"
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

The test runs the fix inside a fake `FlightSimulator2024.exe` with stand-in display shaders and a fake DLSS that leaves
worst-case ghosts, and checks the result pixel by pixel against an unjittered reference. Every case runs twice: in
`build\test` the fake sim loads `dxgi.dll` from its own folder (as the sim does), in `build\test-addon` it loads the real
ReShade, which loads the add-on.

## Release

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version <version in src\version.h>
```

This creates `dist\glass-cockpit-dlss-fix.addon64` and `dist\dxgi.dll`, the two files attached to the release. The
GitHub Actions workflow (`.github/workflows/build.yml`) builds the same files on every push and keeps them as a build
artifact.

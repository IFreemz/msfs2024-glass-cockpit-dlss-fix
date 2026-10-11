# Building from source

You only need this to change the add-on. To use it, download the release instead (see the [README](README.md)).

Requirements: Visual Studio 2022 with the C++ x64 tools, and the Windows 10/11 SDK (provides `dxc` and `rc`).

```powershell
git clone https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix
cd msfs2024-glass-cockpit-dlss-fix
git submodule update --init --depth 1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

The add-on is written to `build\glass-cockpit-dlss-fix.addon64`.

## Offline test

No sim needed, but an NVIDIA GPU with Shader Model 6.6 is. Pass the path of a ReShade DLL with full add-on support
(e.g. the one from your MSFS folder):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test -ReShadeDll "<path to ReShade64.dll>"
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

The test runs the add-on inside a fake `FlightSimulator2024.exe` with a real ReShade, stand-in display shaders and a
fake DLSS that leaves worst-case ghosts, and checks the result pixel by pixel against an unjittered reference.

## Release

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version <version in src\version.h>
```

This creates `dist\glass-cockpit-dlss-fix-<version>.zip`.

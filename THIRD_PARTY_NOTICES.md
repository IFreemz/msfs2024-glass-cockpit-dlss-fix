# Third-party notices

| Component | Use | License |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) (git submodule) | Hooks the D3D12, DXGI and upscaler functions the fix follows; compiled into both versions | BSD 2-Clause, © Tsuda Kageyu |
| [ReShade](https://github.com/crosire/reshade) (git submodule, `include/` only) | Add-on API headers (ReShade add-on version) | BSD 3-Clause, © Patrick Mours |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.5 (`third_party/imgui`, headers only) | Status tab in the ReShade menu, drawn through ReShade's own ImGui (ReShade add-on version) | MIT, © Omar Cornut |

The list of `dxgi.dll` exports the standalone version passes through (`src/dxgi_proxy.cpp`) is the one ReShade provides
when installed as `dxgi.dll`; no ReShade code is included in either version.

`src/ngx_interface.hpp` declares the small part of the NVIDIA NGX (DLSS) interface the fix calls through; no
NVIDIA code or binaries are included. Likewise `src/ffx_interface.hpp` declares the few AMD FidelityFX API structures
used to find FSR upscaling (following AMD's MIT-licensed FidelityFX SDK headers); no AMD code or binaries are included.
NVIDIA and DLSS are trademarks of NVIDIA Corporation; AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices. Microsoft Flight
Simulator is a trademark of Microsoft Corporation.

The full license texts are in `third_party/minhook/LICENSE.txt`, `third_party/reshade/LICENSE.md` and `third_party/imgui/LICENSE.txt`. MinHook's
license also applies to the compiled `glass-cockpit-dlss-fix.addon64` and `dxgi.dll`.

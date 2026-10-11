# Third-party notices

| Component | Use | License |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) (git submodule) | Hooks the DLSS evaluate call and pipeline creation; compiled into the add-on | BSD 2-Clause, © Tsuda Kageyu |
| [ReShade](https://github.com/crosire/reshade) (git submodule, `include/` only) | Add-on API headers | BSD 3-Clause, © Patrick Mours |

`src/ngx_interface.hpp` declares the small part of the NVIDIA NGX (DLSS) interface the add-on calls through; no
NVIDIA code or binaries are included. NVIDIA and DLSS are trademarks of NVIDIA Corporation. Microsoft Flight
Simulator is a trademark of Microsoft Corporation.

The full license texts are in `third_party/minhook/LICENSE.txt` and `third_party/reshade/LICENSE.md`. MinHook's
license also applies to the compiled `glass-cockpit-dlss-fix.addon64`.

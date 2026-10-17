// How the fix gets loaded. The same fix (glass_cockpit_dlss_fix.cpp) is built two ways:
// - standalone: dxgi.dll next to FlightSimulator2024.exe, so Windows loads it when the sim starts (dxgi_proxy.cpp);
// - ReShade add-on: glass-cockpit-dlss-fix.addon64, loaded by ReShade (reshade_addon.cpp), with a tab in its overlay.
// From there on both work the same way: the fix follows the sim through its own hooks on the D3D12 runtime.
#pragma once

struct IUnknown;
struct ID3D12Device;

namespace glass {
// Starts the fix (log, settings, background threads) if this is the sim's process; false otherwise. reshade_module:
// the ReShade DLL when loaded as its add-on, null for the standalone dxgi.dll.
bool start(void* self_module, void* reshade_module) noexcept;
// DLL_PROCESS_DETACH: from then on the hooks pass everything straight through.
void stop();
void write_log(const char* format, ...);

// Standalone: hooks D3D12CreateDevice (the sim creates its device after loading dxgi.dll), and swapchain creation on
// every DXGI factory made through this DLL's exports.
void hook_device_creation() noexcept;
// Standalone: the settings window (settings_window.cpp), opened and closed with Ctrl+Shift+F6 through
// settings_window_hotkey (null in the add-on, which has its tab in the ReShade overlay instead).
void init_settings_window(void* self_module);
extern void (*settings_window_hotkey)();
void dxgi_factory_created(IUnknown* factory);

// ReShade add-on: the devices ReShade reports (the add-on is loaded when the first one is created), and the swapchain
// the sim presents with and its command queue (native objects, not ReShade's wrappers).
void device_created(ID3D12Device* device);
void swapchain_created(IUnknown* queue, IUnknown* swapchain);
}

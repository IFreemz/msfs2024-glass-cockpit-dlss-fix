// The fix as a ReShade add-on (glass-cockpit-dlss-fix.addon64). ReShade loads it when the sim creates its graphics
// device and reports that device and the sim's swapchain to it; from there the fix follows the sim through its own
// hooks on the D3D12 runtime, as the standalone dxgi.dll does. ReShade's overlay gets a tab with the fix's status and
// settings (status_panel.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <reshade.hpp>
#include "host.hpp"
#include "status.hpp"
#include "version.h"

namespace api = reshade::api;

extern "C" __declspec(dllexport) const char* NAME = GLASS_NAME;
extern "C" __declspec(dllexport) const char* AUTHOR = GLASS_AUTHOR;
extern "C" __declspec(dllexport) const char* WEBSITE = GLASS_WEBSITE;
extern "C" __declspec(dllexport) const char* ISSUES = GLASS_ISSUES;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Removes DLSS ghosting from MSFS 2024 glass cockpit displays. Ctrl+Shift+F9 on/off, Ctrl+Shift+F11 green tint.";

namespace {
void on_init_device(api::device* device) {
    if (device->get_api() == api::device_api::d3d12) glass::device_created(reinterpret_cast<ID3D12Device*>(device->get_native()));
}
// The swapchain the sim presents with and its queue: the fix's frame clock and the frames a capture saves. VR
// runtimes (OpenXR, OpenVR) have no window and no DXGI swapchain.
void on_init_effect_runtime(api::effect_runtime* runtime) {
    const auto queue = runtime->get_command_queue();
    if (!runtime->get_hwnd() || !queue || runtime->get_device()->get_api() != api::device_api::d3d12) return;
    glass::swapchain_created(reinterpret_cast<IUnknown*>(queue->get_native()), reinterpret_cast<IUnknown*>(runtime->get_native()));
}

bool started = false;
}

// ReShade unloads its add-ons when the last device goes away and loads them again for the next one (e.g. when another
// mod such as OptiScaler creates and releases a device at startup). The module stays pinned (its hooks must stay
// valid), so a reload calls AddonInit again on the same, already started module: only the ReShade side is redone.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshade_module) {
    if (!reshade::register_addon(addon, reshade_module)) return false;
    if (started) glass::write_log("add-on reloaded by ReShade (another device was created and released before; normal with some other mods)");
    else {
        started = glass::start(addon, reshade_module);
        if (!started) { // not the sim, or hooking unavailable
            reshade::unregister_addon(addon);
            return false;
        }
    }
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    glass::write_log("overlay tab %s", glass::init_status_panel(reshade_module) ? "added to the ReShade overlay"
        : "unavailable (this ReShade build has a different ImGui version); the fix itself still works");
    return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE) {
    glass::write_log("add-on unloaded by ReShade");
    glass::uninit_status_panel();
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_addon(addon);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    else if (reason == DLL_PROCESS_DETACH) glass::stop();
    return TRUE;
}

// The standalone fix: dxgi.dll pass-through. The fix is installed as dxgi.dll next to FlightSimulator2024.exe, so
// Windows loads it in place of the system's dxgi.dll when the sim starts (early enough to see the sim create its
// graphics device). The sim, Windows' own D3D12 runtime and other mods then call these exports; each calls the same
// export of the system's dxgi.dll. The exports are the ones ReShade provides when it is installed as dxgi.dll.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dxgi1_6.h>
#include <cwchar>
#include "host.hpp"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (glass::start(instance, nullptr)) {
            glass::init_settings_window(instance);
            glass::hook_device_creation();
        }
    } else if (reason == DLL_PROCESS_DETACH) glass::stop();
    return TRUE;
}

namespace {
HMODULE system_dxgi() {
    static const HMODULE module = [] {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH - 16) return HMODULE(nullptr);
        wcscat_s(path, L"\\dxgi.dll");
        return LoadLibraryW(path);
    }();
    return module;
}
template <class T> T system_export(const char* name) {
    const HMODULE module = system_dxgi();
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}
HRESULT factory_created(HRESULT hr, void** factory) {
    if (SUCCEEDED(hr) && factory && *factory) glass::dxgi_factory_created(static_cast<IUnknown*>(*factory)); // hooks swapchain creation (once)
    return hr;
}
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory) {
    static const auto original = system_export<decltype(&CreateDXGIFactory)>("CreateDXGIFactory");
    return original ? factory_created(original(riid, factory), factory) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    static const auto original = system_export<decltype(&CreateDXGIFactory1)>("CreateDXGIFactory1");
    return original ? factory_created(original(riid, factory), factory) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    static const auto original = system_export<decltype(&CreateDXGIFactory2)>("CreateDXGIFactory2");
    return original ? factory_created(original(flags, riid, factory), factory) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    static const auto original = system_export<decltype(&DXGIGetDebugInterface1)>("DXGIGetDebugInterface1");
    return original ? original(flags, riid, debug) : E_NOINTERFACE; // as without the Windows SDK's debug layer
}
extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    static const auto original = system_export<decltype(&DXGIDeclareAdapterRemovalSupport)>("DXGIDeclareAdapterRemovalSupport");
    return original ? original() : S_OK;
}

// Undocumented exports that Windows' graphics components call; passed through as they are.
extern "C" BOOL WINAPI CompatValue(LPCSTR name, UINT64* value) {
    static const auto original = system_export<decltype(&CompatValue)>("CompatValue");
    return original ? original(name, value) : FALSE;
}
extern "C" BOOL WINAPI CompatString(LPCSTR name, ULONG* size, LPSTR data, bool flag) {
    static const auto original = system_export<decltype(&CompatString)>("CompatString");
    return original ? original(name, size, data, flag) : FALSE;
}
extern "C" HRESULT WINAPI DXGIDumpJournal(void* callback) {
    static const auto original = system_export<decltype(&DXGIDumpJournal)>("DXGIDumpJournal");
    return original ? original(callback) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGIReportAdapterConfiguration(void* info) {
    static const auto original = system_export<decltype(&DXGIReportAdapterConfiguration)>("DXGIReportAdapterConfiguration");
    return original ? original(info) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGID3D10CreateDevice(HMODULE module, IDXGIFactory* factory, IDXGIAdapter* adapter, UINT flags, const void* levels, UINT level_count,
    void** device) {
    static const auto original = system_export<decltype(&DXGID3D10CreateDevice)>("DXGID3D10CreateDevice");
    return original ? original(module, factory, adapter, flags, levels, level_count, device) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGID3D10CreateLayeredDevice(IDXGIAdapter* adapter, UINT flags, void* unknown, REFIID riid, void** device) {
    static const auto original = system_export<decltype(&DXGID3D10CreateLayeredDevice)>("DXGID3D10CreateLayeredDevice");
    return original ? original(adapter, flags, unknown, riid, device) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGID3D10GetLayeredDeviceSize(const void* layers, UINT count) {
    static const auto original = system_export<decltype(&DXGID3D10GetLayeredDeviceSize)>("DXGID3D10GetLayeredDeviceSize");
    return original ? original(layers, count) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DXGID3D10RegisterLayers(const void* layers, UINT count) {
    static const auto original = system_export<decltype(&DXGID3D10RegisterLayers)>("DXGID3D10RegisterLayers");
    return original ? original(layers, count) : E_NOTIMPL;
}
extern "C" void WINAPI DXGID3D10ETWRundown() {}

// The standalone installed as version.dll or winmm.dll instead of dxgi.dll (for when another mod already uses
// dxgi.dll): the sim loads those two from its own folder too, as early. This DLL then stands in for that system DLL as
// well, so it passes all of its exports through (src\proxy_exports.asm, generated from the system DLLs' export lists):
// each one jumps to the system DLL's function, looked up on its first call. The fix itself works the same under any of
// the three names; it doesn't need the sim's DXGI calls to go through this DLL (see hook_device_creation).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cwchar>
#include <iterator>

namespace {
enum class ProxyDll { version, winmm };
struct ProxyExport { ProxyDll dll; const char* name; WORD ordinal; };
constexpr ProxyExport proxy_exports[] = {
#include "proxy_exports.inc"
};

HMODULE system_module(ProxyDll dll) {
    static std::atomic<HMODULE> modules[2]{};
    auto& module = modules[int(dll)];
    if (HMODULE loaded = module.load()) return loaded;
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 16) return nullptr;
    wcscat_s(path, dll == ProxyDll::version ? L"\\version.dll" : L"\\winmm.dll");
    const HMODULE loaded = LoadLibraryW(path); // by full path: the system's copy, not this DLL of the same name
    module.store(loaded);
    return loaded;
}

// For a function this Windows version's DLL doesn't have: returns 0 (failure for most of them).
INT_PTR WINAPI missing_export() { return 0; }
}

extern "C" void* glass_proxy_targets[std::size(proxy_exports)] = {};

// Called by the export stubs on an export's first call (its index in proxy_exports): the system DLL's function.
extern "C" void* glass_proxy_resolve(unsigned index) {
    if (index >= std::size(proxy_exports)) return reinterpret_cast<void*>(&missing_export);
    const auto& e = proxy_exports[index];
    const HMODULE module = system_module(e.dll);
    void* target = module ? reinterpret_cast<void*>(GetProcAddress(module, e.name ? e.name : MAKEINTRESOURCEA(e.ordinal))) : nullptr;
    if (!target) target = reinterpret_cast<void*>(&missing_export);
    glass_proxy_targets[index] = target;
    return target;
}

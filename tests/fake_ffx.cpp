// Fixture stand-in for amd_fidelityfx_dx12.dll: "FSR" that leaves the output untouched and reports success.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../src/ffx_interface.hpp"

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext*, ffxApiHeader*, const void*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext*, const void*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext*, const ffxApiHeader*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext*, ffxApiHeader*) { return FFX_API_RETURN_OK; }
// Not __declspec(noinline)-trivial: MinHook needs a few bytes of real code to patch.
extern "C" __declspec(dllexport) __declspec(noinline) ffxReturnCode_t ffxDispatch(ffxContext*, const ffxApiHeader* desc) {
    static volatile unsigned calls = 0;
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) calls = calls + 1;
    return FFX_API_RETURN_OK;
}

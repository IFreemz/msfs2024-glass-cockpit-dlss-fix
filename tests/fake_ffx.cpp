// Fixture stand-in for amd_fidelityfx_dx12.dll: "FSR" that leaves the output untouched and reports success.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "../src/ffx_interface.hpp"
#include "../src/ngx_interface.hpp"

// --nested: answer each upscale with "DLSS" inside the dispatch, the way OptiScaler does with its upscaler set to DLSS.
static NgxEvaluate inner_evaluate = nullptr;
static const NVSDK_NGX_Parameter* inner_params = nullptr;
extern "C" __declspec(dllexport) void FakeFfxRunDlssInside(NgxEvaluate evaluate, const NVSDK_NGX_Parameter* params) {
    inner_evaluate = evaluate; inner_params = params;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext*, ffxApiHeader*, const void*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext*, const void*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext*, const ffxApiHeader*) { return FFX_API_RETURN_OK; }
extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext*, ffxApiHeader*) { return FFX_API_RETURN_OK; }
// The work itself, separate from the export: OptiScaler hooks the sim's ffxDispatch before the add-on and calls the
// original code through its own trampoline, past any later hook (--ngx-proxy calls this directly to do the same).
extern "C" __declspec(dllexport) __declspec(noinline) ffxReturnCode_t FakeFfxDispatchBody(ffxContext*, const ffxApiHeader* desc) {
    static volatile unsigned calls = 0;
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        calls = calls + 1;
        if (inner_evaluate) {
            const auto& upscale = *reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
            inner_evaluate(static_cast<ID3D12GraphicsCommandList*>(upscale.commandList), nullptr, inner_params, nullptr);
        }
    }
    return FFX_API_RETURN_OK;
}
// Not __declspec(noinline)-trivial: MinHook needs a few bytes of real code to patch.
extern "C" __declspec(dllexport) __declspec(noinline) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxApiHeader* desc) {
    static volatile unsigned dispatches = 0;
    dispatches = dispatches + 1;
    return FakeFfxDispatchBody(context, desc);
}

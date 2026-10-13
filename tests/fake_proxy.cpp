// Fixture stand-in for OptiScaler as the sim's DLSS (--ngx-proxy): it exports the same evaluate function as the driver's
// _nvngx.dll and answers it with FSR from its own FidelityFX upscaler file, never calling real DLSS (as on an AMD GPU).
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "../src/ngx_interface.hpp"
#include "../src/ffx_interface.hpp"

static FfxDispatch fsr = nullptr;
static ffxDispatchDescUpscale* upscale = nullptr;
extern "C" __declspec(dllexport) void FakeProxyAnswerWithFsr(FfxDispatch dispatch, ffxDispatchDescUpscale* desc) { fsr = dispatch; upscale = desc; }

// Exported as NVSDK_NGX_D3D12_EvaluateFeature (fake_proxy.def).
extern "C" __declspec(noinline) NVSDK_NGX_Result FakeProxyEvaluate(
    ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    if (!fsr || !upscale) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    upscale->commandList = list;
    return fsr(nullptr, &upscale->header) == FFX_API_RETURN_OK ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
}

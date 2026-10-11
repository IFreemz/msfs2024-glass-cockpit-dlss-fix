// Fixture stand-in for the driver's _nvngx.dll: "DLSS" that leaves the output untouched and reports success.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "../src/ngx_interface.hpp"

static unsigned calls = 0;
extern "C" __declspec(dllexport) unsigned FakeNgxCalls() { return calls; }

extern "C" NVSDK_NGX_Result FakeEvaluate(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    ++calls;
    return NVSDK_NGX_Result_Success;
}
extern "C" NVSDK_NGX_Result FakeEvaluateC(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C) {
    ++calls;
    return NVSDK_NGX_Result_Success;
}

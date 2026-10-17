#pragma once
// Minimal declarations of the NVIDIA NGX (DLSS) API surface this add-on touches: the D3D12 evaluate entry points
// and the parameter map interface. Layout and names are those of the public NGX API; nothing here links to NGX.
#include <d3d12.h>

struct ID3D11Resource;
struct NVSDK_NGX_Handle;

enum NVSDK_NGX_Result : int { NVSDK_NGX_Result_Success = 0x1, NVSDK_NGX_Result_FAIL_InvalidParameter = int(0xBAD00005) };

struct NVSDK_NGX_Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;
    virtual NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, float* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, double* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, unsigned int* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, int* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, void** value) const = 0;
    virtual void Reset() = 0;
};

using PFN_NVSDK_NGX_ProgressCallback = void (*)(float progress, bool& cancel);
using PFN_NVSDK_NGX_ProgressCallback_C = void (*)(float progress, bool* cancel);
using NgxEvaluate = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using NgxEvaluateC = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);

#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_Depth "Depth"
#define NVSDK_NGX_Parameter_Jitter_Offset_X "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X "DLSS.Input.Color.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y "DLSS.Input.Color.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X "DLSS.Input.Depth.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y "DLSS.Input.Depth.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X "DLSS.Output.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y "DLSS.Output.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width "DLSS.Render.Subrect.Dimensions.Width"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height "DLSS.Render.Subrect.Dimensions.Height"

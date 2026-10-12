// Minimal declarations of the AMD FidelityFX API (amd_fidelityfx_dx12.dll, FSR 3.1+) used to find FSR upscaling.
// Layouts follow the public FidelityFX SDK headers (ffx_api.h, ffx_api_types.h, ffx_upscale.h; MIT license).
#pragma once
#include <cstdint>

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
constexpr ffxReturnCode_t FFX_API_RETURN_OK = 0;

struct ffxApiHeader {
    uint64_t type;
    ffxApiHeader* pNext;
};

constexpr uint64_t FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x00010001u;

struct FfxApiResourceDescription {
    uint32_t type, format, width, height, depth, mipCount, flags, usage;
};
struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
};
enum : uint32_t {
    FFX_API_RESOURCE_STATE_UNORDERED_ACCESS = 1u << 1,
    FFX_API_RESOURCE_STATE_COMPUTE_READ = 1u << 2,
    FFX_API_RESOURCE_STATE_PIXEL_READ = 1u << 3,
    FFX_API_RESOURCE_STATE_COPY_SRC = 1u << 4,
    FFX_API_RESOURCE_STATE_COPY_DEST = 1u << 5,
    FFX_API_RESOURCE_STATE_RENDER_TARGET = 1u << 8,
};
struct FfxApiFloatCoords2D { float x, y; };
struct FfxApiDimensions2D { uint32_t width, height; };

struct ffxDispatchDescUpscale {
    ffxApiHeader header;
    void* commandList;
    FfxApiResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    FfxApiDimensions2D renderSize, upscaleSize;
    bool enableSharpening;
    float sharpness, frameTimeDelta, preExposure;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    uint32_t flags;
};

using FfxDispatch = ffxReturnCode_t (*)(ffxContext* context, const ffxApiHeader* desc);

// GPL-3.0-only with the additional terms in NOTICE.md.
// The build inserts the REAL composite residual block from src/overlay.hlsl below.
// Only its residual texture reads are supplied synthetic values; the fallback/guard is not reimplemented.
RWStructuredBuffer<float4> result : register(u0);
Texture2D<float4> scene : register(t1);
Texture2DArray<float4> glass_history : register(t2);
RWTexture2D<float4> output : register(u1);
static const uint2 map_size = uint2(16, 16);
static const uint flags = 8192; // no history, no DLSS-detail blend
static const float detail_clamp = 0;
static uint test_case;
float4 test_residual(int2 q)
{
    // 0: no samples; 1: outlines only; 2: regular samples; 3: mixed.
    uint tag = test_case == 0 ? 0 : test_case == 1 ? 65 : test_case == 2 ? 1 : (q.x & 1) ? 65 : 1;
    return float4(0.05, 0.10, 0.15, tag);
}
[numthreads(4, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    test_case = id.x;
    result[id.x] = float4(0.8, 0.1, 0.8, 1); // valid upscaler output, retained only if samples are unavailable
    float2 location = float2(8, 8), f = float2(0.5, 0.5);
    int2 cell = int2(8, 8);
    uint layer = 0;
    float4 offset = float4(0.1, 0.1, 0.1, 0);
    float3 content = float3(0.2, 0.2, 0.2);
    uint slot = 0;
    // COMPOSITE_RESIDUAL_BLOCK
    result[id.x] = float4(colour, 1);
}

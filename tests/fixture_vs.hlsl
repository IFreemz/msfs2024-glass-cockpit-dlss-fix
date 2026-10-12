// Stand-in for the game's cockpit vertex shader in the offline test host: same output signature (CockpitVertex),
// simple camera transform from b0 (row-major view-projection).
// VR=1: stand-in for the VR display vertex shader (instanced stereo): instance = eye, each eye into its own layer, eyes
// 0.06 clip units apart. REFERENCE=1 (with VR) is the reference for eye REFERENCE_EYE: layer 0, other eye clipped away
// (SV_InstanceID ignores the start instance, so the eye cannot be picked with it).
#include "../src/cockpit_vertex.hlsli"

cbuffer Camera : register(b0) { row_major float4x4 view_projection; };

struct Input
{
    float4 position : POSITION;
    float4 tangent : TANGENT;
    float3 normal : NORMAL;
    float4 uv : TEXCOORD0;
    float4 color : TEXCOORD8;
};

#if VR
// Same output signature (order, packing) as the game's VR display vertex shader.
struct VrOutput
{
    float4 previous_position : PREVIOUS_POSITION;
    float4 uv : TEXCOORD0;
    float4 tangent_frame0 : TEXSPACE_MAT0;
    float4 tangent_frame1 : TEXSPACE_MAT1;
    float4 tangent_frame2 : TEXSPACE_MAT2;
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE_ID;
    nointerpolation uint eye : EYE_ID;
    uint layer : SV_RenderTargetArrayIndex;
    float3 color : COLOR_RGB;
    float color_alpha : COLOR_ALPHA;
};

VrOutput main(Input input, uint instance : SV_InstanceID)
{
    VrOutput output = (VrOutput)0;
    output.position = mul(float4(input.position.xyz, 1), view_projection);
    output.position.x += (instance == 0 ? -0.06 : 0.06) * output.position.w;
    output.previous_position = output.position;
    output.uv = input.uv;
    output.tangent_frame0 = input.tangent;
    output.tangent_frame2 = float4(input.normal, 0);
    output.instance = instance;
    output.eye = instance;
    output.layer = REFERENCE ? 0 : instance;
#if REFERENCE
    if (instance != REFERENCE_EYE) output.position = float4(0, 0, 2, 1); // behind the far plane
#endif
    output.color = input.color.rgb;
    output.color_alpha = input.color.a;
    return output;
}
#else
CockpitVertex main(Input input, uint instance : SV_InstanceID)
{
    CockpitVertex output = (CockpitVertex)0;
    output.position = mul(float4(input.position.xyz, 1), view_projection);
    output.previous_position = output.position;
    output.uv = input.uv;
    output.tangent_frame0 = input.tangent;
    output.tangent_frame2 = float4(input.normal, 0);
    output.instance = instance;
    output.color = input.color.rgb;
    output.color_alpha = input.color.a;
    return output;
}
#endif

// Stand-in for the game's cockpit vertex shader in the offline test host: same output signature (CockpitVertex),
// simple camera transform from b0 (row-major view-projection).
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

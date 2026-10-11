// Replayed in place of the cockpit display pixel shader: writes where each render pixel samples its display
// texture. Compiled once per display slot (-D DISPLAY_SLOT=n) so the slot needs no extra root binding.
#include "cockpit_vertex.hlsli"

cbuffer RenderState : register(b1) { float4 render_state; };
cbuffer Material : register(b3) { float4 material[17]; };
Texture2D<float4> base_texture : register(t0);
SamplerState base_sampler : register(s1);

float4 main(CockpitVertex input) : SV_Target0
{
    // Same alpha test as the game's ph_base_gbuffer: t0.a * b3[1].w must exceed b1.x.
    float alpha = base_texture.Sample(base_sampler, input.uv.xy).a * material[1].w;
    if (alpha <= render_state.x) discard;
    return float4(input.uv.xy, input.position.z, DISPLAY_SLOT + 1);
}

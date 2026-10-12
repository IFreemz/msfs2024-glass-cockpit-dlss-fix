// Replayed in place of the cockpit display pixel shader: writes where each render pixel samples its display
// texture. Compiled once per display slot (-D DISPLAY_SLOT=n) so the slot needs no extra root binding, and with
// -D VR=1 for the VR display pipeline (its vertex shader routes each view to its own layer of the map), and with
// -D SCREEN_T0=1 for the display shader variant that reads its screen from t0 (iniBuilds A340 main screens).
#include "cockpit_vertex.hlsli"

cbuffer RenderState : register(b1) { float4 render_state; };
#if SCREEN_T0
cbuffer Material : register(b4) { float4 material[17]; };
#else
cbuffer Material : register(b3) { float4 material[17]; };
#endif
Texture2D<float4> base_texture : register(t0);
SamplerState base_sampler : register(s1);

#if VR
float4 main(CockpitVertexVR input) : SV_Target0
#else
float4 main(CockpitVertex input) : SV_Target0
#endif
{
#if SCREEN_T0
    // The variant's alpha test uses no texture: b4[1].w must exceed b1.x.
    float alpha = material[1].w;
#else
    // Same alpha test as the game's ph_base_gbuffer: t0.a * b3[1].w must exceed b1.x.
    float alpha = base_texture.Sample(base_sampler, input.uv.xy).a * material[1].w;
#endif
    if (alpha <= render_state.x) discard;
    return float4(input.uv.xy, input.position.z, DISPLAY_SLOT + 1);
}

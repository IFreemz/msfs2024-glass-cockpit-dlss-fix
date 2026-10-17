// Replayed in place of the cockpit display pixel shader: writes where each render pixel samples its display
// texture. Compiled once per display slot (-D DISPLAY_SLOT=n) so the slot needs no extra root binding, and with
// -D VR=1 for the VR display pipeline (its vertex shader routes each view to its own layer of the map), and with
// -D SCREEN_T0=1 for the display shader variant that reads its screen from t0 (iniBuilds A340 main screens).
// -D COLOUR=1 (screen capture only): writes the screen texture's colour at the moment the sim draws it instead.
// -D BRIGHTNESS=1 (standard display shader, flat and VR): also records how bright the sim draws this mesh (see below).
#include "cockpit_vertex.hlsli"

cbuffer RenderState : register(b1) { float4 render_state; };
#if SCREEN_T0
cbuffer Material : register(b4) { float4 material[17]; };
#else
cbuffer Material : register(b3) { float4 material[17]; };
#endif
Texture2D<float4> base_texture : register(t0);
#if BRIGHTNESS
// Per-instance data of the game's display shader: its emissive output is the screen colour times row 0's rgb (and the
// vertex colour, and on the emissive path the material's b3[1].y). Aircraft set it per mesh: e.g. the Fenix A320's eight
// readouts share one screen texture, each at its own brightness (DDRMI, FCU, RMPs, ...).
cbuffer Instances : register(b2) { float4 instance_data[64]; };
#endif
#if COLOUR && !SCREEN_T0
Texture2D<float4> screen_texture : register(t1);
#endif
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
#if COLOUR
#if SCREEN_T0
    return float4(base_texture.SampleLevel(base_sampler, input.uv.xy, 0).rgb, 1);
#else
    return float4(screen_texture.SampleLevel(base_sampler, input.uv.xy, 0).rgb, 1);
#endif
#else
    // Texture coordinates whole periods away from the texture (the iFly 737's captain EFB: v = -2..-1.25, drawn by the
    // sim's repeating sampler) are brought back into it; slightly past an edge stays as it is (clamped like before).
    float2 uv = input.uv.xy;
    uv = select(or(uv < -0.05, uv > 1.05), uv - floor(uv), uv);
    // w = slot + 1, plus the mesh's brightness as log2 / 64 (within +-0.49: rounding still gives the slot).
    float level_log = 0;
#if BRIGHTNESS
    float3 tint = instance_data[min(input.instance, 15u) * 4].rgb * input.color;
    float level = dot(tint, float3(0.2126, 0.7152, 0.0722)) * (material[1].z >= 0.25 ? material[1].y : 1);
    level_log = level > 0 && isfinite(level) ? clamp(log2(level), -31, 31) : -31; // 0: the mesh is drawn black
#endif
    return float4(uv, input.position.z, DISPLAY_SLOT + 1 + level_log / 64);
#endif
}

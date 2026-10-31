// Stand-in for the game's display shader in the offline fixture:
//   scene colour = gain * linear(display at uv + scroll) + offset + glass layer
// The glass layer is fixed to the screen (does not scroll): sun glare spot, fine dirt and a hard shadow edge.
// REFERENCE=1 builds the unjittered output-resolution reference (different bytecode, so the add-on ignores it).
// VR=1 takes the VR vertex shader's interpolants (stand-in for the VR display shader).
// SCREEN_T0=1: stand-in for the variant with the screen in t0, material in b4 and a texture-free alpha test.
// PAINTED=1: stand-in for a translucent material drawing a virtual painting (the Fenix A320's readouts with "FCU readouts:
// New"): a shader the fix doesn't know, the painting in t0, another material texture in t1, no alpha test.
// ALPHA=1 (with PAINTED): the painting holds its picture in alpha only, its colour black (the Fenix A320's readouts): the
// alpha is black laid over the lit backing, so the picture shows where the alpha is low (1 - alpha, as grey); the
// reference reads the gauge's colour as that grey.
#include "../src/cockpit_vertex.hlsli"

cbuffer RenderState : register(b1) { float4 render_state; };
#if SCREEN_T0
cbuffer Material : register(b4) { float4 material[17]; };
Texture2D<float4> display_texture : register(t0);
#elif PAINTED
cbuffer Material : register(b3) { float4 material[17]; };
Texture2D<float4> display_texture : register(t0);
Texture2D<float4> base_texture : register(t1);
#else
cbuffer Material : register(b3) { float4 material[17]; };
Texture2D<float4> base_texture : register(t0);
Texture2D<float4> display_texture : register(t1);
#endif
SamplerState base_sampler : register(s1);
cbuffer Instances : register(b2) { float4 instance_data[64]; }; // row 0 per instance: brightness (as the game's display shader)

float3 srgb_to_linear(float3 c) { return select(c <= 0.04045, c / 12.92, pow(c * 0.947867 + 0.0521327, 2.4)); }
float3 screen_tap(float2 uv)
{
    const float4 t = display_texture.SampleBias(base_sampler, uv, -1);
#if ALPHA && REFERENCE
    return dot(t.rgb, float3(0.2126, 0.7152, 0.0722)).xxx;
#elif ALPHA
    return (1 - t.a).xxx;
#else
    return t.rgb;
#endif
}

// material[2] = gain, [3] = offset, [4] = glare (centre uv, radius, intensity), [5] = scroll uv, dirt amount, shadow edge u
#if ROV
// ROV=1 (with PAINTED): like the sim's geometry decals, no render target: the colour goes into the scene through a
// rasterizer-ordered UAV (the sim's decals blend into its G-buffer this way).
RasterizerOrderedTexture2D<float4> scene_out : register(u0);
[earlydepthstencil]
void main(CockpitVertex input)
#elif VR
float4 main(CockpitVertexVR input) : SV_Target0
#else
float4 main(CockpitVertex input) : SV_Target0
#endif
{
#if PAINTED
    float coverage = base_texture.Sample(base_sampler, input.uv.xy).a; // translucent: blended, not alpha tested
#else
#if SCREEN_T0
    float alpha = material[1].w;
#else
    float alpha = base_texture.Sample(base_sampler, input.uv.xy).a * material[1].w;
#endif
    if (alpha <= render_state.x) discard;
    float coverage = 1;
#endif
    float2 glass_uv = input.uv.xy;
    // Same 4-tap SampleBias(-1) pattern as the game's display shader (offsets in screen-space derivatives).
    float2 uv = glass_uv + material[5].xy, dx = ddx_coarse(uv), dy = ddy_coarse(uv);
    float3 sum = screen_tap(uv + 0.125 * dx + 0.375 * dy) + screen_tap(uv - 0.125 * dx - 0.375 * dy)
               + screen_tap(uv + 0.375 * dx - 0.125 * dy) + screen_tap(uv - 0.375 * dx + 0.125 * dy);
    float3 c = srgb_to_linear(sum * 0.25);
    float2 d = (glass_uv - material[4].xy) / max(material[4].z, 1e-3);
    float glare = material[4].w * exp(-dot(d, d));
    // Smooth fine "dirt": a few octaves of sine noise in glass space.
    float dirt = 0.5 + 0.25 * sin(glass_uv.x * 157.0 + sin(glass_uv.y * 71.0) * 3.0) + 0.25 * sin(glass_uv.y * 211.0 + glass_uv.x * 53.0);
    float3 glass = material[5].z * dirt * float3(0.9, 0.85, 0.8) + glare * float3(1.0, 0.9, 0.7);
    if (material[5].w > 0 && glass_uv.x > material[5].w) glass *= 0.25; // shadow edge
    c *= instance_data[min(input.instance, 15u) * 4].rgb * input.color;
    const float4 result = float4(c * material[2].rgb + material[3].rgb + glass, (REFERENCE ? 0.5 : 1.0) * coverage);
#if ROV
    scene_out[uint2(input.position.xy)] = result;
#else
    return result;
#endif
}

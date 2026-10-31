// Stand-in for the sim's HTML renderer drawing a gauge's live view over a whole virtual painting (the Fenix A320's
// Readouts.html, --painted --rov --source): one triangle over the target, sampling the gauge texture in t0.
// ALPHA=1 (--alpha): the picture written into alpha only as black coverage (1 - grey), the colour black (as the Fenix's
// readouts painting holds it: the digits are where the alpha is low).
Texture2D<float4> source : register(t0);
SamplerState linear_sampler : register(s1);

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output vs_main(uint id : SV_VertexID)
{
    Output output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 ps_main(Output input) : SV_Target0
{
    const float4 c = source.Sample(linear_sampler, input.uv);
#if ALPHA
    return float4(0, 0, 0, 1 - dot(c.rgb, float3(0.2126, 0.7152, 0.0722)));
#else
    return c;
#endif
}

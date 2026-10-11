struct CockpitVertex
{
    float4 previous_position : PREVIOUS_POSITION;
    float4 uv : TEXCOORD0;
    float4 tangent_frame0 : TEXSPACE_MAT0;
    float4 tangent_frame1 : TEXSPACE_MAT1;
    float4 tangent_frame2 : TEXSPACE_MAT2;
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE_ID;
    float3 color : COLOR_RGB;
    float color_alpha : COLOR_ALPHA;
};

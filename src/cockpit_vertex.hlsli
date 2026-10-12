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

// VR (instanced stereo: one instance per view, each into its own render-target layer). Same interpolants plus the
// eye index; the game's VR display shader reads SV_IsFrontFace where the VR vertex shader writes the layer index.
struct CockpitVertexVR
{
    float4 previous_position : PREVIOUS_POSITION;
    float4 uv : TEXCOORD0;
    float4 tangent_frame0 : TEXSPACE_MAT0;
    float4 tangent_frame1 : TEXSPACE_MAT1;
    float4 tangent_frame2 : TEXSPACE_MAT2;
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE_ID;
    nointerpolation uint eye : EYE_ID;
    bool front_face : SV_IsFrontFace;
    float3 color : COLOR_RGB;
    float color_alpha : COLOR_ALPHA;
};

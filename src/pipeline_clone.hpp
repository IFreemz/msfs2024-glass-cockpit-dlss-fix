#pragma once
// Cloning of the game's cockpit display pipeline with a replacement pixel shader (UV map output).
#include <d3d12.h>
#include <algorithm>
#include <bitset>
#include <cstring>

namespace glass {
constexpr DXGI_FORMAT map_format = DXGI_FORMAT_R32G32B32A32_FLOAT;

inline bool clone_pipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
    D3D12_SHADER_BYTECODE pixel_shader, D3D12_GRAPHICS_PIPELINE_STATE_DESC& result) {
    if (!original.pRootSignature || !original.VS.pShaderBytecode || !original.VS.BytecodeLength ||
        !pixel_shader.pShaderBytecode || !pixel_shader.BytecodeLength ||
        original.GS.BytecodeLength || original.HS.BytecodeLength || original.DS.BytecodeLength ||
        original.StreamOutput.NumEntries || original.StreamOutput.NumStrides ||
        original.SampleDesc.Count != 1 || original.SampleDesc.Quality != 0 ||
        original.DepthStencilState.StencilEnable || original.BlendState.AlphaToCoverageEnable ||
        original.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
        original.NumRenderTargets == 0 || original.NumRenderTargets > 8 ||
        !original.DepthStencilState.DepthEnable || original.DSVFormat == DXGI_FORMAT_UNKNOWN) return false;
    result = original;
    result.PS = pixel_shader;
    result.NumRenderTargets = 1;
    for (auto& format : result.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    result.RTVFormats[0] = map_format;
    result.BlendState = {};
    for (auto& blend : result.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    result.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    // Replayed right after the game's own draw, against the depth it just wrote: only the triangle the game kept passes
    // (a screen mesh can hold layers drawn in either order, e.g. the iFly 737's CDU and radio displays have a backing
    // plate 0.2 mm off the screen, after it in the mesh). A tiny bias towards the camera (16 float steps) absorbs
    // rounding differences between the two pipelines; layers millimetres apart stay apart.
    const auto func = original.DepthStencilState.DepthFunc;
    if (func == D3D12_COMPARISON_FUNC_GREATER || func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) {
        result.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        result.RasterizerState.DepthBias = original.RasterizerState.DepthBias + 16; // on top of the game's own bias
    } else if (func == D3D12_COMPARISON_FUNC_LESS || func == D3D12_COMPARISON_FUNC_LESS_EQUAL) {
        result.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        result.RasterizerState.DepthBias = original.RasterizerState.DepthBias - 16;
    }
    result.CachedPSO = {};
    return true;
}

struct Draw {
    bool indexed = false;
    UINT count = 0, instances = 1, first = 0, first_instance = 0;
    INT base_vertex = 0;
};

// Pipeline-state stream decoding (games may create PSOs with CreatePipelineState).
#pragma warning(push)
#pragma warning(disable:4324)
template <typename T> struct alignas(void*) StreamField {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T data;
};
#pragma warning(pop)

// Only losslessly representable graphics streams are cloned.
// VR pipelines carry a view-instancing subobject (several views per draw); it is returned separately.
inline bool decode_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& stream,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& out, D3D12_VIEW_INSTANCING_DESC* view_instancing = nullptr, unsigned* unknown_type = nullptr) {
    out = {};
    if (view_instancing) *view_instancing = {};
    if (!stream.pPipelineStateSubobjectStream || !stream.SizeInBytes || stream.SizeInBytes > 8192) return false;
    auto bytes = static_cast<const unsigned char*>(stream.pPipelineStateSubobjectStream);
    std::bitset<64> seen;
    size_t position = 0;
    while (position < stream.SizeInBytes) {
        if (stream.SizeInBytes - position < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) return false;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
        std::memcpy(&type, bytes + position, sizeof(type));
        if (unsigned(type) >= seen.size() || seen[unsigned(type)]) return false;
        seen.set(unsigned(type));
#define FIELD(tag, value_type, ...) \
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: { \
            StreamField<value_type> field{}; \
            if (stream.SizeInBytes - position < sizeof(field)) return false; \
            std::memcpy(&field, bytes + position, sizeof(field)); \
            __VA_ARGS__; position += sizeof(field); break; }
        switch (type) {
        FIELD(ROOT_SIGNATURE, ID3D12RootSignature*, out.pRootSignature = field.data)
        FIELD(VS, D3D12_SHADER_BYTECODE, out.VS = field.data)
        FIELD(PS, D3D12_SHADER_BYTECODE, out.PS = field.data)
        FIELD(DS, D3D12_SHADER_BYTECODE, out.DS = field.data)
        FIELD(HS, D3D12_SHADER_BYTECODE, out.HS = field.data)
        FIELD(GS, D3D12_SHADER_BYTECODE, out.GS = field.data)
        FIELD(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC, out.StreamOutput = field.data)
        FIELD(BLEND, D3D12_BLEND_DESC, out.BlendState = field.data)
        FIELD(SAMPLE_MASK, UINT, out.SampleMask = field.data)
        FIELD(RASTERIZER, D3D12_RASTERIZER_DESC, out.RasterizerState = field.data)
        FIELD(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC, out.DepthStencilState = field.data)
        FIELD(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1,
            if (field.data.DepthBoundsTestEnable) return false;
            out.DepthStencilState = {field.data.DepthEnable, field.data.DepthWriteMask, field.data.DepthFunc,
                field.data.StencilEnable, field.data.StencilReadMask, field.data.StencilWriteMask,
                field.data.FrontFace, field.data.BackFace})
        FIELD(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, out.InputLayout = field.data)
        FIELD(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, out.IBStripCutValue = field.data)
        FIELD(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE, out.PrimitiveTopologyType = field.data)
        FIELD(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY,
            if (field.data.NumRenderTargets > 8) return false;
            out.NumRenderTargets = field.data.NumRenderTargets;
            std::copy_n(field.data.RTFormats, 8, out.RTVFormats))
        FIELD(DEPTH_STENCIL_FORMAT, DXGI_FORMAT, out.DSVFormat = field.data)
        FIELD(SAMPLE_DESC, DXGI_SAMPLE_DESC, out.SampleDesc = field.data)
        FIELD(NODE_MASK, UINT, out.NodeMask = field.data)
        FIELD(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE, out.CachedPSO = field.data)
        FIELD(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC, if (view_instancing) *view_instancing = field.data)
        FIELD(FLAGS, D3D12_PIPELINE_STATE_FLAGS,
            if (field.data != D3D12_PIPELINE_STATE_FLAG_NONE) return false;
            out.Flags = field.data)
        default: if (unknown_type) *unknown_type = unsigned(type); return false;
        }
#undef FIELD
    }
    for (const auto required : {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC})
        if (!seen[unsigned(required)]) return false;
    return seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL] ||
        seen[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1];
}
}

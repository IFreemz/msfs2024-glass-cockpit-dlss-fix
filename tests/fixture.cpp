// Offline test of the Glass Cockpit DLSS Fix add-on inside a fake FlightSimulator2024.exe with a real ReShade.
// A perspective display quad (captured cockpit VS) samples a detailed 768x768 texture at 1280x720 with per-frame
// jitter; scene colour = gain * linear(texture) + offset. A fake _nvngx.dll "evaluate" leaves the 1920x1080 output
// magenta, so after the hook every display pixel must come from the overlay. The result is compared with an
// unjittered 1920x1080 reference render of the same quad.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "../src/ngx_interface.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static HRESULT save_png(const std::filesystem::path& path, UINT width, UINT height, UINT pitch, BYTE* data) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT result = S_OK;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> converter;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
        if (SUCCEEDED(result)) result = frame->SetSize(width, height);
        if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
        if (SUCCEEDED(result)) result = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA,
            pitch, pitch * height, data, &bitmap);
        if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(result)) result = frame->WriteSource(converter.Get(), nullptr);
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void check(HRESULT hr, int line = __builtin_LINE()) {
    if (FAILED(hr)) { std::printf("line=%d hr=0x%08lx\n", line, static_cast<unsigned long>(hr)); throw std::runtime_error("D3D12 call failed"); }
}
static std::vector<char> load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "file missing");
    return {std::istreambuf_iterator<char>(input), {}};
}

constexpr UINT render_w = 1280, render_h = 720, output_w = 1920, output_h = 1080;
const std::array<float, 3> gain{1.6f, 1.2f, 0.9f}, offset{0.02f, 0.01f, 0.03f};

struct Params final : NVSDK_NGX_Parameter {
    ID3D12Resource *colour = nullptr, *output = nullptr;
    float jitter_x = 0, jitter_y = 0;
    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char*, int) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}
    void Reset() override {}
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char* key, float* value) const override {
        if (!std::strcmp(key, NVSDK_NGX_Parameter_Jitter_Offset_X)) { *value = jitter_x; return NVSDK_NGX_Result_Success; }
        if (!std::strcmp(key, NVSDK_NGX_Parameter_Jitter_Offset_Y)) { *value = jitter_y; return NVSDK_NGX_Result_Success; }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char* key, unsigned int* value) const override {
        if (!std::strcmp(key, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width)) *value = render_w;
        else if (!std::strcmp(key, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height)) *value = render_h;
        else *value = 0;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* key, ID3D12Resource** value) const override {
        *value = !std::strcmp(key, NVSDK_NGX_Parameter_Color) ? colour : !std::strcmp(key, NVSDK_NGX_Parameter_Output) ? output : nullptr;
        return *value ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char* key, void** value) const override {
        ID3D12Resource* r = nullptr; const auto result = Get(key, &r); *value = r; return result;
    }
};

struct Vertex { float position[4], tangent[4], normal[3], uv[4], color[4]; };
struct Constants {
    std::array<std::array<float, 4>, 300> global{}; // b0: row-major view-projection in the first four rows
    std::array<std::array<float, 4>, 192> world{};
    std::array<std::array<float, 4>, 17> object{};
};
// Display quad turned 35 degrees around Y, 2 units in front of a 60-degree 16:9 camera (perspective-correct UVs).
static Constants constants(float shift) {
    using M = std::array<std::array<float, 4>, 4>;
    auto multiply = [](const M& a, const M& b) {
        M r{};
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
        return r;
    };
    const float angle = 0.61f, c = std::cos(angle), sn = std::sin(angle);
    const M world{{{c, 0, -sn, 0}, {0, 1, 0, 0}, {sn, 0, c, 0}, {shift, 0, 2.0f, 1}}};
    const float ys = 1.0f / std::tan(0.5236f), xs = ys / (16.0f / 9.0f), zn = 0.1f, zf = 100.0f;
    const M projection{{{xs, 0, 0, 0}, {0, ys, 0, 0}, {0, 0, zf / (zf - zn), 1}, {0, 0, -zn * zf / (zf - zn), 0}}};
    const auto view_projection = multiply(world, projection);
    Constants result;
    for (int i = 0; i < 4; ++i) result.global[i] = view_projection[i];
    return result;
}

static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{type};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static void upload(ID3D12Resource* destination, const void* source, size_t bytes) {
    void* data = nullptr; D3D12_RANGE none{};
    check(destination->Map(0, &none, &data)); std::memcpy(data, source, bytes); destination->Unmap(0, nullptr);
}
static ComPtr<ID3D12Resource> texture(ID3D12Device* device, UINT w, UINT h, UINT16 mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, mips, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&r)));
    return r;
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
struct Readback { ComPtr<ID3D12Resource> buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0; };
static Readback readback(ID3D12Device* device, ID3D12Resource* source) {
    Readback r; const auto desc = source->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &r.bytes);
    r.buffer = buffer(device, r.bytes, D3D12_HEAP_TYPE_READBACK);
    return r;
}
static void copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source, const Readback& r) {
    D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, to{r.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = r.footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}
static float f11(uint32_t bits, int mantissa_bits) {
    const uint32_t mantissa = bits & ((1u << mantissa_bits) - 1), exponent = bits >> mantissa_bits;
    if (exponent == 0) return std::ldexp(float(mantissa), -14 - mantissa_bits);
    if (exponent == 31) return INFINITY;
    return std::ldexp(1.0f + float(mantissa) / float(1u << mantissa_bits), int(exponent) - 15);
}
static uint32_t encode_float(float v, int mantissa_bits) {
    if (!(v > 0)) return 0;
    int exponent = 0;
    const float m = std::frexp(v, &exponent); // v = m * 2^exponent, m in [0.5, 1)
    int e = exponent - 1 + 15;
    if (e <= 0) return uint32_t(std::lround(std::ldexp(v, 14 + mantissa_bits))) & ((1u << mantissa_bits) - 1);
    if (e >= 31) return (30u << mantissa_bits) | ((1u << mantissa_bits) - 1);
    uint32_t mantissa = uint32_t(std::lround((m * 2 - 1) * (1 << mantissa_bits)));
    if (mantissa == (1u << mantissa_bits)) { mantissa = 0; ++e; }
    return (uint32_t(e) << mantissa_bits) | mantissa;
}
static uint32_t encode_r11g11b10(std::array<float, 3> c) { return encode_float(c[0], 6) | (encode_float(c[1], 6) << 11) | (encode_float(c[2], 5) << 22); }
static std::array<float, 3> r11g11b10(uint32_t v) { return {f11(v & 0x7ff, 6), f11((v >> 11) & 0x7ff, 6), f11(v >> 22, 5)}; }

// PFD-like content: black background, thin white/green/magenta lines and blocky "glyphs".
static std::array<uint8_t, 4> pattern(UINT x, UINT y) {
    if (y % 48 < 3) return {255, 255, 255, 255};
    if (x % 96 < 2) return {0, 255, 0, 255};
    if ((x / 12 + y / 16) % 7 == 0 && (y % 48) > 10 && (y % 48) < 30) return {255, 0, 255, 255};
    if (((x * 7 + y * 3) / 5) % 23 == 0) return {200, 200, 200, 255};
    return {0, 0, 0, 255};
}

int main(int, char** argv) {
    try {
        const auto here = std::filesystem::path(argv[0]).parent_path();
        require(LoadLibraryW((here / L"_nvngx.dll").c_str()) != nullptr, "fake _nvngx.dll missing");
        if (!LoadLibraryW((here / L"ReShade64.asi").c_str())) { std::printf("LoadLibrary(ReShade64.asi) failed, error %lu\n", GetLastError()); return 2; }
        const auto vs = load(here / "fixture_vs.cso");
        const auto ps = load(here / "fixture_ps.cso"), reference_ps = load(here / "reference_ps.cso");

        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        const auto addon = GetModuleHandleW(L"glass-cockpit-dlss-fix.addon64");
        require(addon != nullptr, "overlay add-on not loaded by ReShade");
        const auto set_jitter = reinterpret_cast<void (*)(bool)>(GetProcAddress(addon, "GlassFixJitterPositive"));
        const auto evaluate = reinterpret_cast<NgxEvaluate>(GetProcAddress(GetModuleHandleW(L"_nvngx.dll"), "NVSDK_NGX_D3D12_EvaluateFeature"));
        require(set_jitter && evaluate, "exports missing");

        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        UINT64 fence_value = 0;
        auto submit = [&] {
            check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
            check(queue->Signal(fence.Get(), ++fence_value));
            const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(fence->SetEventOnCompletion(fence_value, e)); require(WaitForSingleObject(e, 10000) == WAIT_OBJECT_0, "GPU timeout"); CloseHandle(e);
            require(device->GetDeviceRemovedReason() == S_OK, "device removed");
            check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr));
        };

        // Root signature shaped like the game's: VS b0-b2, PS b1/b3, SRV table t0-t1, static s1.
        std::array<D3D12_ROOT_PARAMETER, 6> params{};
        for (UINT i = 0; i < 5; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[i].ShaderVisibility = i < 3 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
            params[i].Descriptor.ShaderRegister = i < 3 ? i : i == 3 ? 1 : 3;
        }
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[5].DescriptorTable = {1, &range};
        D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX, 1, 0,
            D3D12_SHADER_VISIBILITY_PIXEL};
        D3D12_ROOT_SIGNATURE_DESC rsd{UINT(params.size()), params.data(), 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob, blob_error;
        check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &blob_error));
        ComPtr<ID3D12RootSignature> rs; check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));

        const D3D12_INPUT_ELEMENT_DESC layout[]{
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get(); pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()};
        pd.InputLayout = {layout, UINT(std::size(layout))};
        pd.SampleMask = UINT_MAX; pd.SampleDesc.Count = 1; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE}; pd.RasterizerState.DepthClipEnable = TRUE;
        for (auto& b : pd.BlendState.RenderTarget) {
            b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ZERO;
            b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD; b.LogicOp = D3D12_LOGIC_OP_NOOP; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ComPtr<ID3D12PipelineState> game_pso; check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&game_pso)));
        auto rd = pd; rd.PS = {reference_ps.data(), reference_ps.size()}; rd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ComPtr<ID3D12PipelineState> reference_pso; check(device->CreateGraphicsPipelineState(&rd, IID_PPV_ARGS(&reference_pso)));

        // Mesh, constants.
        std::array<Vertex, 7> vertices{};
        const float positions[6][2]{{-0.7f, 0.7f}, {0.7f, 0.7f}, {-0.7f, -0.7f}, {-0.7f, -0.7f}, {0.7f, 0.7f}, {0.7f, -0.7f}};
        for (size_t i = 0; i < 6; ++i) {
            auto& v = vertices[i + 1];
            v.position[0] = positions[i][0]; v.position[1] = positions[i][1]; v.position[2] = 0.4f; v.position[3] = 1;
            v.tangent[0] = v.tangent[3] = v.normal[2] = 1;
            v.uv[0] = (positions[i][0] + 0.7f) / 1.4f; v.uv[1] = (0.7f - positions[i][1]) / 1.4f;
            std::fill(std::begin(v.color), std::end(v.color), 1.0f);
        }
        auto vb = buffer(device.Get(), sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD); upload(vb.Get(), vertices.data(), sizeof(vertices));
        constexpr UINT16 indices[]{0, 1, 2, 3, 4, 5};
        auto ib = buffer(device.Get(), sizeof(indices), D3D12_HEAP_TYPE_UPLOAD); upload(ib.Get(), indices, sizeof(indices));
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex)};
        const D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
        std::array<ComPtr<ID3D12Resource>, 2> globals, worlds, objects;
        for (int k = 0; k < 2; ++k) {
            const auto c = constants(k ? 0.25f : 0.0f);
            globals[k] = buffer(device.Get(), sizeof(c.global), D3D12_HEAP_TYPE_UPLOAD); upload(globals[k].Get(), c.global.data(), sizeof(c.global));
            worlds[k] = buffer(device.Get(), sizeof(c.world), D3D12_HEAP_TYPE_UPLOAD); upload(worlds[k].Get(), c.world.data(), sizeof(c.world));
            objects[k] = buffer(device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD); upload(objects[k].Get(), c.object.data(), sizeof(c.object));
        }
        auto render_cb = buffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD);
        const float threshold[4]{0, 0, 0, 0}; upload(render_cb.Get(), threshold, sizeof(threshold));
        std::array<std::array<float, 4>, 17> material{};
        material[1][3] = 1;
        material[2] = {gain[0], gain[1], gain[2], 0}; material[3] = {offset[0], offset[1], offset[2], 0};
        std::array<ComPtr<ID3D12Resource>, 2> materials;
        for (int glare = 0; glare < 2; ++glare) {
            // Sun glare: a soft bright spot on one corner of the display (uv 0.25,0.3, radius 0.15).
            material[4] = glare ? std::array<float, 4>{0.25f, 0.3f, 0.15f, 0.8f} : std::array<float, 4>{};
            materials[glare] = buffer(device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD); upload(materials[glare].Get(), material.data(), sizeof(material));
        }

        // Textures: 2x1 alpha texture (t0) and a 768x768 5-mip render-target display texture (t1).
        auto alpha = texture(device.Get(), 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto display = texture(device.Get(), 768, 768, 5, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        // The display texture is rewritten whenever its content scrolls (like the sim's instrument renderer does).
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 5> display_fp{}; UINT64 display_total = 0;
        { const auto dd = display->GetDesc(); device->GetCopyableFootprints(&dd, 0, 5, 0, display_fp.data(), nullptr, nullptr, &display_total); }
        auto display_staging = buffer(device.Get(), display_total, D3D12_HEAP_TYPE_UPLOAD);
        bool display_initialised = false;
        auto upload_display = [&](UINT scroll) {
            std::vector<uint8_t> bytes(size_t(display_total), 0);
            std::vector<std::array<float, 4>> level(768 * 768);
            for (UINT y = 0; y < 768; ++y) for (UINT x = 0; x < 768; ++x) { const auto p = pattern(x, (y + scroll) % 768); level[y * 768 + x] = {float(p[0]), float(p[1]), float(p[2]), float(p[3])}; }
            UINT size = 768;
            for (UINT m = 0; m < 5; ++m) {
                for (UINT y = 0; y < size; ++y) for (UINT x = 0; x < size; ++x)
                    for (int c = 0; c < 4; ++c) bytes[size_t(display_fp[m].Offset + y * display_fp[m].Footprint.RowPitch + x * 4 + c)] = uint8_t(std::lround(level[y * size + x][c]));
                const UINT next = size / 2;
                std::vector<std::array<float, 4>> down(size_t(next) * next);
                for (UINT y = 0; y < next; ++y) for (UINT x = 0; x < next; ++x) for (int c = 0; c < 4; ++c)
                    down[y * next + x][c] = 0.25f * (level[(2 * y) * size + 2 * x][c] + level[(2 * y) * size + 2 * x + 1][c] + level[(2 * y + 1) * size + 2 * x][c] + level[(2 * y + 1) * size + 2 * x + 1][c]);
                level = std::move(down); size = next;
            }
            upload(display_staging.Get(), bytes.data(), bytes.size());
            constexpr auto readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (display_initialised) transition(list.Get(), display.Get(), readable, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT m = 0; m < 5; ++m) {
                D3D12_TEXTURE_COPY_LOCATION to{display.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{display_staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                to.SubresourceIndex = m; from.PlacedFootprint = display_fp[m];
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), display.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable);
            display_initialised = true;
            submit();
        };
        upload_display(0);
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT afp{}; UINT64 atotal = 0; const auto ad = alpha->GetDesc();
            device->GetCopyableFootprints(&ad, 0, 1, 0, &afp, nullptr, nullptr, &atotal);
            std::vector<uint8_t> abytes(size_t(atotal), 255);
            auto astaging = buffer(device.Get(), atotal, D3D12_HEAP_TYPE_UPLOAD); upload(astaging.Get(), abytes.data(), abytes.size());
            D3D12_TEXTURE_COPY_LOCATION ato{alpha.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, afrom{astaging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            afrom.PlacedFootprint = afp; list->CopyTextureRegion(&ato, 0, 0, 0, &afrom, nullptr);
            transition(list.Get(), alpha.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            submit();
        }
        // Descriptors created in a CPU heap and copied into the shader-visible heap, like the game.
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ComPtr<ID3D12DescriptorHeap> cpu_heap, gpu_heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpu_heap)));
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu_heap)));
        auto cpu0 = cpu_heap->GetCPUDescriptorHandleForHeapStart(), gpu0 = gpu_heap->GetCPUDescriptorHandleForHeapStart();
        auto cpu1 = cpu0; cpu1.ptr += inc;
        device->CreateShaderResourceView(alpha.Get(), nullptr, cpu0);
        device->CreateShaderResourceView(display.Get(), nullptr, cpu1);
        auto gpu_slot = gpu0; gpu_slot.ptr += 4 * inc; // table at slot 4
        device->CopyDescriptorsSimple(2, gpu_slot, cpu0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto table = gpu_heap->GetGPUDescriptorHandleForHeapStart(); table.ptr += 4 * inc;

        // Targets.
        D3D12_CLEAR_VALUE black{DXGI_FORMAT_R11G11B10_FLOAT, {}};
        auto scene = texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &black);
        D3D12_CLEAR_VALUE depth_clear{DXGI_FORMAT_D32_FLOAT}; depth_clear.DepthStencil.Depth = 1;
        auto depth = texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear);
        const float magenta[4]{1, 0, 1, 1};
        D3D12_CLEAR_VALUE magenta_clear{DXGI_FORMAT_R11G11B10_FLOAT, {1, 0, 1, 1}};
        auto output = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_R11G11B10_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET, &magenta_clear);
        D3D12_CLEAR_VALUE ref_clear{DXGI_FORMAT_R32G32B32A32_FLOAT, {}};
        auto reference = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &ref_clear);
        auto ref_depth = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear);
        ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}, dh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        check(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap))); check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsv_heap)));
        const UINT rinc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), dinc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto rtv = [&](UINT i) { auto h = rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rinc; return h; };
        auto dsv = [&](UINT i) { auto h = dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * dinc; return h; };
        device->CreateRenderTargetView(scene.Get(), nullptr, rtv(0));
        device->CreateRenderTargetView(output.Get(), nullptr, rtv(1));
        device->CreateRenderTargetView(reference.Get(), nullptr, rtv(2));
        device->CreateDepthStencilView(depth.Get(), nullptr, dsv(0));
        device->CreateDepthStencilView(ref_depth.Get(), nullptr, dsv(1));

        auto bind = [&](ID3D12PipelineState* pso, int variant) {
            const int camera = variant == 1 ? 1 : 0;
            list->SetGraphicsRootSignature(rs.Get());
            ID3D12DescriptorHeap* heaps[]{gpu_heap.Get()}; list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(pso);
            list->SetGraphicsRootConstantBufferView(0, globals[camera]->GetGPUVirtualAddress());
            list->SetGraphicsRootConstantBufferView(1, worlds[camera]->GetGPUVirtualAddress());
            list->SetGraphicsRootConstantBufferView(2, objects[camera]->GetGPUVirtualAddress());
            list->SetGraphicsRootConstantBufferView(3, render_cb->GetGPUVirtualAddress());
            list->SetGraphicsRootConstantBufferView(4, materials[0]->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(5, table);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->IASetVertexBuffers(0, 1, &vbv); list->IASetIndexBuffer(&ibv);
        };

        // Per-frame scene description. Material is rewritten every frame (each frame waits for the GPU).
        struct Frame {
            const char* name; int camera; bool draw, jitter_positive; float jx, jy;
            UINT scroll; float dirt, shadow_u; bool glare;
            enum Prefill { magenta, perfect, previous } prefill; // what the fake DLSS leaves in the output
            enum Check { info, quality, untouched } check;
        };
        const Frame frames[]{
            {"warmup", 0, true, true, 0.25f, -0.17f, 0, 0.06f, 0.6f, false, Frame::perfect, Frame::info},
            {"warmup2", 0, true, true, -0.33f, 0.41f, 0, 0.06f, 0.6f, false, Frame::perfect, Frame::info},
            {"static_a", 0, true, true, 0.25f, -0.17f, 0, 0.06f, 0.6f, false, Frame::perfect, Frame::quality},
            {"static_b", 0, true, true, -0.33f, 0.41f, 0, 0.06f, 0.6f, false, Frame::perfect, Frame::quality},
            {"flipped", 0, true, false, 0.45f, 0.30f, 0, 0.06f, 0.6f, false, Frame::perfect, Frame::info},
            {"no_dlss_info", 0, true, true, 0.45f, 0.30f, 0, 0.06f, 0.6f, false, Frame::magenta, Frame::info},
            {"no_display", 0, false, true, 0.1f, 0.1f, 0, 0.06f, 0.6f, false, Frame::magenta, Frame::untouched},
            // Scrolling tape with dirt, glare and a hard shadow edge; the fake DLSS shows the previous frame (worst-case ghost).
            {"scroll_1", 0, true, true, 0.3f, 0.2f, 8, 0.06f, 0.6f, true, Frame::previous, Frame::info},
            {"scroll_2", 0, true, true, -0.1f, 0.35f, 16, 0.06f, 0.6f, true, Frame::previous, Frame::info},
            {"scroll_3", 0, true, true, 0.2f, -0.3f, 24, 0.06f, 0.6f, true, Frame::previous, Frame::quality},
            {"scroll_4", 0, true, true, -0.4f, 0.1f, 32, 0.06f, 0.6f, true, Frame::previous, Frame::quality},
            {"scroll_5", 0, true, true, 0.05f, 0.45f, 40, 0.06f, 0.6f, true, Frame::previous, Frame::quality},
            {"moved", 1, true, true, -0.2f, -0.4f, 40, 0.06f, 0.6f, true, Frame::previous, Frame::quality},
        };
        auto set_material = [&](const Frame& f) {
            material[4] = f.glare ? std::array<float, 4>{0.25f, 0.3f, 0.15f, 0.8f} : std::array<float, 4>{};
            material[5] = {0, 0, f.dirt, f.shadow_u};
            upload(materials[0].Get(), material.data(), sizeof(material));
        };
        auto render_reference = [&](int camera) {
            const float zero[4]{};
            list->ClearRenderTargetView(rtv(2), zero, 0, nullptr);
            list->ClearDepthStencilView(dsv(1), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            bind(reference_pso.Get(), camera);
            const D3D12_VIEWPORT vp{0, 0, float(output_w), float(output_h), 0, 1}; const D3D12_RECT sc{0, 0, LONG(output_w), LONG(output_h)};
            list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
            auto r2 = rtv(2), d1 = dsv(1); list->OMSetRenderTargets(1, &r2, FALSE, &d1);
            list->DrawIndexedInstanced(6, 1, 0, 1, 0);
            transition(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto rb = readback(device.Get(), reference.Get()); copy(list.Get(), reference.Get(), rb);
            transition(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            submit();
            std::vector<std::array<float, 4>> result(size_t(output_w) * output_h);
            void* mapped = nullptr; check(rb.buffer->Map(0, nullptr, &mapped));
            for (UINT y = 0; y < output_h; ++y)
                std::memcpy(&result[size_t(y) * output_w], static_cast<uint8_t*>(mapped) + rb.footprint.Offset + size_t(y) * rb.footprint.Footprint.RowPitch, size_t(output_w) * 16);
            rb.buffer->Unmap(0, nullptr);
            return result;
        };

        Params ngx; ngx.colour = scene.Get(); ngx.output = output.Get();
        auto out_rb = readback(device.Get(), output.Get());
        auto prefill_upload = buffer(device.Get(), out_rb.bytes, D3D12_HEAP_TYPE_UPLOAD);
        std::vector<std::array<float, 4>> previous_ref;
        std::vector<uint32_t> prefill(size_t(output_w) * output_h);
        bool all_passed = true;
        for (const auto& f : frames) {
            set_jitter(f.jitter_positive);
            if (!std::strcmp(f.name, "warmup2")) Sleep(1500); // let the background PSO compile finish
            set_material(f);
            static UINT current_scroll = 0;
            if (f.scroll != current_scroll) { upload_display(f.scroll); current_scroll = f.scroll; }
            const int variant = f.camera == 1 ? 1 : (f.glare ? 2 : 0);
            const auto ref = render_reference(variant);
            // Fake DLSS output: the current unjittered image (perfect), the previous frame's (ghost) or magenta.
            const auto& source = f.prefill == Frame::perfect ? ref : previous_ref.empty() ? ref : previous_ref;
            for (size_t i = 0; i < prefill.size(); ++i)
                prefill[i] = f.prefill == Frame::magenta || source[i][3] <= 0 ? encode_r11g11b10({1, 0, 1}) : encode_r11g11b10({source[i][0], source[i][1], source[i][2]});
            {
                void* data = nullptr; check(prefill_upload->Map(0, nullptr, &data));
                for (UINT y = 0; y < output_h; ++y)
                    std::memcpy(static_cast<uint8_t*>(data) + out_rb.footprint.Offset + size_t(y) * out_rb.footprint.Footprint.RowPitch, &prefill[size_t(y) * output_w], size_t(output_w) * 4);
                prefill_upload->Unmap(0, nullptr);
            }
            list->ClearRenderTargetView(rtv(0), black.Color, 0, nullptr);
            list->ClearDepthStencilView(dsv(0), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            if (f.draw) {
                bind(game_pso.Get(), variant);
                // Jitter: geometry displaced by +J render pixels (y down).
                const D3D12_VIEWPORT vp{f.jx, f.jy, float(render_w), float(render_h), 0, 1}; const D3D12_RECT sc{0, 0, LONG(render_w), LONG(render_h)};
                list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
                auto r0 = rtv(0), d0 = dsv(0); list->OMSetRenderTargets(1, &r0, FALSE, &d0);
                list->DrawIndexedInstanced(6, 1, 0, 1, 0);
            }
            transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            {
                D3D12_TEXTURE_COPY_LOCATION to{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{prefill_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                from.PlacedFootprint = out_rb.footprint;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ngx.jitter_x = f.jx; ngx.jitter_y = f.jy;
            require(evaluate(list.Get(), nullptr, &ngx, nullptr) == NVSDK_NGX_Result_Success, "evaluate failed");
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy(list.Get(), output.Get(), out_rb);
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            submit();

            // Compare with the current reference; "ghost" = what the fake DLSS output alone would show.
            void* mapped = nullptr; check(out_rb.buffer->Map(0, nullptr, &mapped));
            uint64_t covered = 0, replaced = 0, stray = 0, interior = 0;
            double error = 0, ghost = 0, max_error = 0;
            std::vector<BYTE> png(size_t(output_w) * output_h * 4);
            for (UINT y = 0; y < output_h; ++y) for (UINT x = 0; x < output_w; ++x) {
                const size_t i = size_t(y) * output_w + x;
                const uint32_t bits = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(mapped) + out_rb.footprint.Offset + size_t(y) * out_rb.footprint.Footprint.RowPitch + x * 4);
                const auto c = r11g11b10(bits), before = r11g11b10(prefill[i]);
                const bool changed = bits != prefill[i];
                const bool in_ref = ref[i][3] > 0;
                bool inner = in_ref;
                for (int dy = -3; dy <= 3 && inner; ++dy) for (int dx = -3; dx <= 3 && inner; ++dx) {
                    const int xx = int(x) + dx, yy = int(y) + dy;
                    inner = xx >= 0 && yy >= 0 && xx < int(output_w) && yy < int(output_h) && ref[size_t(yy) * output_w + xx][3] > 0;
                }
                covered += in_ref; replaced += changed;
                if (changed && !in_ref) ++stray;
                if (inner && f.draw) {
                    ++interior;
                    double e = 0, g = 0;
                    for (int k = 0; k < 3; ++k) { e += std::abs(double(c[k]) - ref[i][k]); g += std::abs(double(before[k]) - ref[i][k]); }
                    error += e / 3; ghost += g / 3; max_error = std::max(max_error, e / 3);
                }
                auto p = png.data() + i * 4;
                for (int k = 0; k < 3; ++k) p[k] = BYTE(std::clamp(c[k], 0.0f, 1.0f) * 255);
                p[3] = 255;
            }
            out_rb.buffer->Unmap(0, nullptr);
            save_png(here / (std::string("overlay-") + f.name + ".png"), output_w, output_h, output_w * 4, png.data());
            const double mean = interior ? error / interior : 0, ghost_mean = interior ? ghost / interior : 0;
            bool pass = true;
            if (f.check == Frame::quality) pass = interior > 10000 && mean < 0.02 && stray < covered / 50;
            if (f.check == Frame::untouched) pass = replaced == 0;
            std::printf("%-12s covered=%llu replaced=%llu stray=%llu error=%.4f (dlss_alone=%.4f) max=%.3f %s\n", f.name, covered, replaced, stray,
                mean, ghost_mean, max_error, f.check == Frame::info ? "(info)" : pass ? "PASS" : "FAIL");
            all_passed &= pass;
            previous_ref = ref;
        }
        std::printf(all_passed ? "ALL PASSED\n" : "FAILED\n");
        return all_passed ? 0 : 1;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}

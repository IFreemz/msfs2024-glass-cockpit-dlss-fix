// Offline test of the Glass Cockpit DLSS Fix add-on inside a fake FlightSimulator2024.exe with a real ReShade.
// A perspective display quad (captured cockpit VS) samples a detailed 768x768 texture at 1280x720 with per-frame
// jitter; scene colour = gain * linear(texture) + offset. A fake _nvngx.dll "evaluate" leaves the 1920x1080 output
// magenta, so after the hook every display pixel must come from the overlay. The result is compared with an
// unjittered 1920x1080 reference render of the same quad. With --fsr the frames go through a fake
// amd_fidelityfx_dx12.dll ffxDispatch instead, with the colour input left in a pixel-shader-only state. With --vr
// the display is drawn like the sim's VR mode: one instanced draw, one eye per instance into 2-layer targets, then
// one upscaler call per eye (each eye's layer copied to the colour input) checked against that eye's reference. Like
// the sim with foveated rendering, the eyes are upscaled out of layer order (eye 1 first) and each result is then
// copied into its layer of a final layered image, from which the add-on learns the order.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "../src/ngx_interface.hpp"
#include "../src/ffx_interface.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
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
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr, UINT16 layers = 1) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, layers, mips, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
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

// --hotpatch: another mod (the RTX 40 MFG unlock) redirects DLSS evaluate before ReShade loads: a two-byte jump back
// over the export's first bytes to a jump placed just before it, which leads (through a nearby jump to absolute) to
// that mod's handler, which calls the real work itself. The fix can't hook such a function directly.
static NgxEvaluate fake_body = nullptr;
static unsigned mfg_handler_calls = 0;
static NVSDK_NGX_Result mfg_handler(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback progress) {
    ++mfg_handler_calls;
    return fake_body(list, handle, parameters, progress);
}
static void redirect_evaluate_like_mfg_unlock() {
    const auto ngx = GetModuleHandleW(L"_nvngx.dll");
    auto* export_address = reinterpret_cast<uint8_t*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    fake_body = reinterpret_cast<NgxEvaluate>(GetProcAddress(ngx, "FakeEvaluateBody"));
    require(export_address && fake_body, "fake NGX exports missing");
    for (int i = 1; i <= 5; ++i) require(export_address[-i] == 0xcc || export_address[-i] == 0x90, "no padding before the fake evaluate");
    // A page within 2 GB of the export for the absolute jump to the handler.
    uint8_t* page = nullptr;
    for (uintptr_t at = (reinterpret_cast<uintptr_t>(export_address) & ~uintptr_t(0xffff)) - 0x10000; !page && at > 0x10000; at -= 0x10000)
        page = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    require(page != nullptr, "no page near the fake NGX");
    const auto handler = reinterpret_cast<uint64_t>(&mfg_handler);
    page[0] = 0xff; page[1] = 0x25; std::memset(page + 2, 0, 4); std::memcpy(page + 6, &handler, 8);
    DWORD old = 0;
    require(VirtualProtect(export_address - 5, 16, PAGE_EXECUTE_READWRITE, &old), "VirtualProtect failed");
    const int32_t rel = int32_t(page - export_address);
    export_address[-5] = 0xe9; std::memcpy(export_address - 4, &rel, 4);
    // The rest of the old first instruction (mov [rsp+8], rbx) stays, as in the real DLSS runtime with that mod.
    const uint8_t patched[5]{0xeb, 0xf9, 0x5c, 0x24, 0x08};
    std::memcpy(export_address, patched, sizeof(patched));
    VirtualProtect(export_address - 5, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), export_address - 5, 16);
    std::printf("DLSS evaluate redirected like the RTX 40 MFG unlock\n");
}

int main(int argc, char** argv) {
    try {
        const auto here = std::filesystem::path(argv[0]).parent_path();
        require(LoadLibraryW((here / L"_nvngx.dll").c_str()) != nullptr, "fake _nvngx.dll missing");
        bool fsr = false, vr = false, screen_t0 = false, reload = false, hotpatch = false, bench = false, nested = false, optiscaler_fsr = false,
            ngx_proxy = false, extra_pass = false, motion = false;
        for (int i = 1; i < argc; ++i) {
            fsr |= !std::strcmp(argv[i], "--fsr"); vr |= !std::strcmp(argv[i], "--vr"); screen_t0 |= !std::strcmp(argv[i], "--screen-t0");
            reload |= !std::strcmp(argv[i], "--reload"); hotpatch |= !std::strcmp(argv[i], "--hotpatch"); bench |= !std::strcmp(argv[i], "--bench");
            nested |= !std::strcmp(argv[i], "--nested");
            extra_pass |= !std::strcmp(argv[i], "--extra-pass");
            optiscaler_fsr |= !std::strcmp(argv[i], "--optiscaler-fsr");
            ngx_proxy |= !std::strcmp(argv[i], "--ngx-proxy");
            motion |= !std::strcmp(argv[i], "--motion");
        }
        if (hotpatch) redirect_evaluate_like_mfg_unlock();
        if (screen_t0) std::printf("display: variant with the screen texture in t0\n");
        const UINT views = vr ? 2 : 1;
        if (vr) std::printf("display: VR (instanced stereo, %u eyes)\n", views);
        FfxDispatch ffx_dispatch = nullptr;
        // --ngx-proxy: the sim's DLSS goes to "OptiScaler", which answers with FSR from its own FidelityFX file.
        NgxEvaluate proxy_evaluate = nullptr;
        void (*proxy_answer_with_fsr)(FfxDispatch, ffxDispatchDescUpscale*) = nullptr;
        if (ngx_proxy) {
            const auto proxy = LoadLibraryW((here / L"OptiScaler.asi").c_str());
            const auto upscaler = LoadLibraryW((here / L"OptiScaler" / L"amd_fidelityfx_upscaler_dx12.dll").c_str());
            require(proxy && upscaler, "fake OptiScaler missing");
            proxy_evaluate = reinterpret_cast<NgxEvaluate>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_EvaluateFeature"));
            proxy_answer_with_fsr = reinterpret_cast<decltype(proxy_answer_with_fsr)>(GetProcAddress(proxy, "FakeProxyAnswerWithFsr"));
            // Like OptiScaler: its FSR runs through its own trampoline to the original code, past the add-on's ffxDispatch hooks.
            const auto upscaler_dispatch = reinterpret_cast<FfxDispatch>(GetProcAddress(upscaler, "FakeFfxDispatchBody"));
            require(proxy_evaluate && proxy_answer_with_fsr && upscaler_dispatch, "fake OptiScaler exports missing");
            ffx_dispatch = upscaler_dispatch;
            std::printf("upscaler: DLSS answered by OptiScaler with FSR from its own files\n");
        }
        void (*ffx_run_dlss_inside)(NgxEvaluate, const NVSDK_NGX_Parameter*) = nullptr;
        if (fsr) {
            // --optiscaler-fsr: FSR from another mod's FidelityFX upscaler file in its own folder (OptiScaler), not the sim's.
            const auto ffx = LoadLibraryW((optiscaler_fsr ? here / L"OptiScaler" / L"amd_fidelityfx_upscaler_dx12.dll" : here / L"amd_fidelityfx_dx12.dll").c_str());
            require(ffx != nullptr, "fake amd_fidelityfx_dx12.dll missing");
            if (optiscaler_fsr) std::printf("upscaler: FSR from OptiScaler\\amd_fidelityfx_upscaler_dx12.dll\n");
            ffx_dispatch = reinterpret_cast<FfxDispatch>(GetProcAddress(ffx, "ffxDispatch"));
            require(ffx_dispatch != nullptr, "ffxDispatch missing");
            std::printf("upscaler: FSR (fake ffxDispatch)\n");
            if (nested) {
                ffx_run_dlss_inside = reinterpret_cast<decltype(ffx_run_dlss_inside)>(GetProcAddress(ffx, "FakeFfxRunDlssInside"));
                require(ffx_run_dlss_inside != nullptr, "FakeFfxRunDlssInside missing");
                std::printf("upscaler: FSR answered with DLSS inside its dispatch (like OptiScaler)\n");
            }
        }
        if (!LoadLibraryW((here / L"ReShade64.asi").c_str())) { std::printf("LoadLibrary(ReShade64.asi) failed, error %lu\n", GetLastError()); return 2; }
        const auto vs = load(here / (vr ? "fixture_vr_vs.cso" : "fixture_vs.cso"));
        const std::string kind = std::string(vr ? "_vr" : "") + (screen_t0 ? "_t0" : "");
        const auto ps = load(here / ("fixture" + kind + "_ps.cso")), reference_ps = load(here / ("reference" + kind + "_ps.cso"));
        const auto reference_vs = vr ? load(here / "reference_vr_vs_eye0.cso") : vs, reference_vs_eye1 = vr ? load(here / "reference_vr_vs_eye1.cso") : vs;

        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
        if (reload) { // like OptiScaler at startup: a device created and released first makes ReShade unload and reload add-ons
            ComPtr<ID3D12Device> early; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&early)));
            early.Reset();
            std::printf("early device created and released\n");
        }
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        const auto addon = GetModuleHandleW(L"glass-cockpit-dlss-fix.addon64");
        require(addon != nullptr, "overlay add-on not loaded by ReShade");
        const auto set_jitter = reinterpret_cast<void (*)(bool)>(GetProcAddress(addon, "GlassFixJitterPositive"));
        const auto evaluate = reinterpret_cast<NgxEvaluate>(GetProcAddress(GetModuleHandleW(L"_nvngx.dll"), "NVSDK_NGX_D3D12_EvaluateFeature"));
        require(set_jitter && evaluate, "exports missing");
        // The reference is the game's own sharp sampling; smooth lines deliberately differ from it.
        const auto set_smooth = reinterpret_cast<void (*)(int)>(GetProcAddress(addon, "GlassFixLineSmoothing"));
        require(set_smooth != nullptr, "exports missing");
        set_smooth(0);

        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        UINT64 fence_value = 0;
        const auto cost_tick = reinterpret_cast<void (*)(ID3D12CommandQueue*)>(GetProcAddress(addon, "GlassFixCostTick"));
        require(cost_tick != nullptr, "exports missing");
        auto submit = [&] {
            check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
            check(queue->Signal(fence.Get(), ++fence_value));
            const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(fence->SetEventOnCompletion(fence_value, e)); require(WaitForSingleObject(e, 10000) == WAIT_OBJECT_0, "GPU timeout"); CloseHandle(e);
            require(device->GetDeviceRemovedReason() == S_OK, "device removed");
            check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr));
            cost_tick(queue.Get()); // stands in for the present event: drives the add-on's cost measurement
        };

        // Root signature shaped like the game's: VS b0-b2, PS b1/b3, SRV table t0-t1, static s1.
        std::array<D3D12_ROOT_PARAMETER, 6> params{};
        for (UINT i = 0; i < 5; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[i].ShaderVisibility = i < 3 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
            params[i].Descriptor.ShaderRegister = i < 3 ? i : i == 3 ? 1 : screen_t0 ? 4 : 3; // variant: material in b4
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
        auto rd = pd; rd.VS = {reference_vs.data(), reference_vs.size()}; rd.PS = {reference_ps.data(), reference_ps.size()}; rd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ComPtr<ID3D12PipelineState> reference_pso; check(device->CreateGraphicsPipelineState(&rd, IID_PPV_ARGS(&reference_pso)));
        rd.VS = {reference_vs_eye1.data(), reference_vs_eye1.size()};
        ComPtr<ID3D12PipelineState> reference_pso_eye1; check(device->CreateGraphicsPipelineState(&rd, IID_PPV_ARGS(&reference_pso_eye1)));
        // --motion: the reference drawn 16 times at sub-pixel offsets and summed (the screen filtered over each pixel's area).
        auto sum_desc = rd; sum_desc.VS = {reference_vs.data(), reference_vs.size()}; sum_desc.DepthStencilState.DepthEnable = FALSE;
        for (auto& b : sum_desc.BlendState.RenderTarget) { b.BlendEnable = TRUE; b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ONE; }
        ComPtr<ID3D12PipelineState> accumulate_pso; check(device->CreateGraphicsPipelineState(&sum_desc, IID_PPV_ARGS(&accumulate_pso)));

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
        // Variant: the screen sits in t0 (first descriptor of the table).
        device->CreateShaderResourceView(screen_t0 ? display.Get() : alpha.Get(), nullptr, cpu0);
        device->CreateShaderResourceView(screen_t0 ? alpha.Get() : display.Get(), nullptr, cpu1);
        // Views the sim creates that the add-on must leave alone: a depth-stencil texture read as depth (plane 0)
        // and as stencil (plane 1), plus a plain sampled RGBA8 texture and an HDR UAV texture.
        D3D12_CLEAR_VALUE stencil_clear{DXGI_FORMAT_D24_UNORM_S8_UINT}; stencil_clear.DepthStencil.Depth = 1;
        auto depth_stencil = texture(device.Get(), 256, 256, 1, DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &stencil_clear);
        auto sampled = texture(device.Get(), 512, 512, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        auto hdr = texture(device.Get(), 256, 256, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        {
            auto spare = cpu0; spare.ptr += 2 * inc;
            D3D12_SHADER_RESOURCE_VIEW_DESC v{DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(depth_stencil.Get(), &v, spare);
            v.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT; v.Texture2D.PlaneSlice = 1;
            device->CreateShaderResourceView(depth_stencil.Get(), &v, spare);
            device->CreateShaderResourceView(sampled.Get(), nullptr, spare);
            device->CreateShaderResourceView(hdr.Get(), nullptr, spare);
        }
        auto gpu_slot = gpu0; gpu_slot.ptr += 4 * inc; // table at slot 4
        device->CopyDescriptorsSimple(2, gpu_slot, cpu0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto table = gpu_heap->GetGPUDescriptorHandleForHeapStart(); table.ptr += 4 * inc;
        if (bench) {
            // The sim makes millions of descriptor copies per second from several threads; with descriptor tracking
            // (--track) each goes through the add-on. Time them: 4 threads, 1M single-descriptor copies each, from a
            // CPU heap with screen views into a big shader-visible heap.
            constexpr UINT threads = 4, calls = 1000000, sources = 4096, targets = 262144;
            ComPtr<ID3D12DescriptorHeap> bench_cpu, bench_gpu;
            D3D12_DESCRIPTOR_HEAP_DESC bd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, sources, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            check(device->CreateDescriptorHeap(&bd, IID_PPV_ARGS(&bench_cpu)));
            bd.NumDescriptors = targets; bd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            check(device->CreateDescriptorHeap(&bd, IID_PPV_ARGS(&bench_gpu)));
            const auto src0 = bench_cpu->GetCPUDescriptorHandleForHeapStart(), dst0 = bench_gpu->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < sources; ++i) {
                auto at = src0; at.ptr += SIZE_T(i) * inc;
                device->CreateShaderResourceView(i % 64 == 0 ? display.Get() : sampled.Get(), nullptr, at); // some screen views
            }
            LARGE_INTEGER frequency, begin, end;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&begin);
            std::vector<std::thread> workers;
            for (UINT t = 0; t < threads; ++t)
                workers.emplace_back([&, t] {
                    for (UINT i = 0; i < calls; ++i) {
                        auto from = src0; from.ptr += SIZE_T((i * 7 + t) % sources) * inc;
                        auto to = dst0; to.ptr += SIZE_T((t * (targets / threads)) + i % (targets / threads)) * inc;
                        device->CopyDescriptorsSimple(1, to, from, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                });
            for (auto& w : workers) w.join();
            QueryPerformanceCounter(&end);
            const double seconds = double(end.QuadPart - begin.QuadPart) / frequency.QuadPart;
            std::printf("bench: %u threads x %u descriptor copies: %.1f ms, %.1f ns per copy (wall clock / all copies)\n", threads, calls,
                seconds * 1000, seconds * 1e9 / (double(threads) * calls));
            return 0;
        }

        // Targets.
        D3D12_CLEAR_VALUE black{DXGI_FORMAT_R11G11B10_FLOAT, {}};
        auto scene = texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &black, UINT16(views));
        // VR: each eye's layer is copied here before its upscaler call (the sim's per-view colour input).
        auto eye_colour = vr ? texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
        auto final_views = vr ? texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, UINT16(views)) : nullptr;
        D3D12_CLEAR_VALUE depth_clear{DXGI_FORMAT_D32_FLOAT}; depth_clear.DepthStencil.Depth = 1;
        auto depth = texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &stencil_clear, UINT16(views));
        const float magenta[4]{1, 0, 1, 1};
        D3D12_CLEAR_VALUE magenta_clear{DXGI_FORMAT_R11G11B10_FLOAT, {1, 0, 1, 1}};
        auto output = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_R11G11B10_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET, &magenta_clear);
        // --extra-pass: another mod's DLSS pass at the same render size with a render-size output, before the sim's own.
        auto extra_output = texture(device.Get(), render_w, render_h, 1, DXGI_FORMAT_R11G11B10_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &magenta_clear);
        D3D12_CLEAR_VALUE ref_clear{DXGI_FORMAT_R32G32B32A32_FLOAT, {}};
        auto reference = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &ref_clear);
        auto ref_depth = texture(device.Get(), output_w, output_h, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &stencil_clear);
        ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}, dh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        check(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap))); check(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsv_heap)));
        const UINT rinc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), dinc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto rtv = [&](UINT i) { auto h = rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rinc; return h; };
        auto dsv = [&](UINT i) { auto h = dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * dinc; return h; };
        if (vr) {
            D3D12_RENDER_TARGET_VIEW_DESC layered{DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
            layered.Texture2DArray = {0, 0, views, 0};
            device->CreateRenderTargetView(scene.Get(), &layered, rtv(0));
        } else device->CreateRenderTargetView(scene.Get(), nullptr, rtv(0));
        device->CreateRenderTargetView(output.Get(), nullptr, rtv(1));
        device->CreateRenderTargetView(reference.Get(), nullptr, rtv(2));
        if (vr) {
            D3D12_DEPTH_STENCIL_VIEW_DESC layered{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2DARRAY};
            layered.Texture2DArray = {0, 0, views};
            device->CreateDepthStencilView(depth.Get(), &layered, dsv(0));
        } else device->CreateDepthStencilView(depth.Get(), nullptr, dsv(0));
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
        auto render_reference = [&](int camera, UINT eye) {
            const float zero[4]{};
            list->ClearRenderTargetView(rtv(2), zero, 0, nullptr);
            list->ClearDepthStencilView(dsv(1), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            bind(eye ? reference_pso_eye1.Get() : reference_pso.Get(), camera);
            const D3D12_VIEWPORT vp{0, 0, float(output_w), float(output_h), 0, 1}; const D3D12_RECT sc{0, 0, LONG(output_w), LONG(output_h)};
            list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
            auto r2 = rtv(2), d1 = dsv(1); list->OMSetRenderTargets(1, &r2, FALSE, &d1);
            list->DrawIndexedInstanced(6, views, 0, 1, 0);
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

        ID3D12Resource* const colour = vr ? eye_colour.Get() : scene.Get();
        Params ngx; ngx.colour = colour; ngx.output = output.Get();
        ffxDispatchDescUpscale proxy_fsr{};
        if (proxy_answer_with_fsr) {
            proxy_fsr.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            proxy_fsr.color = {colour, {}, FFX_API_RESOURCE_STATE_COMPUTE_READ};
            proxy_fsr.output = {output.Get(), {}, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
            proxy_fsr.renderSize = {render_w, render_h};
            proxy_fsr.upscaleSize = {output_w, output_h};
            proxy_fsr.preExposure = 1;
            proxy_answer_with_fsr(ffx_dispatch, &proxy_fsr);
        }
        if (ffx_run_dlss_inside) ffx_run_dlss_inside(evaluate, &ngx);
        auto out_rb = readback(device.Get(), output.Get());
        auto prefill_upload = buffer(device.Get(), out_rb.bytes, D3D12_HEAP_TYPE_UPLOAD);
        std::array<std::vector<std::array<float, 4>>, 2> previous_refs;
        std::vector<uint32_t> prefill(size_t(output_w) * output_h);
        bool all_passed = true;
        if (motion) {
            // Shimmer and trails with the camera panning slowly (~1/3 output pixel per frame) over a screen with thin lines,
            // and a content change (the picture scrolls 8 texels) at frame 30. Compared with the screen filtered over each
            // pixel's area (16 sub-pixel reference renders): "error" = how far from that, "flicker" = how much more the
            // picture changes from frame to frame than that ideal picture does (shimmer), "after change" = error on the
            // 3 frames after the content change (trails would show here).
            const auto set_temporal = reinterpret_cast<void (*)(int)>(GetProcAddress(addon, "GlassFixTemporal"));
            require(set_temporal != nullptr, "exports missing");
            using Image = std::vector<std::array<float, 4>>;
            auto truth = [&] {
                const float zero[4]{};
                list->ClearRenderTargetView(rtv(2), zero, 0, nullptr);
                bind(accumulate_pso.Get(), 0);
                auto r2 = rtv(2); list->OMSetRenderTargets(1, &r2, FALSE, nullptr);
                const D3D12_RECT sc{0, 0, LONG(output_w), LONG(output_h)}; list->RSSetScissorRects(1, &sc);
                for (int k = 0; k < 16; ++k) {
                    const D3D12_VIEWPORT vp{(k % 4 + 0.5f) / 4 - 0.5f, (k / 4 + 0.5f) / 4 - 0.5f, float(output_w), float(output_h), 0, 1};
                    list->RSSetViewports(1, &vp);
                    list->DrawIndexedInstanced(6, 1, 0, 1, 0);
                }
                transition(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                auto rb = readback(device.Get(), reference.Get()); copy(list.Get(), reference.Get(), rb);
                transition(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                submit();
                Image result(size_t(output_w) * output_h);
                void* mapped = nullptr; check(rb.buffer->Map(0, nullptr, &mapped));
                for (UINT y = 0; y < output_h; ++y)
                    std::memcpy(&result[size_t(y) * output_w], static_cast<uint8_t*>(mapped) + rb.footprint.Offset + size_t(y) * rb.footprint.Footprint.RowPitch, size_t(output_w) * 16);
                rb.buffer->Unmap(0, nullptr);
                for (auto& v : result) { for (int c = 0; c < 3; ++c) v[c] /= 16; v[3] = v[3] >= 7.99f ? 1.0f : 0.0f; } // w: fully covered
                return result;
            };
            auto run = [&](const Image& ideal, float jx, float jy) {
                list->ClearRenderTargetView(rtv(0), black.Color, 0, nullptr);
                list->ClearDepthStencilView(dsv(0), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
                bind(game_pso.Get(), 0);
                const D3D12_VIEWPORT vp{jx, jy, float(render_w), float(render_h), 0, 1}; const D3D12_RECT sc{0, 0, LONG(render_w), LONG(render_h)};
                list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
                auto r0 = rtv(0), d0 = dsv(0); list->OMSetRenderTargets(1, &r0, FALSE, &d0);
                list->DrawIndexedInstanced(6, 1, 0, 1, 0);
                transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                submit();
                // The fake DLSS output: the ideal picture (no ghost); the fix redraws the screen over it.
                for (size_t i = 0; i < prefill.size(); ++i) prefill[i] = encode_r11g11b10({ideal[i][0], ideal[i][1], ideal[i][2]});
                {
                    void* data = nullptr; check(prefill_upload->Map(0, nullptr, &data));
                    for (UINT y = 0; y < output_h; ++y)
                        std::memcpy(static_cast<uint8_t*>(data) + out_rb.footprint.Offset + size_t(y) * out_rb.footprint.Footprint.RowPitch, &prefill[size_t(y) * output_w], size_t(output_w) * 4);
                    prefill_upload->Unmap(0, nullptr);
                }
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION to{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{prefill_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                from.PlacedFootprint = out_rb.footprint;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ngx.jitter_x = jx; ngx.jitter_y = jy;
                require(evaluate(list.Get(), nullptr, &ngx, nullptr) == NVSDK_NGX_Result_Success, "evaluate failed");
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                copy(list.Get(), output.Get(), out_rb);
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                submit();
                Image result(size_t(output_w) * output_h);
                void* mapped = nullptr; check(out_rb.buffer->Map(0, nullptr, &mapped));
                for (UINT y = 0; y < output_h; ++y) for (UINT x = 0; x < output_w; ++x) {
                    const auto c = r11g11b10(*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(mapped) + out_rb.footprint.Offset + size_t(y) * out_rb.footprint.Footprint.RowPitch + x * 4));
                    result[size_t(y) * output_w + x] = {c[0], c[1], c[2], 1};
                }
                out_rb.buffer->Unmap(0, nullptr);
                return result;
            };
            auto save = [&](const Image& image, const std::string& name) {
                std::vector<BYTE> png(size_t(output_w) * output_h * 4);
                for (size_t i = 0; i < image.size(); ++i) { for (int k = 0; k < 3; ++k) png[i * 4 + k] = BYTE(std::clamp(image[i][k], 0.0f, 1.0f) * 255); png[i * 4 + 3] = 255; }
                save_png(here / ("motion-" + name + ".png"), output_w, output_h, output_w * 4, png.data());
            };
            struct Mode { const char* name; int smoothing, temporal; float speed = 0.0007f; int gaussian = 0; };
            const Mode modes[]{{"still-off-ls0", 0, 0, 0}, {"still-taa-ls0", 0, 1, 0}, {"off-ls0", 0, 0}, {"off-ls20", 20, 0}, {"off-ls35", 35, 0}, {"off-ls50", 50, 0},
                {"off-ls70", 70, 0}, {"gauss-ls20", 20, 0, 0.0007f, 1}, {"gauss-ls35", 35, 0, 0.0007f, 1}, {"gauss-ls50", 50, 0, 0.0007f, 1}, {"gauss-ls70", 70, 0, 0.0007f, 1},
                {"taa-ls0", 0, 1}, {"taa-ls20", 20, 1}, {"taa-gauss20", 20, 1, 0.0007f, 1}};
            const auto set_gaussian = reinterpret_cast<void (*)(int)>(GetProcAddress(addon, "GlassFixGaussian"));
            require(set_gaussian != nullptr, "exports missing");
            const float jitters[8][2]{{0.0f, -0.1667f}, {-0.25f, 0.1667f}, {0.25f, -0.3889f}, {-0.375f, -0.0556f},
                {0.125f, 0.2778f}, {-0.125f, -0.2778f}, {0.375f, 0.0556f}, {-0.4375f, 0.3889f}};
            constexpr int frame_count = 52, change_at = 46, measure_from = 26;
            // Screen content only: no dirt, glare or shadow, glass layer off (the fake DLSS output is the ideal picture,
            // which the glass layer would otherwise pass through).
            set_material({"motion", 0, true, true, 0, 0, 0, 0.0f, 0.0f, false, Frame::perfect, Frame::info});
            const auto set_glass = reinterpret_cast<void (*)(int)>(GetProcAddress(addon, "GlassFixGlassStrength"));
            require(set_glass != nullptr, "exports missing");
            set_glass(0);
            Sleep(1500); // let the background PSO compile finish
            for (const auto& m : modes) {
                set_smooth(m.smoothing);
                set_gaussian(m.gaussian);
                set_temporal(0);
                upload_display(0);
                Image previous_out, previous_ideal;
                double error = 0, flicker = 0, after_change = 0;
                uint64_t error_n = 0, flicker_n = 0, change_n = 0;
                for (int t = 0; t < frame_count; ++t) {
                    if (t == 1) set_temporal(m.temporal); // the first frame without history (like switching it on)
                    if (t == change_at) upload_display(8);
                    const auto c = constants(m.speed * t);
                    upload(globals[0].Get(), c.global.data(), sizeof(c.global));
                    const auto ideal = truth();
                    const auto out = run(ideal, jitters[t % 8][0], jitters[t % 8][1]);
                    for (UINT y = 3; y + 3 < output_h; ++y) for (UINT x = 3; x + 3 < output_w; ++x) {
                        const size_t i = size_t(y) * output_w + x;
                        bool inner = true;
                        for (int dy = -3; dy <= 3 && inner; dy += 3) for (int dx = -3; dx <= 3 && inner; dx += 3) inner = ideal[i + dy * int(output_w) + dx][3] > 0;
                        if (!inner) continue;
                        double e = 0, f = 0;
                        for (int k = 0; k < 3; ++k) {
                            e += std::abs(double(out[i][k]) - ideal[i][k]);
                            if (!previous_out.empty()) f += std::abs((double(out[i][k]) - previous_out[i][k]) - (double(ideal[i][k]) - previous_ideal[i][k]));
                        }
                        if (t >= measure_from && t < change_at) { error += e / 3; ++error_n; if (!previous_out.empty()) { flicker += f / 3; ++flicker_n; } }
                        if (t >= change_at && t < change_at + 3) { after_change += e / 3; ++change_n; }
                    }
                    if (t == change_at - 1) save(out, std::string(m.name));
                    if (t == change_at) save(out, std::string(m.name) + "-changed");
                    previous_out = out; previous_ideal = ideal;
                }
                std::printf("%-10s error=%.4f flicker=%.4f after_change=%.4f\n", m.name, error / std::max<uint64_t>(error_n, 1),
                    flicker / std::max<uint64_t>(flicker_n, 1), after_change / std::max<uint64_t>(change_n, 1));
            }
            set_temporal(0); set_smooth(0);
            return 0;
        }
        for (const auto& f : frames) {
            set_jitter(f.jitter_positive);
            if (!std::strcmp(f.name, "warmup2")) Sleep(1500); // let the background PSO compile finish
            set_material(f);
            static UINT current_scroll = 0;
            if (f.scroll != current_scroll) { upload_display(f.scroll); current_scroll = f.scroll; }
            const int variant = f.camera == 1 ? 1 : (f.glare ? 2 : 0);
            std::array<std::vector<std::array<float, 4>>, 2> refs;
            for (UINT eye = 0; eye < views; ++eye) refs[eye] = render_reference(variant, eye);
            list->ClearRenderTargetView(rtv(0), black.Color, 0, nullptr);
            list->ClearDepthStencilView(dsv(0), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            if (f.draw) {
                bind(game_pso.Get(), variant);
                // Jitter: geometry displaced by +J render pixels (y down). VR: one instance per eye.
                const D3D12_VIEWPORT vp{f.jx, f.jy, float(render_w), float(render_h), 0, 1}; const D3D12_RECT sc{0, 0, LONG(render_w), LONG(render_h)};
                list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
                auto r0 = rtv(0), d0 = dsv(0); list->OMSetRenderTargets(1, &r0, FALSE, &d0);
                list->DrawIndexedInstanced(6, views, 0, 1, 0);
            }
            transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, vr ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            submit();
          for (UINT call = 0; call < views; ++call) {
            const UINT eye = views - 1 - call; // out of layer order
            const auto& ref = refs[eye];
            auto& previous_ref = previous_refs[eye];
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
            if (vr) {
                // This eye's layer becomes the upscaler's colour input.
                D3D12_TEXTURE_COPY_LOCATION to{eye_colour.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{scene.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                from.SubresourceIndex = eye;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                transition(list.Get(), eye_colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            {
                D3D12_TEXTURE_COPY_LOCATION to{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{prefill_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                from.PlacedFootprint = out_rb.footprint;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ngx.jitter_x = f.jx; ngx.jitter_y = f.jy;
            if (fsr) {
                transition(list.Get(), colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                ffxDispatchDescUpscale desc{};
                desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
                desc.commandList = list.Get();
                desc.color = {colour, {}, FFX_API_RESOURCE_STATE_PIXEL_READ};
                desc.output = {output.Get(), {}, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
                desc.jitterOffset = {f.jx, f.jy};
                desc.renderSize = {render_w, render_h};
                desc.upscaleSize = {output_w, output_h};
                desc.preExposure = 1;
                require(ffx_dispatch(nullptr, &desc.header) == FFX_API_RETURN_OK, "ffxDispatch failed");
                transition(list.Get(), colour, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            } else if (proxy_evaluate) {
                proxy_fsr.jitterOffset = {f.jx, f.jy};
                require(proxy_evaluate(list.Get(), nullptr, &ngx, nullptr) == NVSDK_NGX_Result_Success, "evaluate (OptiScaler) failed");
            } else {
                if (extra_pass) {
                    Params extra = ngx; extra.output = extra_output.Get();
                    require(evaluate(list.Get(), nullptr, &extra, nullptr) == NVSDK_NGX_Result_Success, "evaluate (extra pass) failed");
                }
                require(evaluate(list.Get(), nullptr, &ngx, nullptr) == NVSDK_NGX_Result_Success, "evaluate failed");
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy(list.Get(), output.Get(), out_rb);
            if (vr) {
                D3D12_TEXTURE_COPY_LOCATION to{final_views.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                to.SubresourceIndex = eye;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (vr) transition(list.Get(), eye_colour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (call + 1 == views) transition(list.Get(), scene.Get(), vr ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
            const std::string name = vr ? std::string(f.name) + "/eye" + std::to_string(eye) : f.name;
            save_png(here / ("overlay-" + (vr ? std::string(f.name) + "-eye" + std::to_string(eye) : name) + ".png"), output_w, output_h, output_w * 4, png.data());
            const double mean = interior ? error / interior : 0, ghost_mean = interior ? ghost / interior : 0;
            bool pass = true;
            if (f.check == Frame::quality) pass = interior > 10000 && mean < 0.02 && stray < covered / 50;
            if (f.check == Frame::untouched) pass = replaced == 0;
            std::printf("%-12s covered=%llu replaced=%llu stray=%llu error=%.4f (dlss_alone=%.4f) max=%.3f %s\n", name.c_str(), covered, replaced, stray,
                mean, ghost_mean, max_error, f.check == Frame::info ? "(info)" : pass ? "PASS" : "FAIL");
            all_passed &= pass;
            previous_ref = ref;
          }
        }
        if (hotpatch) {
            std::printf("redirected DLSS: the other mod's handler ran %u times\n", mfg_handler_calls);
            all_passed = all_passed && mfg_handler_calls > 0;
        }
        std::printf(all_passed ? "ALL PASSED\n" : "FAILED\n");
        return all_passed ? 0 : 1;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}

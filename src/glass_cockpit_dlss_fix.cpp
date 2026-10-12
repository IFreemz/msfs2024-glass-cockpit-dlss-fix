// Glass Cockpit DLSS Fix - ReShade add-on for Microsoft Flight Simulator 2024.
// Removes DLSS ghosting/smearing from glass cockpit displays by redrawing their content after DLSS.
// https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix  (MIT license)
//
// 1. Every cockpit display draw (MSFS display shader, VS f102f1e3f5d9f66f / PS 37e87c938bc76310) is drawn once more
//    into a private render-resolution "UV map" (uv, depth, display slot) using the game's own vertex shader,
//    bindings and depth test. The bound display texture is identified by its descriptor bytes.
// 2. Right after DLSS evaluate, on the same command list, compute passes fit each display's brightness against the
//    DLSS input colour, sample the clean display texture at output resolution (jitter removed) and write it over the
//    DLSS output. Dirt, glare and shadows on the glass come from the DLSS output itself, clamped to this frame's
//    local range so that trails of old screen content are removed.
// Hotkeys: Ctrl+Shift+F9 on/off, Ctrl+Shift+F10 flip jitter direction, Ctrl+Shift+F11 green tint (shows coverage).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <reshade.hpp>
#include <MinHook.h>
#include "ngx_interface.hpp"
#include "ffx_interface.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <share.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include "pipeline_clone.hpp"
#include "overlay_shaders.h"
#include "status.hpp"
#include "version.h"

namespace api = reshade::api;
using Microsoft::WRL::ComPtr;

size_t graphics_pipeline_slot();
size_t pipeline_stream_slot();

namespace {
// MSFS 2024 cockpit display shader pair (replaced by the stand-in shaders' hashes in the offline test host).
uint64_t display_vs = 0xf102f1e3f5d9f66fULL, display_ps = 0x37e87c938bc76310ULL;

uint64_t shader_hash(const void* bytes, size_t size) { // FNV-1a over the bytecode
    auto data = static_cast<const unsigned char*>(bytes);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ULL;
    return size ? hash : 0;
}
// Per frame: map, scene colour, one SRV per display slot, then output, tiles, params, map UAV and residual UAVs.
constexpr unsigned slot_count = 32, ring_size = 8, uav_base = 2 + slot_count, descriptors_per_frame = uav_base + 5;
std::filesystem::path addon_directory;
ID3D12Device* main_device = nullptr;
uint64_t init_tick = 0;

std::atomic<bool> enabled{true}, jitter_positive{true}, tint{false};
// Line smoothing 0-50 %: 0 = the screen texture as sharp as the sim samples it, 35 = default, 50 = DLSS-like (softest).
constexpr int max_smoothing = 50, default_smoothing = 35;
std::atomic<int> smoothing_percent{default_smoothing};
// Bumped when the overlay is switched on: lists recorded while it was off have stale binding state.
std::atomic<uint32_t> generation{1};
FILE* logfile = nullptr;
std::mutex log_mutex;
void log(const char* format, ...) {
    std::lock_guard lock(log_mutex);
    if (!logfile) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(logfile, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    va_list args; va_start(args, format); vfprintf(logfile, format, args); va_end(args);
    fputc('\n', logfile); fflush(logfile);
}

constexpr const char* overlay_version = GLASS_VERSION_STRING;

// Shown in the ReShade status panel.
std::mutex info_mutex;
std::string gpu_name, gpu_driver, dlss_version, fsr_version;
std::atomic<const char*> last_upscaler{"DLSS"};
std::atomic<bool> gpu_known{false}, gpu_nvidia{false};
std::atomic<unsigned> last_render_w{0}, last_render_h{0}, last_out_w{0}, last_out_h{0};
std::atomic<uint64_t> last_composite_present{0};
std::atomic<const char*> problem{nullptr};
std::string utf8(const wchar_t* text) {
    char buffer[512]{};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
    return buffer;
}

// Logs the first few occurrences of each skip reason.
void note(std::atomic<uint64_t>& counter, const char* reason) {
    if (counter++ < 3) log("skip reason=%s", reason);
}
// Logs a message only the first time (milestones and one-off warnings).
#define LOG_ONCE(...) do { static std::atomic<bool> logged_{false}; if (!logged_.exchange(true)) log(__VA_ARGS__); } while (0)

struct Stats {
    std::atomic<uint64_t> replays{0}, variant_pending{0}, unknown_texture{0}, unsupported_draw{0},
        composites{0}, composite_skipped{0}, evaluations{0}, fsr_dispatches{0}, display_draws{0}, display_pipelines{0};
} stats;

std::string file_version(const wchar_t* path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return "unknown";
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!GetFileVersionInfoW(path, 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return "unknown";
    char text[64];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

// Logs DLSS/Streamline/ReShade-related modules once each, with version and path (helps spot missing DLSS or conflicts).
void scan_modules() {
    static std::set<std::wstring> seen;
    HMODULE modules[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (auto& c : name) c = wchar_t(towlower(c));
        const bool relevant = name.starts_with(L"nvngx") || name.starts_with(L"_nvngx") || name.starts_with(L"sl.") ||
            name.ends_with(L".addon64") || name.ends_with(L".addon") || name.starts_with(L"reshade") || name == L"d3d12core.dll" ||
            name == L"dxgi.dll" || name == L"version.dll" || name == L"nvapi64.dll";
        if (!relevant || !seen.insert(name).second) continue;
        const auto version = file_version(path);
        log("module %ls version=%s path=%ls", name.c_str(), version.c_str(), path);
        if (name == L"nvngx_dlss.dll") { std::lock_guard lock(info_mutex); dlss_version = version; }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Display pipelines: deep copy of the game's PSO description so slot variants can be created later.
struct DisplayPipeline {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<uint8_t> vs;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> names;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12Device> device;
    std::array<std::atomic<ID3D12PipelineState*>, slot_count> variants{};
    std::array<std::atomic<bool>, slot_count> requested{};
};
constexpr size_t max_pipelines = 64;
std::array<std::atomic<uint64_t>, max_pipelines> pipeline_keys{};
std::array<DisplayPipeline*, max_pipelines> pipeline_values{};
std::mutex pipelines_mutex;

DisplayPipeline* find_pipeline(uint64_t handle) {
    for (size_t i = 0; i < max_pipelines; ++i) {
        const auto key = pipeline_keys[i].load(std::memory_order_acquire);
        if (!key) return nullptr;
        if (key == handle) return pipeline_values[i];
    }
    return nullptr;
}

const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders{{
#define SLOT(n) {map_ps_##n, sizeof(map_ps_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};

// Variant PSOs are compiled on a worker thread; draws are skipped (not stalled) until theirs is ready.
std::mutex work_mutex;
std::condition_variable work_ready;
std::deque<std::pair<DisplayPipeline*, unsigned>> work;
void worker() {
    for (;;) {
        std::pair<DisplayPipeline*, unsigned> item;
        { std::unique_lock lock(work_mutex); work_ready.wait(lock, [] { return !work.empty(); }); item = work.front(); work.pop_front(); }
        auto [pipeline, slot] = item;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC clone{};
        if (!glass::clone_pipeline(pipeline->desc, {map_shaders[slot].first, map_shaders[slot].second}, clone)) {
            log("variant_rejected slot=%u", slot); continue;
        }
        ID3D12PipelineState* created = nullptr;
        const auto hr = pipeline->device->CreateGraphicsPipelineState(&clone, IID_PPV_ARGS(&created));
        if (FAILED(hr)) { log("variant_failed slot=%u hr=0x%lx", slot, hr); continue; }
        pipeline->variants[slot] = created;
    }
}
ID3D12PipelineState* variant(DisplayPipeline* pipeline, unsigned slot) {
    if (auto ready = pipeline->variants[slot].load(std::memory_order_acquire)) return ready;
    if (!pipeline->requested[slot].exchange(true)) {
        { std::lock_guard lock(work_mutex); work.emplace_back(pipeline, slot); }
        work_ready.notify_one();
    }
    ++stats.variant_pending;
    return nullptr;
}

void pipeline_created(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState* pso) noexcept try {
    if (!desc.PS.pShaderBytecode || !desc.VS.pShaderBytecode) return;
    const auto ps = shader_hash(desc.PS.pShaderBytecode, desc.PS.BytecodeLength), vs = shader_hash(desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
    if (ps != display_ps || vs != display_vs) return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC probe{};
    if (!glass::clone_pipeline(desc, {map_shaders[0].first, map_shaders[0].second}, probe)) {
        log("pipeline_unsupported pso=%p", pso); return;
    }
    auto copy = new DisplayPipeline;
    copy->desc = desc;
    copy->vs.assign(static_cast<const uint8_t*>(desc.VS.pShaderBytecode), static_cast<const uint8_t*>(desc.VS.pShaderBytecode) + desc.VS.BytecodeLength);
    copy->desc.VS = {copy->vs.data(), copy->vs.size()};
    copy->desc.PS = {};
    copy->desc.CachedPSO = {};
    copy->names.reserve(desc.InputLayout.NumElements);
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) copy->names.emplace_back(desc.InputLayout.pInputElementDescs[i].SemanticName);
    copy->elements.assign(desc.InputLayout.pInputElementDescs, desc.InputLayout.pInputElementDescs + desc.InputLayout.NumElements);
    for (size_t i = 0; i < copy->elements.size(); ++i) copy->elements[i].SemanticName = copy->names[i].c_str();
    copy->desc.InputLayout = {copy->elements.data(), UINT(copy->elements.size())};
    copy->root = desc.pRootSignature;
    copy->device = device;
    std::lock_guard lock(pipelines_mutex);
    for (size_t i = 0; i < max_pipelines; ++i) if (!pipeline_keys[i].load()) {
        pipeline_values[i] = copy;
        pipeline_keys[i].store(reinterpret_cast<uint64_t>(pso), std::memory_order_release);
        if (stats.display_pipelines++ == 0) log("milestone cockpit display shader found (glass cockpit screens can be recognised)");
        log("display_pipeline pso=%p root=%p targets=%u depth_format=%u", pso, desc.pRootSignature, desc.NumRenderTargets, unsigned(desc.DSVFormat));
        return;
    }
    log("display_pipeline_overflow");
} catch (...) { log("pipeline_exception"); }

// Native PSO creation hooks (creation time only).
using Graphics = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using Stream = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
Graphics original_graphics = nullptr;
Stream original_stream = nullptr;
HRESULT STDMETHODCALLTYPE graphics_hook(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
    const auto hr = original_graphics(device, desc, iid, out);
    if (SUCCEEDED(hr) && desc && out && *out && iid == __uuidof(ID3D12PipelineState))
        pipeline_created(device, *desc, static_cast<ID3D12PipelineState*>(*out));
    return hr;
}
HRESULT STDMETHODCALLTYPE stream_hook(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out) {
    const auto hr = original_stream(device, desc, iid, out);
    if (SUCCEEDED(hr) && desc && out && *out && iid == __uuidof(ID3D12PipelineState)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
        if (glass::decode_stream(*desc, graphics)) pipeline_created(device, graphics, static_cast<ID3D12PipelineState*>(*out));
    }
    return hr;
}
template <class T> bool hook(void* target, void* detour, T& original) {
    if (original) return true;
    void* trampoline = nullptr;
    if (MH_CreateHook(target, detour, &trampoline) != MH_OK) return false;
    original = reinterpret_cast<T>(trampoline);
    return MH_EnableHook(target) == MH_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Display texture identification: compare descriptor bytes at the draw's t1 slot with SRVs of known
// render-target textures (verified identical after CopyDescriptors on NVIDIA, see tests/descriptor_bytes_check.cpp).
struct Signature { std::array<uint8_t, 32> bytes{}; bool operator==(const Signature&) const = default; };
struct SignatureHash { size_t operator()(const Signature& s) const { return std::hash<std::string_view>{}({reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size()}); } };
struct Texture { ID3D12Resource* resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; };
std::shared_mutex textures_mutex;
std::unordered_map<Signature, Texture, SignatureHash> signatures;
std::unordered_set<ID3D12Resource*> known_textures;
std::array<std::atomic<ID3D12Resource*>, slot_count> slot_resources{};
std::string hex(const Signature& s) {
    char text[65]{};
    for (size_t i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", s.bytes[i]);
    return text;
}
std::array<DXGI_FORMAT, slot_count> slot_formats{};
std::array<std::atomic<uint64_t>, slot_count> slot_last_used{};
std::atomic<uint64_t> present_count{0};
std::mutex slots_mutex;
UINT descriptor_size = 0;

bool read_bytes(const void* source, void* destination, size_t size) {
    __try { std::memcpy(destination, source, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool rgba8(api::format format) {
    switch (format) {
    case api::format::r8g8b8a8_typeless: case api::format::r8g8b8a8_unorm: case api::format::r8g8b8a8_unorm_srgb:
    case api::format::b8g8r8a8_typeless: case api::format::b8g8r8a8_unorm: case api::format::b8g8r8a8_unorm_srgb: return true;
    default: return false;
    }
}

int slot_of(ID3D12Resource* resource, DXGI_FORMAT format) {
    const auto now = present_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < slot_count; ++i) if (slot_resources[i].load() == resource) { slot_last_used[i] = now; return int(i); }
    std::lock_guard lock(slots_mutex);
    // Free slot first, else the least recently used one that has not been drawn for ~3 s (aircraft changes, closed pages).
    int chosen = -1;
    for (unsigned i = 0; i < slot_count && chosen < 0; ++i) if (!slot_resources[i].load()) chosen = int(i);
    if (chosen < 0) {
        uint64_t oldest = UINT64_MAX;
        for (unsigned i = 0; i < slot_count; ++i)
            if (now > slot_last_used[i] + 120 && slot_last_used[i] < oldest) { oldest = slot_last_used[i]; chosen = int(i); }
    }
    if (chosen < 0) return -1;
    slot_formats[chosen] = format;
    slot_last_used[chosen] = now;
    slot_resources[chosen] = resource;
    const auto d = resource->GetDesc();
    log("display_slot slot=%d resource=%p w=%llu h=%u mips=%u format=%u", chosen, resource, d.Width, d.Height, d.MipLevels, unsigned(format));
    return chosen;
}

// ---------------------------------------------------------------------------------------------------------------
struct Map {
    UINT width = 0, height = 0;
    ComPtr<ID3D12Resource> texture, tiles, residual;
    ComPtr<ID3D12DescriptorHeap> rtv;
    bool initialised = false;
    std::atomic<bool> drawn{false};
};

struct __declspec(uuid("2B8D0E51-7C1A-4F43-9A07-8E4C51D2F6A3")) DeviceState {
    ID3D12Device* native = nullptr;
    std::mutex maps_mutex;
    std::vector<std::unique_ptr<Map>> maps;
    std::shared_mutex layouts_mutex;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> display_bindings; // layout -> root index, table offset of t1
    std::mutex composite_mutex;
    bool composite_ready = false, composite_failed = false, accumulators_zeroed = false, output_readable = false;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> passes; // zero, fit, solve, composite, clear
    ComPtr<ID3D12DescriptorHeap> heap, scratch;
    ComPtr<ID3D12Resource> sums, params;
    uint64_t frame = 0;
};

struct __declspec(uuid("9E1F3C77-5A2B-4D8E-B0C4-71D6A2E9F015")) ListState {
    api::command_list* list = nullptr;
    DeviceState* device = nullptr;
    DisplayPipeline* display = nullptr;
    uint64_t pso = 0, layout = 0;
    std::array<uint64_t, 64> tables{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    UINT target_count = 0;
    bool in_pass = false;
    uint32_t generation = 0;
};

Map* map_for(DeviceState& device, UINT width, UINT height) {
    std::lock_guard lock(device.maps_mutex);
    for (auto& map : device.maps) if (map->width == width && map->height == height) return map.get();
    if (device.maps.size() >= 4 || !width || !height || width > 8192 || height > 8192) return nullptr;
    auto map = std::make_unique<Map>();
    map->width = width; map->height = height;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, glass::map_format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_CLEAR_VALUE clear{glass::map_format, {}};
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&map->texture)))) return nullptr;
    desc.Width = (width + 7) / 8; desc.Height = (height + 7) / 8; desc.Format = DXGI_FORMAT_R32_UINT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->tiles)))) return nullptr;
    desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // per render pixel
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->residual)))) return nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device.native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&map->rtv)))) return nullptr;
    device.native->CreateRenderTargetView(map->texture.Get(), nullptr, map->rtv->GetCPUDescriptorHandleForHeapStart());
    log("map_created w=%u h=%u", width, height);
    device.maps.push_back(std::move(map));
    return device.maps.back().get();
}

constexpr D3D12_RESOURCE_STATES map_resting = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
void uav_barrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    list->ResourceBarrier(1, &b);
}

void install_ngx();
void install_ffx();

// ---------------------------------------------------------------------------------------------------------------
// ReShade events.
void on_init_device(api::device* device) {
    if (device->get_api() != api::device_api::d3d12) return;
    // Only the game's hardware device: MSFS also creates a WARP (software) device for a helper window,
    // whose descriptors have a different size and must not be mixed with the GPU's.
    const auto native = reinterpret_cast<ID3D12Device*>(device->get_native());
    const auto size = native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    bool software = false;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 adapter_desc{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(native->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(adapter->GetDesc1(&adapter_desc))) software = (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (!software) {
        LARGE_INTEGER driver{};
        const bool known = adapter && SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
        log("gpu name=\"%ls\" vendor=0x%04x device=0x%04x vram_mb=%llu driver=%u.%u.%u.%u", adapter_desc.Description, adapter_desc.VendorId,
            adapter_desc.DeviceId, uint64_t(adapter_desc.DedicatedVideoMemory >> 20),
            known ? HIWORD(driver.HighPart) : 0, known ? LOWORD(driver.HighPart) : 0, known ? HIWORD(driver.LowPart) : 0, known ? LOWORD(driver.LowPart) : 0);
        if (adapter_desc.VendorId != 0x10de) log("note not an NVIDIA GPU: DLSS is unavailable, select FSR in the sim's graphics settings");
        {
            std::lock_guard lock(info_mutex);
            gpu_name = utf8(adapter_desc.Description);
            char text[64]{};
            if (known) snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
            gpu_driver = text;
        }
        gpu_nvidia = adapter_desc.VendorId == 0x10de;
        gpu_known = true;
    }
    if (software || size > 32 || (descriptor_size && descriptor_size != size)) {
        log("device_ignored native=%p software=%d descriptor_size=%u", native, software, size);
        return;
    }
    auto state = device->create_private_data<DeviceState>();
    state->native = native;
    descriptor_size = size;
    main_device = native;
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    state->native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&state->scratch));
    const auto table = *reinterpret_cast<void***>(state->native);
    bool hooked = hook(table[graphics_pipeline_slot()], reinterpret_cast<void*>(&graphics_hook), original_graphics);
    ComPtr<ID3D12Device2> device2;
    if (SUCCEEDED(state->native->QueryInterface(IID_PPV_ARGS(&device2))))
        hooked &= hook((*reinterpret_cast<void***>(device2.Get()))[pipeline_stream_slot()], reinterpret_cast<void*>(&stream_hook), original_stream);
    log("device native=%p pso_hooks=%d descriptor_size=%u", state->native, hooked, descriptor_size);
    install_ngx();
    install_ffx();
}
void on_destroy_device(api::device* device) {
    if (device->get_private_data<DeviceState>()) device->destroy_private_data<DeviceState>();
}

void on_init_layout(api::device* device, uint32_t count, const api::pipeline_layout_param* params, api::pipeline_layout layout) {
    auto state = device->get_private_data<DeviceState>();
    if (!state) return;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        const api::descriptor_range* ranges = nullptr; uint32_t range_count = 0;
        if (params[i].type == api::pipeline_layout_param_type::descriptor_table || params[i].type == api::pipeline_layout_param_type::push_descriptors_with_ranges) {
            ranges = params[i].descriptor_table.ranges; range_count = params[i].descriptor_table.count;
        } else if (params[i].type == api::pipeline_layout_param_type::descriptor_table_with_static_samplers) {
            ranges = params[i].descriptor_table_with_static_samplers.ranges; range_count = params[i].descriptor_table_with_static_samplers.count;
        } else continue;
        for (uint32_t j = 0; j < range_count; ++j) {
            const auto& r = ranges[j];
            if (r.type != api::descriptor_type::texture_shader_resource_view || r.dx_register_space || r.dx_register_index > 1 ||
                uint64_t(r.dx_register_index) + r.count <= 1 || (r.visibility & api::shader_stage::pixel) == api::shader_stage{}) continue;
            std::unique_lock lock(state->layouts_mutex);
            state->display_bindings[layout.handle] = {i, r.binding + 1 - r.dx_register_index};
            return;
        }
    }
}

void on_init_view(api::device* device, api::resource resource, api::resource_usage usage, const api::resource_view_desc& desc, api::resource_view) {
    // A default view (null D3D12 view desc) arrives with type/format "unknown".
    if (usage != api::resource_usage::shader_resource || !resource.handle ||
        (desc.type != api::resource_view_type::unknown && desc.type != api::resource_view_type::texture_2d &&
         desc.type != api::resource_view_type::texture_2d_array)) return;
    auto state = device->get_private_data<DeviceState>();
    if (!state || !state->scratch || descriptor_size > 32) return;
    const auto r = device->get_resource_desc(resource);
    // Cockpit screens are plain single-sample 2D textures.
    if (r.type != api::resource_type::texture_2d || r.texture.samples != 1 || r.texture.depth_or_layers != 1 ||
        r.texture.width < 64 || r.texture.height < 64) return;
    // Only RGBA8 colour textures the GPU writes (render target or UAV), the kind cockpit screens are. Recreating views
    // of other textures (depth/stencil planes, video, streamed material textures) at sim startup removed the device.
    const bool written = (r.usage & (api::resource_usage::render_target | api::resource_usage::unordered_access)) != api::resource_usage{};
    if (!written || !rgba8(r.texture.format) || (r.usage & api::resource_usage::depth_stencil) != api::resource_usage{}) return;
    // Recreate exactly the view the game made (a null desc stays null), so the call is valid by construction.
    const bool default_view = desc.type == api::resource_view_type::unknown;
    auto native = reinterpret_cast<ID3D12Resource*>(resource.handle);
    const auto view_format = default_view ? r.texture.format : desc.format;
    if (view_format == api::format::unknown ||
        (view_format == api::format_to_typeless(view_format) && view_format != api::format_to_default_typed(view_format))) return;
    const auto format = DXGI_FORMAT(view_format);
    // Any size: Fenix PFD 768x768; the A220 draws its main screens side by side into one 7410x1110 texture.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    if (!default_view) {
        const uint32_t first = desc.texture.first_level, count = desc.texture.level_count;
        if (first >= r.texture.levels || (count != UINT32_MAX && (count == 0 || uint64_t(first) + count > r.texture.levels))) return;
        srv.Texture2D.MostDetailedMip = first;
        srv.Texture2D.MipLevels = count == UINT32_MAX ? UINT(-1) : count;
    }
    const auto slot = state->scratch->GetCPUDescriptorHandleForHeapStart();
    std::unique_lock lock(textures_mutex);
    state->native->CreateShaderResourceView(native, default_view ? nullptr : &srv, slot);
    Signature s{};
    if (read_bytes(reinterpret_cast<const void*>(slot.ptr), s.bytes.data(), descriptor_size) && signatures.size() < 16384) {
        signatures[s] = {native, format};
        static std::atomic<int> logged{0};
        if (logged++ < 40) log("texture_candidate resource=%p w=%llu h=%u mips=%u format=%u default_view=%d", native, r.texture.width, r.texture.height,
            unsigned(r.texture.levels), unsigned(format), default_view);
    }
    known_textures.insert(native);
}
void on_destroy_resource(api::device*, api::resource resource) {
    auto native = reinterpret_cast<ID3D12Resource*>(resource.handle);
    { std::shared_lock lock(textures_mutex); if (!known_textures.contains(native)) return; }
    std::unique_lock lock(textures_mutex);
    known_textures.erase(native);
    std::erase_if(signatures, [&](const auto& p) { return p.second.resource == native; });
    for (auto& slot : slot_resources) { auto expected = native; slot.compare_exchange_strong(expected, nullptr); }
}

void on_init_list(api::command_list* list) {
    auto device = list->get_device()->get_private_data<DeviceState>();
    if (!device) return;
    auto state = list->create_private_data<ListState>();
    state->list = list; state->device = device; state->generation = generation;
    auto native = reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());
    native->SetPrivateData(__uuidof(ListState), sizeof(list), &list);
}
void on_destroy_list(api::command_list* list) {
    if (!list->get_private_data<ListState>()) return;
    reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native())->SetPrivateData(__uuidof(ListState), 0, nullptr);
    list->destroy_private_data<ListState>();
}
void on_reset_list(api::command_list* list) {
    if (auto state = list->get_private_data<ListState>()) {
        const auto device = state->device;
        *state = ListState{}; state->list = list; state->device = device; state->generation = generation;
    }
}
void on_bind_pipeline(api::command_list* list, api::pipeline_stage stages, api::pipeline pipeline) {
    if (!enabled.load(std::memory_order_relaxed) || (stages & api::pipeline_stage::all_graphics) == api::pipeline_stage{}) return;
    if (auto state = list->get_private_data<ListState>()) { state->pso = pipeline.handle; state->display = find_pipeline(pipeline.handle); }
}
void on_bind_tables(api::command_list* list, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table* tables) {
    if (!enabled.load(std::memory_order_relaxed) || (stages & api::shader_stage::all_graphics) == api::shader_stage{}) return;
    auto state = list->get_private_data<ListState>();
    if (!state) return;
    if (state->layout != layout.handle) { state->layout = layout.handle; state->tables = {}; }
    for (uint32_t i = 0; i < count && first + i < state->tables.size(); ++i) state->tables[first + i] = tables[i].handle;
}
void on_bind_targets(api::command_list* list, uint32_t count, const api::resource_view* targets, api::resource_view depth) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    auto state = list->get_private_data<ListState>();
    if (!state) return;
    state->target_count = count <= 8 ? count : 0;
    for (uint32_t i = 0; i < state->target_count; ++i) state->targets[i].ptr = SIZE_T(targets[i].handle);
    state->depth.ptr = SIZE_T(depth.handle);
}
void on_begin_pass(api::command_list* list, uint32_t, const api::render_pass_render_target_desc*, const api::render_pass_depth_stencil_desc*) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (auto state = list->get_private_data<ListState>()) state->in_pass = true;
}
void on_end_pass(api::command_list* list) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (auto state = list->get_private_data<ListState>()) { state->in_pass = false; state->target_count = 0; state->depth = {}; }
}

void replay(api::command_list* list, ListState& state, const glass::Draw& draw) noexcept try {
    auto& device = *state.device;
    if (state.in_pass || !state.target_count || !state.depth.ptr || !draw.count || !draw.instances) { note(stats.unsupported_draw, "render_pass_or_targets"); return; }
    std::pair<uint32_t, uint32_t> binding;
    {
        std::shared_lock lock(device.layouts_mutex);
        const auto found = device.display_bindings.find(state.layout);
        if (found == device.display_bindings.end() || state.layout != reinterpret_cast<uint64_t>(state.display->root.Get())) { note(stats.unsupported_draw, "root_layout"); return; }
        binding = found->second;
    }
    const auto table = state.tables[binding.first];
    if (!table) { note(stats.unsupported_draw, "t1_table_unbound"); return; }
    api::descriptor_heap heap{}; uint32_t offset = 0;
    list->get_device()->get_descriptor_heap_offset({table}, binding.second, 0, &heap, &offset);
    if (!heap.handle) { note(stats.unsupported_draw, "descriptor_heap_unknown"); return; }
    auto native_heap = reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
    Signature s{};
    if (!read_bytes(reinterpret_cast<const void*>(native_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(offset) * descriptor_size), s.bytes.data(), descriptor_size)) {
        note(stats.unknown_texture, "descriptor_unreadable"); return;
    }
    Texture texture;
    {
        std::shared_lock lock(textures_mutex);
        const auto found = signatures.find(s);
        if (found == signatures.end()) {
            if (stats.unknown_texture < 3) log("unrecognised_descriptor heap=%p offset=%u bytes=%s known=%llu", native_heap, offset, hex(s).c_str(), uint64_t(signatures.size()));
            note(stats.unknown_texture, "texture_not_recognised"); return;
        }
        texture = found->second;
    }
    const int slot = slot_of(texture.resource, texture.format);
    if (slot < 0) { note(stats.unsupported_draw, "slots_full"); return; }
    auto pso = variant(state.display, unsigned(slot));
    if (!pso) return;
    const auto target = list->get_device()->get_resource_from_view({state.targets[0].ptr});
    if (!target.handle) { note(stats.unsupported_draw, "target_unknown"); return; }
    const auto desc = reinterpret_cast<ID3D12Resource*>(target.handle)->GetDesc();
    auto map = map_for(device, UINT(desc.Width), desc.Height);
    if (!map) { note(stats.unsupported_draw, "map_allocation"); return; }
    auto native = reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());
    const auto rtv = map->rtv->GetCPUDescriptorHandleForHeapStart();
    {
        std::lock_guard lock(device.maps_mutex);
        if (!map->initialised) {
            constexpr float zero[4]{};
            native->ClearRenderTargetView(rtv, zero, 0, nullptr);
            map->initialised = true;
        } else transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // Viewport, scissor, root arguments and mesh buffers stay exactly as the game bound them.
    native->SetPipelineState(pso);
    native->OMSetRenderTargets(1, &rtv, FALSE, &state.depth);
    if (draw.indexed) native->DrawIndexedInstanced(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
    else native->DrawInstanced(draw.count, draw.instances, draw.first, draw.first_instance);
    native->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pso));
    native->OMSetRenderTargets(state.target_count, state.targets.data(), FALSE, &state.depth);
    transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, map_resting);
    map->drawn = true;
    if (stats.replays++ == 0) log("milestone first cockpit screen recorded (render target %ux%u)", UINT(desc.Width), desc.Height);
} catch (...) { note(stats.unsupported_draw, "exception"); }

bool on_draw(api::command_list* list, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
    if (!enabled.load(std::memory_order_relaxed)) return false;
    auto state = list->get_private_data<ListState>();
    if (state && state->display) ++stats.display_draws;
    if (state && state->display && state->generation == generation.load(std::memory_order_relaxed)) replay(list, *state, {false, count, instances, first, first_instance, 0});
    return false;
}
bool on_draw_indexed(api::command_list* list, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex, uint32_t first_instance) {
    if (!enabled.load(std::memory_order_relaxed)) return false;
    auto state = list->get_private_data<ListState>();
    if (state && state->display) ++stats.display_draws;
    if (state && state->display && state->generation == generation.load(std::memory_order_relaxed)) replay(list, *state, {true, count, instances, first, first_instance, vertex});
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Composite after DLSS.
bool create_composite(DeviceState& d) {
    D3D12_DESCRIPTOR_RANGE ranges[3]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, uav_base, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, uav_base},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 5, 0, uav_base + 4}}; // u4 is the root UAV (fit sums)
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 12};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {3, ranges};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {4, 0};
    D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 8, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX, 0, 0,
        D3D12_SHADER_VISIBILITY_ALL};
    D3D12_ROOT_SIGNATURE_DESC desc{3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(d.native->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&d.root)))) return false;
    const std::array<std::pair<const unsigned char*, size_t>, 5> code{{
        {cs_zero, sizeof(cs_zero)}, {cs_fit, sizeof(cs_fit)}, {cs_solve, sizeof(cs_solve)}, {cs_composite, sizeof(cs_composite)}, {cs_clear, sizeof(cs_clear)}}};
    for (size_t i = 0; i < code.size(); ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{d.root.Get(), {code[i].first, code[i].second}};
        if (FAILED(d.native->CreateComputePipelineState(&p, IID_PPV_ARGS(&d.passes[i])))) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ring_size * descriptors_per_frame, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(d.native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&d.heap)))) return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, slot_count * 13 * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.sums)))) return false;
    buffer.Width = slot_count * 2 * 16;
    return SUCCEEDED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.params)));
}

ID3D12Resource* ngx_resource(const NVSDK_NGX_Parameter& params, const char* name) {
    ID3D12Resource* resource = nullptr;
    if (params.Get(name, &resource) == NVSDK_NGX_Result_Success) return resource;
    void* pointer = nullptr;
    return params.Get(name, &pointer) == NVSDK_NGX_Result_Success ? static_cast<ID3D12Resource*>(pointer) : nullptr;
}

// What the upscaler (DLSS or FSR) was given and wrote, and the states it left its colour input and output in.
struct Upscale {
    const char* name;
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned render_w = 0, render_h = 0;
    float jitter_x = 0, jitter_y = 0;
    D3D12_RESOURCE_STATES colour_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
void composite(ID3D12GraphicsCommandList* supplied, const Upscale& u) noexcept;

DXGI_FORMAT typed(DXGI_FORMAT format) { return DXGI_FORMAT(api::format_to_default_typed(api::format(format))); }

void composite_dlss(ID3D12GraphicsCommandList* supplied, const NVSDK_NGX_Parameter* params) noexcept try {
    if (!supplied || !params || !enabled.load(std::memory_order_relaxed)) return;
    unsigned render_w = 0, render_h = 0, base[6]{};
    float jitter_x = 0, jitter_y = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &render_w) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &render_h) != NVSDK_NGX_Result_Success) {
        note(stats.composite_skipped, "dlss_render_size_missing (not a DLSS Super Resolution call?)"); return;
    }
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &base[0]);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &base[1]);
    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &base[2]);
    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &base[3]);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitter_x);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitter_y);
    auto colour = ngx_resource(*params, NVSDK_NGX_Parameter_Color), output = ngx_resource(*params, NVSDK_NGX_Parameter_Output);
    if (!colour || !output || base[0] || base[1] || base[2] || base[3]) {
        LOG_ONCE("dlss_inputs_unsupported colour=%p output=%p colour_base=%u,%u output_base=%u,%u", colour, output, base[0], base[1], base[2], base[3]);
        note(stats.composite_skipped, "dlss_inputs_unsupported"); return;
    }
    {
        const auto o = output->GetDesc(), c = colour->GetDesc();
        LOG_ONCE("milestone first DLSS frame render=%ux%u output=%llux%u output_format=%u colour_format=%u jitter=%g,%g",
            render_w, render_h, o.Width, o.Height, unsigned(o.Format), unsigned(c.Format), jitter_x, jitter_y);
    }
    Upscale u{"DLSS", colour, output, render_w, render_h, jitter_x, jitter_y};
    composite(supplied, u);
} catch (...) { ++stats.composite_skipped; }

D3D12_RESOURCE_STATES d3d12_state(uint32_t ffx) {
    D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_COMMON;
    if (ffx & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) s |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (ffx & FFX_API_RESOURCE_STATE_COMPUTE_READ) s |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (ffx & FFX_API_RESOURCE_STATE_PIXEL_READ) s |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (ffx & FFX_API_RESOURCE_STATE_COPY_SRC) s |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (ffx & FFX_API_RESOURCE_STATE_COPY_DEST) s |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (ffx & FFX_API_RESOURCE_STATE_RENDER_TARGET) s |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return s;
}

void composite_fsr(const ffxDispatchDescUpscale& desc) noexcept try {
    if (!desc.commandList || !enabled.load(std::memory_order_relaxed)) return;
    Upscale u{"FSR", static_cast<ID3D12Resource*>(desc.color.resource), static_cast<ID3D12Resource*>(desc.output.resource),
        desc.renderSize.width, desc.renderSize.height, desc.jitterOffset.x, desc.jitterOffset.y,
        d3d12_state(desc.color.state), d3d12_state(desc.output.state)};
    if (!u.colour || !u.output) { note(stats.composite_skipped, "fsr_inputs_missing"); return; }
    {
        const auto o = u.output->GetDesc(), c = u.colour->GetDesc();
        LOG_ONCE("milestone first FSR frame render=%ux%u output=%llux%u output_format=%u colour_format=%u jitter=%g,%g colour_state=0x%x output_state=0x%x",
            u.render_w, u.render_h, o.Width, o.Height, unsigned(o.Format), unsigned(c.Format), u.jitter_x, u.jitter_y, desc.color.state, desc.output.state);
    }
    composite(static_cast<ID3D12GraphicsCommandList*>(desc.commandList), u);
} catch (...) { ++stats.composite_skipped; }

void composite(ID3D12GraphicsCommandList* supplied, const Upscale& u) noexcept try {
    api::command_list* list = nullptr; UINT size = sizeof(list);
    if (FAILED(supplied->GetPrivateData(__uuidof(ListState), &size, &list)) || size != sizeof(list) || !list) {
        note(stats.composite_skipped, "upscaler_command_list_unknown (the upscaler ran on a command list ReShade did not report)"); return;
    }
    auto state = list->get_private_data<ListState>();
    if (!state) return;
    auto& d = *state->device;
    if (reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native())->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        note(stats.composite_skipped, "upscaler_list_not_direct"); return;
    }
    const unsigned render_w = u.render_w, render_h = u.render_h;
    const float jitter_x = u.jitter_x, jitter_y = u.jitter_y;
    ID3D12Resource* colour = u.colour;
    ID3D12Resource* output = u.output;
    Map* map = nullptr;
    {
        std::lock_guard lock(d.maps_mutex);
        for (auto& m : d.maps) if (m->width == render_w && m->height == render_h) map = m.get();
    }
    if (!map) {
        // Normal for views without a cockpit; suspicious if screens were drawn at another size.
        std::lock_guard lock(d.maps_mutex);
        for (auto& m : d.maps) if (m->drawn) {
            LOG_ONCE("note %s render size %ux%u differs from the cockpit render target %ux%u (screens drawn in another view?)", u.name, render_w, render_h, m->width, m->height);
            break;
        }
        return;
    }
    if (!map->drawn.exchange(false)) return;
    const auto out = output->GetDesc(), in = colour->GetDesc();
    // Any float or UNORM colour output the compute pass can write (DLSS in MSFS: R11G11B10_FLOAT).
    const DXGI_FORMAT out_format = typed(out.Format), in_format = typed(in.Format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT out_support{out_format};
    if (!(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || in.Width < render_w || in.Height < render_h ||
        FAILED(d.native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &out_support, sizeof(out_support))) ||
        !(out_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        LOG_ONCE("WARNING unsupported %s output format=%u flags=0x%x (needs a UAV-writable colour format) colour=%llux%u", u.name, unsigned(out.Format), unsigned(out.Flags), in.Width, in.Height);
        note(stats.composite_skipped, "upscaler_output_unsupported"); return;
    }
    const bool output_readable = (out_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
    const UINT out_w = UINT(out.Width), out_h = out.Height;

    std::lock_guard lock(d.composite_mutex);
    if (!d.composite_ready) {
        if (d.composite_failed) return;
        d.composite_ready = create_composite(d);
        log("%s output format=%u readable=%d (sharp glass detail from the upscaled image)", u.name, unsigned(out_format), output_readable);
        d.composite_failed = !d.composite_ready;
        log(d.composite_ready ? "composite_init ok" : "ERROR composite_init failed (shader model 6.6 / 64-bit atomics unsupported?) - overlay disabled");
        if (!d.composite_ready) return;
    }
    // Descriptors for this frame (ring avoids overwriting ones the GPU may still read).
    const UINT first = UINT(d.frame++ % ring_size) * descriptors_per_frame;
    auto cpu = d.heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(first) * descriptor_size;
    auto gpu = d.heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(first) * descriptor_size;
    auto at = [&](UINT i) { auto h = cpu; h.ptr += SIZE_T(i) * descriptor_size; return h; };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{glass::map_format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Texture2D.MipLevels = 1;
    d.native->CreateShaderResourceView(map->texture.Get(), &srv, at(0));
    srv.Format = in_format;
    d.native->CreateShaderResourceView(colour, &srv, at(1));
    for (unsigned i = 0; i < slot_count; ++i) {
        auto resource = slot_resources[i].load();
        srv.Format = resource ? slot_formats[i] : DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.Texture2D.MipLevels = resource ? UINT(-1) : 1;
        d.native->CreateShaderResourceView(resource, &srv, at(2 + i));
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{out_format, D3D12_UAV_DIMENSION_TEXTURE2D};
    d.native->CreateUnorderedAccessView(output, nullptr, &uav, at(uav_base + 0));
    uav.Format = DXGI_FORMAT_R32_UINT;
    d.native->CreateUnorderedAccessView(map->tiles.Get(), nullptr, &uav, at(uav_base + 1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
    structured.Buffer = {0, slot_count * 2, 16, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    d.native->CreateUnorderedAccessView(d.params.Get(), nullptr, &structured, at(uav_base + 2));
    uav.Format = glass::map_format;
    d.native->CreateUnorderedAccessView(map->texture.Get(), nullptr, &uav, at(uav_base + 3));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.native->CreateUnorderedAccessView(map->residual.Get(), nullptr, &uav, at(uav_base + 4));

    struct { UINT map_w, map_h, out_w, out_h; float scale_x, scale_y, jitter_x, jitter_y, smoothing; UINT flags; float line_width; UINT pad; } constants{
        render_w, render_h, out_w, out_h, float(render_w) / out_w, float(render_h) / out_h,
        jitter_positive ? jitter_x : -jitter_x, jitter_positive ? jitter_y : -jitter_y, 0.25f, (tint ? 2u : 0u) | (output_readable ? 4u : 0u) | (smoothing_percent > 0 ? 8u : 0u),
        smoothing_percent / 50.0f, 0};

    api::command_list* api_list = list;
    auto native = reinterpret_cast<ID3D12GraphicsCommandList*>(api_list->get_native());
    ID3D12DescriptorHeap* heaps[]{d.heap.Get()};
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(d.root.Get());
    native->SetComputeRoot32BitConstants(0, 12, &constants, 0);
    native->SetComputeRootDescriptorTable(1, gpu);
    native->SetComputeRootUnorderedAccessView(2, d.sums->GetGPUVirtualAddress());
    // DLSS leaves its colour input compute-readable and the output as UAV; FSR reports the states it restores.
    const bool colour_moved = !(u.colour_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool output_moved = u.output_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (colour_moved) transition(native, colour, u.colour_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (output_moved) transition(native, output, u.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(native); // the upscaler just wrote the output
    if (!d.accumulators_zeroed) {
        native->SetPipelineState(d.passes[0].Get());
        native->Dispatch(1, 1, 1);
        uav_barrier(native);
        d.accumulators_zeroed = true;
        constants.flags |= 1;
        native->SetComputeRoot32BitConstants(0, 12, &constants, 0);
    }
    native->SetPipelineState(d.passes[1].Get());
    native->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    uav_barrier(native);
    native->SetPipelineState(d.passes[2].Get());
    native->Dispatch(1, 1, 1);
    uav_barrier(native);
    native->SetPipelineState(d.passes[3].Get());
    native->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(native);
    native->SetPipelineState(d.passes[4].Get());
    native->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, map_resting);
    uav_barrier(native);
    if (colour_moved) transition(native, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, u.colour_state);
    if (output_moved) transition(native, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, u.output_state);
    // Callers of NGX evaluate and ffxDispatch restore their own heaps, root signature and pipeline afterwards.
    last_render_w = render_w; last_render_h = render_h; last_out_w = out_w; last_out_h = out_h;
    last_composite_present = present_count.load(std::memory_order_relaxed);
    last_upscaler = u.name;
    if (stats.composites++ == 0) log("milestone first overlay frame drawn render=%ux%u output=%ux%u (fix is active)", render_w, render_h, out_w, out_h);
} catch (...) { ++stats.composite_skipped; }

// NGX evaluate hooks (driver export entry, i.e. the outermost NGX call).
using Evaluate = NgxEvaluate;
using EvaluateC = NgxEvaluateC;
Evaluate original_evaluate = nullptr;
EvaluateC original_evaluate_c = nullptr;
NVSDK_NGX_Result evaluate_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
    const auto result = original_evaluate(list, handle, params, callback);
    ++stats.evaluations;
    if (result == NVSDK_NGX_Result_Success) composite_dlss(list, params);
    else LOG_ONCE("note DLSS evaluate returned 0x%x (not success)", unsigned(result));
    return result;
}
NVSDK_NGX_Result evaluate_c_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback) {
    const auto result = original_evaluate_c(list, handle, params, callback);
    ++stats.evaluations;
    if (result == NVSDK_NGX_Result_Success) composite_dlss(list, params);
    else LOG_ONCE("note DLSS evaluate returned 0x%x (not success)", unsigned(result));
    return result;
}
// FSR (FidelityFX API, amd_fidelityfx_dx12.dll next to the game): ffxDispatch with an upscale description.
FfxDispatch original_ffx_dispatch = nullptr;
ffxReturnCode_t ffx_dispatch_hook(ffxContext* context, const ffxApiHeader* desc) {
    const auto result = original_ffx_dispatch(context, desc);
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        ++stats.fsr_dispatches;
        if (result == FFX_API_RETURN_OK) composite_fsr(*reinterpret_cast<const ffxDispatchDescUpscale*>(desc));
        else LOG_ONCE("note FSR dispatch returned 0x%x (not success)", unsigned(result));
    }
    return result;
}
bool ffx_hooked = false;
void install_ffx() {
    if (ffx_hooked) return;
    HMODULE module = GetModuleHandleW(L"amd_fidelityfx_dx12.dll");
    if (!module) return;
    auto dispatch = GetProcAddress(module, "ffxDispatch");
    HMODULE pinned = nullptr;
    if (dispatch) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(dispatch), &pinned);
    const bool hooked = dispatch && hook(reinterpret_cast<void*>(dispatch), reinterpret_cast<void*>(&ffx_dispatch_hook), original_ffx_dispatch);
    ffx_hooked = true;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    log("FSR runtime found: amd_fidelityfx_dx12.dll %s path=%ls hook=%d", file_version(path).c_str(), path, hooked);
    { std::lock_guard lock(info_mutex); fsr_version = file_version(path); }
}

bool ngx_hooked = false;
void install_ngx() {
    if (ngx_hooked) return;
    HMODULE module = GetModuleHandleW(L"_nvngx.dll");
    if (!module) return;
    auto evaluate = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto evaluate_c = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature_C");
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(evaluate), &pinned);
    const bool a = evaluate && hook(reinterpret_cast<void*>(evaluate), reinterpret_cast<void*>(&evaluate_hook), original_evaluate);
    const bool b = evaluate_c && hook(reinterpret_cast<void*>(evaluate_c), reinterpret_cast<void*>(&evaluate_c_hook), original_evaluate_c);
    ngx_hooked = true;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    log("milestone DLSS runtime found: _nvngx.dll %s path=%ls hooks evaluate=%d evaluate_c=%d", file_version(path).c_str(), path, a, b);
    if (!a && !b) log("ERROR could not hook DLSS evaluate - overlay cannot run");
}

// Settings live in an .ini next to the add-on (not in ReShade.ini).
std::wstring settings_path() { return (addon_directory / L"glass-cockpit-dlss-fix.ini").wstring(); }
void load_settings() {
    enabled = GetPrivateProfileIntW(L"overlay", L"enabled", 1, settings_path().c_str()) != 0;
    jitter_positive = GetPrivateProfileIntW(L"overlay", L"jitter_positive", 1, settings_path().c_str()) != 0;
    smoothing_percent = std::clamp(int(GetPrivateProfileIntW(L"overlay", L"line_smoothing", default_smoothing, settings_path().c_str())), 0, max_smoothing);
}
void save_settings() {
    WritePrivateProfileStringW(L"overlay", L"enabled", enabled ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"overlay", L"jitter_positive", jitter_positive ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"overlay", L"line_smoothing", std::to_wstring(smoothing_percent.load()).c_str(), settings_path().c_str());
}

void on_present(api::effect_runtime* runtime) {
    ++present_count;
    const bool chord = runtime->is_key_down(VK_CONTROL) && runtime->is_key_down(VK_SHIFT);
    if (chord && runtime->is_key_pressed(VK_F9)) glass::set_enabled(!enabled);
    if (chord && runtime->is_key_pressed(VK_F10)) glass::set_jitter_positive(!jitter_positive);
    if (chord && runtime->is_key_pressed(VK_F11)) glass::set_tint(!tint);
    static uint64_t next = 0;
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 10000;
    install_ngx();
    install_ffx();
    scan_modules();
    if (main_device) {
        const HRESULT removed = main_device->GetDeviceRemovedReason();
        if (removed != S_OK) LOG_ONCE("ERROR graphics device removed, reason=0x%08lx (0x887a0005 = hung/TDR, 0x887a0006 = driver error, 0x887a0001 = invalid call, 0x887a0020 = driver internal)", static_cast<unsigned long>(removed));
    }
    const auto minutes = (now - init_tick) / 60000.0;
    const char* found = nullptr;
    if (!ngx_hooked && !ffx_hooked && minutes > 2) {
        LOG_ONCE("WARNING neither DLSS (_nvngx.dll) nor FSR (amd_fidelityfx_dx12.dll) loaded after 2 minutes: is DLSS or FSR selected in the sim's graphics settings?");
        found = "No upscaler loaded. Select DLSS or FSR in the sim's graphics settings.";
    }
    if (!stats.display_pipelines && minutes > 5) {
        LOG_ONCE("WARNING no cockpit display shader seen after 5 minutes: not in a glass cockpit yet, or a sim update changed the shader (fix needs an update)");
        if (!found) found = "No cockpit display shader seen yet. Load a flight with a glass cockpit. If you are in one, a sim update may have changed the shader and the fix needs an update.";
    }
    static uint64_t display_since = 0;
    if (stats.display_draws && !display_since) display_since = now;
    if (display_since && now - display_since > 20000 && !found) {
        if (!stats.evaluations && !stats.fsr_dispatches) {
            LOG_ONCE("WARNING cockpit screens are drawn but neither DLSS nor FSR is called: select DLSS or FSR (not TAA) in the sim's graphics settings");
            found = "Cockpit screens are drawn but neither DLSS nor FSR is running. Select DLSS or FSR (not TAA) in the sim's graphics settings.";
        } else if (!stats.replays) {
            LOG_ONCE("WARNING cockpit screens are drawn but none could be recorded: see skip reasons above");
            found = "Cockpit screens are drawn but could not be recorded. Please report this with your log files.";
        } else if (!stats.composites && enabled) {
            LOG_ONCE("WARNING screens recorded and the upscaler running, but no overlay frame yet: see skip reasons above");
            found = "The upscaler is running and screens are recorded, but the fix could not be drawn yet. Please report this with your log files.";
        }
    }
    if (stats.composites) found = nullptr;
    problem = found;
    // Stats only when something changed, to keep the log small.
    static std::array<uint64_t, 6> last{};
    const std::array<uint64_t, 6> current{stats.replays.load(), stats.composites.load(), stats.composite_skipped.load(), stats.unknown_texture.load(),
        stats.unsupported_draw.load(), uint64_t(enabled.load())};
    if (current != last) {
        last = current;
        log("stats overlay=%d jitter_positive=%d dlss_calls=%llu fsr_calls=%llu screen_draws=%llu recorded=%llu overlay_frames=%llu skipped=%llu shader_pending=%llu unknown_texture=%llu unsupported=%llu",
            enabled.load(), jitter_positive.load(), stats.evaluations.load(), stats.fsr_dispatches.load(), stats.display_draws.load(), stats.replays.load(), stats.composites.load(),
            stats.composite_skipped.load(), stats.variant_pending.load(), stats.unknown_texture.load(), stats.unsupported_draw.load());
    }
}
}

namespace glass {
Status status() {
    Status s;
    {
        std::lock_guard lock(info_mutex);
        s.gpu = gpu_name; s.driver = gpu_driver; s.dlss_version = dlss_version; s.fsr_version = fsr_version;
    }
    s.gpu_known = gpu_known; s.nvidia = gpu_nvidia; s.dlss_loaded = ngx_hooked; s.fsr_loaded = ffx_hooked; s.shader_found = stats.display_pipelines > 0;
    s.enabled = enabled; s.tint = tint; s.jitter_positive = jitter_positive; s.line_smoothing = smoothing_percent;
    s.dlss_calls = stats.evaluations; s.fsr_calls = stats.fsr_dispatches; s.upscaler = last_upscaler; s.screen_draws = stats.display_draws; s.recorded = stats.replays;
    s.overlay_frames = stats.composites; s.skipped = stats.composite_skipped;
    const auto now = present_count.load(std::memory_order_relaxed);
    s.active = s.enabled && s.overlay_frames && now <= last_composite_present + 30;
    for (unsigned i = 0; i < slot_count; ++i) if (slot_resources[i].load() && now <= slot_last_used[i] + 30) ++s.screens;
    s.render_w = last_render_w; s.render_h = last_render_h; s.out_w = last_out_w; s.out_h = last_out_h;
    s.problem = problem;
    s.log_path = (addon_directory / L"glass-cockpit-dlss-fix.log").wstring();
    return s;
}
void set_enabled(bool on) {
    if (on == enabled) return;
    if (on) ++generation;
    enabled = on; save_settings(); log("setting overlay=%d", int(on));
}
void set_tint(bool on) { tint = on; log("setting tint=%d", int(on)); }
void set_line_smoothing(int percent) { smoothing_percent = std::clamp(percent, 0, max_smoothing); save_settings(); log("setting line_smoothing=%d", smoothing_percent.load()); }
void set_jitter_positive(bool on) { jitter_positive = on; save_settings(); log("setting jitter_positive=%d", int(on)); }
}

extern "C" __declspec(dllexport) const char* NAME = GLASS_NAME;
extern "C" __declspec(dllexport) const char* AUTHOR = GLASS_AUTHOR;
extern "C" __declspec(dllexport) const char* WEBSITE = GLASS_WEBSITE;
extern "C" __declspec(dllexport) const char* ISSUES = GLASS_ISSUES;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Removes DLSS ghosting from MSFS 2024 glass cockpit displays. Ctrl+Shift+F9 on/off, Ctrl+Shift+F10 flip jitter direction, Ctrl+Shift+F11 green tint.";

#define OVERLAY_EVENTS(X) \
    X(init_device, on_init_device) X(destroy_device, on_destroy_device) X(init_pipeline_layout, on_init_layout) \
    X(init_resource_view, on_init_view) X(destroy_resource, on_destroy_resource) \
    X(init_command_list, on_init_list) X(destroy_command_list, on_destroy_list) X(reset_command_list, on_reset_list) \
    X(bind_pipeline, on_bind_pipeline) X(bind_descriptor_tables, on_bind_tables) \
    X(bind_render_targets_and_depth_stencil, on_bind_targets) X(begin_render_pass, on_begin_pass) X(end_render_pass, on_end_pass) \
    X(draw, on_draw) X(draw_indexed, on_draw_indexed) X(reshade_present, on_present)

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshade_module) {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"FlightSimulator2024.exe") != 0) return false;
    if (MH_Initialize() != MH_OK || !reshade::register_addon(addon, reshade_module)) return false;
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(addon, module, MAX_PATH);
    addon_directory = std::filesystem::path(module).parent_path();
    // Offline test host: the stand-in display shader sits next to the fake FlightSimulator2024.exe.
    const auto fixture_directory = std::filesystem::path(executable).parent_path();
    std::error_code error;
    const bool fixture = std::filesystem::exists(fixture_directory / L"fixture_ps.cso", error);
    if (fixture) {
        auto hash_file = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
            return shader_hash(bytes.data(), bytes.size());
        };
        display_ps = hash_file(fixture_directory / L"fixture_ps.cso");
        display_vs = hash_file(fixture_directory / L"fixture_vs.cso");
    }
    // Keep the previous session's log: after a crash people usually relaunch before sending it.
    const auto log_path = addon_directory / L"glass-cockpit-dlss-fix.log";
    std::filesystem::rename(log_path, addon_directory / L"glass-cockpit-dlss-fix.old.log", error);
    logfile = _wfsopen(log_path.c_str(), L"w", _SH_DENYWR);
    if (!fixture) load_settings();
    init_tick = GetTickCount64();
    OSVERSIONINFOEXW os{sizeof(os)};
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&os);
    wchar_t reshade_path[MAX_PATH]{};
    GetModuleFileNameW(reshade_module, reshade_path, MAX_PATH);
    log("Glass Cockpit DLSS Fix %s (built " __DATE__ " " __TIME__ ")", overlay_version);
    log("windows %lu.%lu.%lu  game=%ls", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, executable);
    log("reshade %s path=%ls", file_version(reshade_path).c_str(), reshade_path);
    log("addon path=%ls fixture=%d enabled=%d jitter_positive=%d line_smoothing=%d", module, fixture, enabled.load(), jitter_positive.load(), smoothing_percent.load());
    log("hotkeys: Ctrl+Shift+F9 overlay on/off, Ctrl+Shift+F10 flip jitter direction, Ctrl+Shift+F11 green tint");
    scan_modules();
    std::thread(worker).detach();
#define REGISTER(event, callback) reshade::register_event<reshade::addon_event::event>(callback);
    OVERLAY_EVENTS(REGISTER)
#undef REGISTER
    log("status panel %s", glass::init_status_panel(reshade_module) ? "added to the ReShade overlay" : "unavailable (this ReShade build has a different ImGui version); the fix itself still works");
    return true;
}
extern "C" __declspec(dllexport) void GlassFixJitterPositive(bool positive) { jitter_positive = positive; }
extern "C" __declspec(dllexport) void GlassFixLineSmoothing(int percent) { smoothing_percent = percent; }
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE) {
    enabled = false;
    glass::uninit_status_panel();
#define UNREGISTER(event, callback) reshade::unregister_event<reshade::addon_event::event>(callback);
    OVERLAY_EVENTS(UNREGISTER)
#undef UNREGISTER
    reshade::unregister_addon(addon);
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

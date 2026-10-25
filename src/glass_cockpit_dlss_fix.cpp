// Glass Cockpit DLSS Fix for Microsoft Flight Simulator 2024. Removes DLSS ghosting/smearing from glass cockpit displays
// by redrawing their content after DLSS.
// https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix  (GPL-3.0-only from 1.0.0, see NOTICE.md; 0.8.3 and earlier MIT)
//
// This file is the fix itself. It is built two ways, which differ only in how the fix gets loaded (host.hpp):
// - standalone: dxgi.dll next to the sim, no ReShade needed (dxgi_proxy.cpp);
// - ReShade add-on: glass-cockpit-dlss-fix.addon64, with a status and settings tab in ReShade's overlay
//   (reshade_addon.cpp, status_panel.cpp).
//
// 1. Every cockpit display draw (MSFS display shader, VS f102f1e3f5d9f66f / PS 37e87c938bc76310) is drawn once more
//    into a private render-resolution "UV map" (uv, depth, display slot) using the game's own vertex shader,
//    bindings and depth test. The bound display texture is identified by its descriptor bytes.
// 2. Right after DLSS evaluate, on the same command list, compute passes fit each display's brightness against the
//    DLSS input colour, sample the clean display texture at output resolution (jitter removed) and write it over the
//    DLSS output. Dirt, glare and shadows on the glass come from the DLSS output itself, clamped to this frame's
//    local range so that trails of old screen content are removed.
// The draws, bindings and views it needs are followed with hooks on the D3D12 runtime's own functions (see "D3D12
// hooks" below), the same in both builds: under ReShade they sit below ReShade's own wrappers.
// Hotkeys: Ctrl+Shift+F9 on/off, Ctrl+Shift+F11 green tint (shows coverage). Everything else is
// in glass-cockpit-dlss-fix.ini next to the DLL, re-read while the sim runs (and in the overlay tab of the add-on).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <MinHook.h>
#include "ngx_interface.hpp"
#include "ffx_interface.hpp"
#include <wrl/client.h>
#include <wincodec.h>
#include <cmath>
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
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "pipeline_clone.hpp"
#include "overlay_shaders.h"
#include "host.hpp"
#include "status.hpp"
#include "version.h"

using Microsoft::WRL::ComPtr;

size_t graphics_pipeline_slot();
size_t pipeline_stream_slot();
size_t copy_descriptors_slot();
size_t copy_descriptors_simple_slot();
size_t create_descriptor_heap_slot();
size_t create_root_signature_slot();
size_t create_srv_slot();
size_t create_uav_slot();
size_t create_rtv_slot();
size_t create_command_list_slot();
size_t create_command_list1_slot();
size_t create_command_signature_slot();
size_t list_reset_slot();
size_t list_clear_state_slot();
size_t list_draw_slot();
size_t list_draw_indexed_slot();
size_t list_copy_texture_region_slot();
size_t list_set_pipeline_state_slot();
size_t list_set_graphics_root_signature_slot();
size_t list_set_graphics_root_table_slot();
size_t list_set_render_targets_slot();
size_t list_begin_render_pass_slot();
size_t list_end_render_pass_slot();
size_t list_set_scissor_rects_slot();
size_t list_execute_indirect_slot();
size_t queue_execute_slot();
size_t factory_create_swapchain_slot();
size_t factory_create_swapchain_hwnd_slot();
size_t factory_create_swapchain_core_window_slot();
size_t factory_create_swapchain_composition_slot();
size_t swapchain_present_slot();
size_t swapchain_present1_slot();


namespace {
// MSFS 2024 cockpit display shader pair (replaced by the stand-in shaders' hashes in the offline test host).
uint64_t display_vs = 0xf102f1e3f5d9f66fULL, display_ps = 0x37e87c938bc76310ULL;
// Other pixel shaders used with the display vertex shader that bind like display_ps (screen texture in t1, material in b3).
// They shade the screen differently; the map shader only needs the bindings. FlightFX C750 main screens.
constexpr uint64_t display_ps_alike[] = {0x36cc0d164f99b984ULL};
// Display shader variant with the screen texture in t0 and the material constants in b4 (iniBuilds A340 main screens).
uint64_t display_ps_t0 = 0xd3646ba0fd9007e9ULL, vr_display_ps_t0 = 0xf24aebba0272f765ULL; // flat and VR
// The same display drawn in VR: instanced stereo, one instance per view into a layered render target.
uint64_t vr_display_vs = 0x715948137700a5d6ULL, vr_display_ps = 0x9ba9df5bca0ad4b9ULL;

uint64_t shader_hash(const void* bytes, size_t size) { // FNV-1a over the bytecode
    auto data = static_cast<const unsigned char*>(bytes);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ULL;
    return size ? hash : 0;
}
// Per frame: map, scene colour, one SRV per display slot, then output, tiles, params, map, residual, the two temporal
// anti-aliasing history UAVs and the learnt screen edges.
// ring_size: descriptor sets in flight, one per upscaler call. With DLSS Frame Generation the sim calls the upscaler up to
// three times per frame, and the GPU can be several frames behind the CPU (more so with the game in the background): a set
// must not be rewritten while the GPU may still read it.
constexpr unsigned slot_count = 32, ring_size = 32, uav_base = 2 + slot_count, descriptors_per_frame = uav_base + 12; // + depth (t34), capture (u11)
constexpr unsigned bounds_cells = 64; // stable screen edges: cells per side of each screen texture's grid
std::filesystem::path addon_directory; // the folder this DLL is in (the sim's): log, settings, captures
ID3D12Device* main_device = nullptr;
struct DeviceState;
DeviceState* main_state = nullptr; // the main device's state (the offline test host's cost tick)
uint64_t init_tick = 0;

// Loaded by ReShade as its add-on (reshade_addon.cpp) rather than as dxgi.dll (dxgi_proxy.cpp).
bool reshade_addon = false;
const char* edition() { return reshade_addon ? "ReShade add-on" : "standalone"; }

std::atomic<bool> enabled{true}, jitter_positive{true}, tint{false};
// Standalone: the ReShade add-on build of this fix is loaded as well. It does the work, this copy stays off.
std::atomic<bool> duplicate_fix{false};
}
// Standalone: opens and closes the settings window (settings_window.cpp); null in the add-on, which has its ReShade tab.
void (*glass::settings_window_hotkey)() = nullptr;
std::atomic<bool> glass::settings_window_capturing{false};
std::atomic<uint32_t> settings_window_key{glass::default_settings_window_key};
namespace {
// The process is exiting (DLL_PROCESS_DETACH): the hooks pass everything straight through from then on, since the
// fix's own state is being torn down while other modules may still release Direct3D objects.
std::atomic<bool> shutting_down{false};
// Experimental fixes (panel section of the same name): detail clamp and angle softening in percent, edge fade and edge
// stretcher and edge trim in hundredths of a render pixel.
constexpr int default_detail_clamp = 0, max_edge_fade = 250, max_edge_stretch = 400, max_edge_trim = 400;
std::atomic<int> detail_clamp_percent{default_detail_clamp}, edge_fade_percent{0}, angle_soften_percent{0}, edge_stretch_percent{0}, edge_trim_percent{0};
// Dark screen fix (experimental, on/off): also judge the screen match on fine detail (keeps dark screens fixed in sun glare).
std::atomic<int> dark_screen_fix{0};
// UV seam fix (on by default): don't blend texture coordinates across a jump inside one screen mesh.
std::atomic<int> seam_fix{1};
// Temporal anti-aliasing (experimental, on/off): screens accumulated over frames like the game's TAA, history dropped
// wherever a screen's content changes.
std::atomic<int> temporal_aa{0};
// Soft-edged line smoothing (experimental, on/off): Line smoothing weighted towards the pixel's centre over a wider area
// (Gaussian) instead of evenly over it (box). Less shimmer in motion at about the same sharpness.
std::atomic<int> soft_smoothing{0};
// Stable screen edges (on by default): where each screen ends is learnt in its texture, so the camera jitter doesn't
// make the pixels along its edge flip between the fix and the upscaler every frame.
std::atomic<int> stable_edges{1};
// Calm glass at symbols (off by default): next to symbol outlines the glass layer is only the soft average of the sim's
// remainder around the pixel, without the sharp detail taken from the upscaler's picture: DLSS's lines sit slightly
// elsewhere than the redrawn ones, and that difference changes every frame.
std::atomic<int> calm_glass{0};
// Occlusion check (experimental, on/off): screen pixels where the sim's depth buffer shows something closer to the camera
// than the screen (a sidestick, a hand, the yoke drawn after the screen) are left to the upscaler.
std::atomic<int> occlusion_check{1};
// Depth direction of the cockpit display pipelines' depth test: 0 unknown, 1 reversed (closer = larger), 2 standard.
std::atomic<int> depth_direction{0};
std::atomic<uint32_t> bounds_reset{0}; // slots whose learnt edges must be forgotten (the slot took another texture)
std::atomic<uint32_t> debug_flags{0}; // offline test only: extra shader flag bits
// Line smoothing 0-50 %: 0 = the screen texture as sharp as the sim samples it, 35 = default, 50 = DLSS-like.
// With Extra smoothing on it goes to 100 % (twice the DLSS-like footprint: softer than DLSS, for stubborn shimmer).
constexpr int max_smoothing = 50, max_extra_smoothing = 100, default_smoothing = 35;
std::atomic<bool> extra_smoothing{false};
int smoothing_limit() { return extra_smoothing.load() ? max_extra_smoothing : max_smoothing; }
std::atomic<int> smoothing_percent{default_smoothing};
// Advanced setting: share of the glass layer kept on fixed screens, 0-100 %.
std::atomic<int> glass_percent{100};
// Bumped when the overlay is switched on: lists recorded while it was off have stale binding state.
std::atomic<uint32_t> generation{1};
FILE* logfile = nullptr;
std::filesystem::path log_file_path; // the log this session writes (glass-cockpit-dlss-fix.log, or a fallback name)
std::mutex log_mutex;
void log_args(const char* format, va_list args) {
    std::lock_guard lock(log_mutex);
    if (!logfile) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(logfile, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    vfprintf(logfile, format, args);
    fputc('\n', logfile); fflush(logfile);
}
void log(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}

constexpr const char* overlay_version = GLASS_VERSION_STRING;

// Shown in the status file (glass-cockpit-dlss-fix-status.txt).
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

// Cost diagnostics: every 10 s the fix's own CPU time (inside the D3D12/upscaler hooks) and GPU
// time (the passes after the upscaler) are measured over 120 frames and logged. Off in between: one relaxed load per event.
enum CpuSlot { cpu_draws, cpu_binds, cpu_targets, cpu_passes, cpu_views, cpu_copies, cpu_descriptors, cpu_composite, cpu_slots };
constexpr const char* cpu_slot_names[cpu_slots]{"draws", "binds", "targets", "passes", "views", "copies", "descriptors", "composite"};
std::atomic<bool> measuring{false};
std::array<std::atomic<uint64_t>, cpu_slots> cpu_ticks{}, cpu_calls{};
// The frequent events (hundreds of thousands per second) are sampled, 1 in 64 per thread, and scaled: timing every call
// would cost more than the work measured. The rare ones are timed every time.
constexpr uint32_t cpu_sample_rate[cpu_slots]{64, 64, 64, 64, 1, 1, 64, 1};
// What reading the clock twice costs by itself (it is part of every sample): measured once, subtracted from each sample.
int64_t clock_overhead() {
    static const int64_t overhead = [] {
        int64_t best = INT64_MAX;
        for (int i = 0; i < 2000; ++i) {
            LARGE_INTEGER a, b; QueryPerformanceCounter(&a); QueryPerformanceCounter(&b);
            best = std::min<int64_t>(best, b.QuadPart - a.QuadPart);
        }
        return best == INT64_MAX ? 0 : best;
    }();
    return overhead;
}
struct CpuScope {
    int slot; bool on = false; LARGE_INTEGER start{};
    explicit CpuScope(int s) : slot(s) {
        if (!measuring.load(std::memory_order_relaxed)) return;
        thread_local std::array<uint32_t, cpu_slots> counter{};
        if (counter[slot]++ % cpu_sample_rate[slot]) return;
        on = true;
        QueryPerformanceCounter(&start);
    }
    ~CpuScope() {
        if (!on) return;
        LARGE_INTEGER end; QueryPerformanceCounter(&end);
        const int64_t ticks = std::max<int64_t>(end.QuadPart - start.QuadPart - clock_overhead(), 0);
        cpu_ticks[slot].fetch_add(uint64_t(ticks) * cpu_sample_rate[slot], std::memory_order_relaxed);
        cpu_calls[slot].fetch_add(cpu_sample_rate[slot], std::memory_order_relaxed);
    }
};
std::atomic<uint64_t> timestamp_frequency{0}; // of the queue the sim presents on (the upscaler runs on it too)
constexpr UINT max_timed_calls = 256;

struct Stats {
    std::atomic<uint64_t> repeated_calls{0}; // upscaler calls repeated for a frame already fixed (frame generation)
    std::atomic<uint64_t> replays{0}, variant_pending{0}, unknown_texture{0}, unsupported_draw{0},
        composites{0}, composite_skipped{0}, evaluations{0}, fsr_dispatches{0}, display_draws{0}, display_pipelines{0},
        dlss_upscales{0}; // evaluations that upscale (have a render size); the others are DLSS Frame Generation
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

// Company name from the file's version resource ("" if none).
std::wstring file_company(const wchar_t* path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    struct Translation { WORD language, codepage; }* translations = nullptr; UINT length = 0;
    if (!GetFileVersionInfoW(path, 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) || length < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].language, translations[0].codepage);
    wchar_t* company = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&company), &length) || !company) return {};
    return company;
}

// Logs DLSS/Streamline/ReShade-related modules once each, with version and path (helps spot missing DLSS or conflicts).
// Any other module loaded more than a minute after start is logged too: tools that attach to the running sim (e.g.
// GlassOut) show up there. Those by name and path only: their files are never opened (the FSLabs A32X's protected
// interface DLL unloads itself when another module reads its file, and the sim crashed loading the aircraft).
void scan_modules() {
    static std::set<std::wstring> seen, all;
    static int late = 0;
    const bool settled = GetTickCount64() - init_tick > 60000;
    HMODULE modules[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (auto& c : name) c = wchar_t(towlower(c));
        const bool new_module = all.insert(path).second;
        const bool relevant = name.starts_with(L"nvngx") || name.starts_with(L"_nvngx") || name.starts_with(L"sl.") ||
            name.ends_with(L".addon64") || name.ends_with(L".addon") || name.starts_with(L"reshade") || name == L"d3d12core.dll" ||
            name == L"dxgi.dll" || name == L"version.dll" || name == L"nvapi64.dll";
        if (!relevant && new_module && settled && late < 64) {
            ++late;
            log("module loaded later: %ls path=%ls", name.c_str(), path);
        }
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
    bool vr = false;
    bool screen_t0 = false; // screen texture in t0 (display_ps_t0)
    bool brightness = false; // the standard display shader (flat or VR): its map variant records each mesh's brightness (b2)
    std::array<std::atomic<ID3D12PipelineState*>, slot_count> variants{};
    std::array<std::atomic<bool>, slot_count> requested{};
    std::atomic<ID3D12PipelineState*> colour_variant{}; // screen capture: writes the screen's colour at draw time
    std::atomic<bool> colour_requested{};
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
const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders_brightness{{
#define SLOT(n) {map_ps_b_##n, sizeof(map_ps_b_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};
const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders_vr_brightness{{
#define SLOT(n) {map_ps_vr_b_##n, sizeof(map_ps_vr_b_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};
const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders_vr{{
#define SLOT(n) {map_ps_vr_##n, sizeof(map_ps_vr_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};
const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders_t0{{
#define SLOT(n) {map_ps_t0_##n, sizeof(map_ps_t0_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};
const std::array<std::pair<const unsigned char*, size_t>, slot_count> map_shaders_vr_t0{{
#define SLOT(n) {map_ps_vr_t0_##n, sizeof(map_ps_vr_t0_##n)},
    SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7)
    SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
    SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
    SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
}};
const std::pair<const unsigned char*, size_t>& map_shader(bool vr, bool screen_t0, unsigned slot) {
    return vr ? (screen_t0 ? map_shaders_vr_t0[slot] : map_shaders_vr[slot]) : screen_t0 ? map_shaders_t0[slot] : map_shaders[slot];
}
const std::pair<const unsigned char*, size_t>& map_shader(const DisplayPipeline& pipeline, unsigned slot) {
    if (pipeline.brightness) return pipeline.vr ? map_shaders_vr_brightness[slot] : map_shaders_brightness[slot];
    return map_shader(pipeline.vr, pipeline.screen_t0, slot);
}

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
        const bool colour = slot == slot_count; // the screen capture's colour variant
        const std::pair<const unsigned char*, size_t> shader = colour
            ? (pipeline->screen_t0 ? std::pair<const unsigned char*, size_t>{map_colour_ps_t0, sizeof(map_colour_ps_t0)} : std::pair<const unsigned char*, size_t>{map_colour_ps, sizeof(map_colour_ps)})
            : map_shader(*pipeline, slot);
        if (!glass::clone_pipeline(pipeline->desc, {shader.first, shader.second}, clone)) {
            log("variant_rejected slot=%u", slot); continue;
        }
        ID3D12PipelineState* created = nullptr;
        auto hr = pipeline->device->CreateGraphicsPipelineState(&clone, IID_PPV_ARGS(&created));
        if (FAILED(hr) && !colour && pipeline->brightness) {
            // The pipeline's root signature doesn't give the pixel shader the per-instance data (b2): the plain map
            // shader instead (one brightness per screen texture, as before).
            LOG_ONCE("display pipeline without per-mesh brightness (hr=0x%lx): plain screen map used", hr);
            const auto& plain = map_shader(pipeline->vr, false, slot);
            if (glass::clone_pipeline(pipeline->desc, {plain.first, plain.second}, clone))
                hr = pipeline->device->CreateGraphicsPipelineState(&clone, IID_PPV_ARGS(&created));
        }
        if (FAILED(hr)) { log("variant_failed slot=%u hr=0x%lx", slot, hr); continue; }
        if (colour) pipeline->colour_variant = created;
        else pipeline->variants[slot] = created;
    }
}
ID3D12PipelineState* colour_variant(DisplayPipeline* pipeline) {
    if (auto ready = pipeline->colour_variant.load(std::memory_order_acquire)) return ready;
    if (!pipeline->vr && !pipeline->colour_requested.exchange(true)) {
        { std::lock_guard lock(work_mutex); work.emplace_back(pipeline, slot_count); }
        work_ready.notify_one();
    }
    return nullptr;
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

std::shared_mutex hashes_mutex;
std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> pipeline_hashes; // pso -> vs, ps (probes)

void pipeline_created(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState* pso,
    const D3D12_VIEW_INSTANCING_DESC& views = {}) noexcept try {
    if (!desc.PS.pShaderBytecode || !desc.VS.pShaderBytecode) return;
    const auto ps = shader_hash(desc.PS.pShaderBytecode, desc.PS.BytecodeLength), vs = shader_hash(desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
    {
        std::unique_lock lock(hashes_mutex);
        if (pipeline_hashes.size() < 65536) pipeline_hashes[reinterpret_cast<uint64_t>(pso)] = {vs, ps};
    }
    if (views.ViewInstanceCount > 1) return; // view-instanced pipelines: not how MSFS draws its displays
    const bool vr = vs == vr_display_vs && (ps == vr_display_ps || ps == vr_display_ps_t0);
    const bool screen_t0 = (ps == display_ps_t0 && vs == display_vs) || (ps == vr_display_ps_t0 && vs == vr_display_vs);
    const bool alike = vs == display_vs && std::find(std::begin(display_ps_alike), std::end(display_ps_alike), ps) != std::end(display_ps_alike);
    if (!vr && !screen_t0 && !alike && (ps != display_ps || vs != display_vs)) return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC probe{};
    const auto& shader = map_shader(vr, screen_t0, 0);
    if (!glass::clone_pipeline(desc, {shader.first, shader.second}, probe)) {
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
    copy->vr = vr;
    copy->screen_t0 = screen_t0;
    copy->brightness = !screen_t0 && (vr ? ps == vr_display_ps : ps == display_ps);
    std::lock_guard lock(pipelines_mutex);
    for (size_t i = 0; i < max_pipelines; ++i) if (!pipeline_keys[i].load()) {
        pipeline_values[i] = copy;
        pipeline_keys[i].store(reinterpret_cast<uint64_t>(pso), std::memory_order_release);
        if (stats.display_pipelines++ == 0) log("milestone cockpit display shader found (glass cockpit screens can be recognised)");
        const auto func = desc.DepthStencilState.DepthFunc;
        const int direction = !desc.DepthStencilState.DepthEnable ? 0
            : func == D3D12_COMPARISON_FUNC_GREATER || func == D3D12_COMPARISON_FUNC_GREATER_EQUAL ? 1
            : func == D3D12_COMPARISON_FUNC_LESS || func == D3D12_COMPARISON_FUNC_LESS_EQUAL ? 2 : 0;
        if (direction && depth_direction.exchange(direction) != direction) log("depth direction %s (display depth test %u)", direction == 1 ? "reversed" : "standard", unsigned(func));
        log("display_pipeline pso=%p root=%p targets=%u depth_format=%u vr=%d screen_t0=%d", pso, desc.pRootSignature, desc.NumRenderTargets,
            unsigned(desc.DSVFormat), vr, screen_t0);
        if (vr) LOG_ONCE("milestone VR cockpit display shader found");
        if (screen_t0) LOG_ONCE("milestone cockpit display shader variant found (screen texture in t0, e.g. iniBuilds A340)");
        if (alike) log("display shader variant ps=%016llx found (binds like the standard one, e.g. FlightFX C750)", ps);
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
        D3D12_VIEW_INSTANCING_DESC views{};
        unsigned unknown = 0;
        if (glass::decode_stream(*desc, graphics, &views, &unknown)) pipeline_created(device, graphics, static_cast<ID3D12PipelineState*>(*out), views);
        else if (unknown) {
            static std::mutex types_mutex;
            static std::set<unsigned> types;
            std::lock_guard lock(types_mutex);
            if (types.size() < 8 && types.insert(unknown).second) log("pipeline_stream_unknown_subobject type=%u (pipelines with it are not inspected)", unknown);
        }
    }
    return hr;
}
MH_STATUS last_hook_status = MH_OK; // why the last hook() failed (diagnostics)
template <class T> bool hook(void* target, void* detour, T& original) {
    if (original) return true;
    void* trampoline = nullptr;
    if ((last_hook_status = MH_CreateHook(target, detour, &trampoline)) != MH_OK) return false;
    original = reinterpret_cast<T>(trampoline);
    return (last_hook_status = MH_EnableHook(target)) == MH_OK;
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
// Mip levels of the game's own view of each screen texture (first level, count; UINT(-1) = all from first). The fix
// samples exactly those: a sim that never fills a texture's smaller mips (iFly 737 CDUs and radios) only views level 0.
std::array<UINT, slot_count> slot_first_mip{}, slot_mip_count{};
std::mutex view_mips_mutex;
std::unordered_map<ID3D12Resource*, std::pair<UINT, UINT>> view_mips;
// Full-size sampling (experimental section, on by default): screen textures are read at full
// size only, their smaller mip levels flicker as the view moves. Off: the game's own mip levels, as before 0.8.4.
std::atomic<bool> mip0_only{true};
std::array<std::atomic<uint64_t>, slot_count> slot_last_used{};
std::atomic<uint64_t> present_count{0};
std::mutex slots_mutex;
UINT descriptor_size = 0;

// Descriptor tracking (all GPUs but NVIDIA). AMD's descriptors contain their own address, so a copied descriptor's
// bytes never equal the original's. Instead the views' addresses are followed through the game's descriptor copies.
// The sim copies descriptors millions of times per second, so this must cost next to nothing per copy: every
// CBV/SRV/UAV heap gets a mirror array with one texture id per descriptor (0 = not a screen texture candidate), and a
// copy just copies those ids. No locks on the copy path: heaps are looked up in an immutable snapshot, and the game
// never writes the same descriptors from two threads at once (D3D12 rule), so neither do the mirrors.
std::atomic<bool> track_descriptors{false};
bool force_tracking = false; // offline test: identify textures by tracking only
struct MirrorHeap { uintptr_t start = 0, end = 0; std::unique_ptr<std::atomic<uint32_t>[]> ids; };
struct HeapSnapshot { std::vector<MirrorHeap*> heaps; }; // sorted by start, never changed once published
std::mutex heaps_mutex;                                   // creating heaps (rare)
std::vector<std::unique_ptr<MirrorHeap>> heap_storage;   // kept until the device goes: a copy may still be reading one
std::vector<std::unique_ptr<HeapSnapshot>> snapshot_storage;
std::atomic<const HeapSnapshot*> heap_snapshot{nullptr};
// Texture ids: index into a fixed table (resource, format); a destroyed texture's entry is cleared, never reused.
constexpr uint32_t max_texture_ids = 65536;
struct TextureId { std::atomic<ID3D12Resource*> resource{nullptr}; std::atomic<DXGI_FORMAT> format{DXGI_FORMAT_UNKNOWN}; };
std::unique_ptr<TextureId[]> texture_ids;
std::atomic<uint32_t> texture_id_count{1}; // id 0 = none
std::mutex texture_ids_mutex;
std::map<std::pair<ID3D12Resource*, DXGI_FORMAT>, uint32_t> texture_id_of;

MirrorHeap* mirror_of(uintptr_t address, size_t span = 1) {
    const auto snapshot = heap_snapshot.load(std::memory_order_acquire);
    if (!snapshot) return nullptr;
    thread_local const HeapSnapshot* cached_snapshot = nullptr;
    thread_local MirrorHeap* cached = nullptr;
    if (cached_snapshot == snapshot && cached && address >= cached->start && address + span <= cached->end) return cached;
    const auto& heaps = snapshot->heaps;
    auto it = std::upper_bound(heaps.begin(), heaps.end(), address, [](uintptr_t a, const MirrorHeap* h) { return a < h->start; });
    if (it == heaps.begin()) return nullptr;
    MirrorHeap* heap = *--it;
    if (address + span > heap->end) return nullptr;
    cached_snapshot = snapshot; cached = heap;
    return heap;
}
void register_heap(uintptr_t start, UINT count) {
    if (!count || !descriptor_size) return;
    auto heap = std::make_unique<MirrorHeap>();
    heap->start = start; heap->end = start + SIZE_T(count) * descriptor_size;
    heap->ids = std::make_unique<std::atomic<uint32_t>[]>(count); // value-initialised: all 0
    std::lock_guard lock(heaps_mutex);
    auto snapshot = std::make_unique<HeapSnapshot>();
    if (const auto current = heap_snapshot.load()) // a heap at the same addresses replaces released ones there
        for (auto h : current->heaps) if (h->end <= heap->start || h->start >= heap->end) snapshot->heaps.push_back(h);
    snapshot->heaps.push_back(heap.get());
    std::sort(snapshot->heaps.begin(), snapshot->heaps.end(), [](const MirrorHeap* a, const MirrorHeap* b) { return a->start < b->start; });
    heap_storage.push_back(std::move(heap));
    heap_snapshot.store(snapshot.get(), std::memory_order_release);
    snapshot_storage.push_back(std::move(snapshot));
    const size_t heaps = heap_storage.size();
    if ((heaps & (heaps - 1)) == 0) log("descriptor tracking: %llu heaps mirrored (latest %u descriptors)", uint64_t(heaps), count);
}
uint32_t texture_id(ID3D12Resource* resource, DXGI_FORMAT format) {
    std::lock_guard lock(texture_ids_mutex);
    if (const auto found = texture_id_of.find({resource, format}); found != texture_id_of.end()) return found->second;
    const uint32_t id = texture_id_count.load();
    if (id >= max_texture_ids) {
        LOG_ONCE("descriptor tracking: texture table full, new screen textures are not tracked");
        return 0;
    }
    texture_ids[id].format = format;
    texture_ids[id].resource = resource;
    texture_id_count = id + 1;
    texture_id_of[{resource, format}] = id;
    return id;
}
void forget_texture_ids(ID3D12Resource* resource) {
    std::lock_guard lock(texture_ids_mutex);
    for (auto it = texture_id_of.lower_bound({resource, DXGI_FORMAT_UNKNOWN}); it != texture_id_of.end() && it->first.first == resource;) {
        texture_ids[it->second].resource = nullptr;
        it = texture_id_of.erase(it);
    }
}
void set_tracked(uintptr_t address, uint32_t id) {
    if (auto heap = mirror_of(address)) heap->ids[(address - heap->start) / descriptor_size].store(id, std::memory_order_relaxed);
}
bool tracked_texture(uintptr_t address, ID3D12Resource*& resource, DXGI_FORMAT& format) {
    const auto heap = mirror_of(address);
    if (!heap) return false;
    const uint32_t id = heap->ids[(address - heap->start) / descriptor_size].load(std::memory_order_relaxed);
    if (!id || id >= texture_id_count.load(std::memory_order_acquire)) return false;
    format = texture_ids[id].format.load(std::memory_order_relaxed);
    resource = texture_ids[id].resource.load(std::memory_order_relaxed);
    return resource != nullptr;
}
void track_copy(uintptr_t dest, uintptr_t source, UINT count) {
    const SIZE_T span = SIZE_T(count) * descriptor_size;
    const auto to = mirror_of(dest, span);
    if (!to) return;
    auto* out = &to->ids[(dest - to->start) / descriptor_size];
    const auto from = mirror_of(source, span);
    if (!from) { for (UINT i = 0; i < count; ++i) out[i].store(0, std::memory_order_relaxed); return; } // unknown source: no screen
    const auto* in = &from->ids[(source - from->start) / descriptor_size];
    if (out == in) return;
    if (out < in || out >= in + count) for (UINT i = 0; i < count; ++i) out[i].store(in[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    else for (UINT i = count; i-- > 0;) out[i].store(in[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// Render target views: the texture each RTV descriptor points at (a draw's render target size and layers).
std::shared_mutex rtv_mutex;
std::unordered_map<SIZE_T, ID3D12Resource*> rtv_resources;
UINT rtv_size = 0; // RTV descriptor increment of the main device
ID3D12Resource* rtv_resource(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    std::shared_lock lock(rtv_mutex);
    const auto found = rtv_resources.find(handle.ptr);
    return found != rtv_resources.end() ? found->second : nullptr;
}
void copy_rtvs(SIZE_T dest, SIZE_T source, UINT count) {
    std::unique_lock lock(rtv_mutex);
    for (UINT i = 0; i < count; ++i) {
        const auto found = rtv_resources.find(source + SIZE_T(i) * rtv_size);
        rtv_resources[dest + SIZE_T(i) * rtv_size] = found != rtv_resources.end() ? found->second : nullptr;
    }
}

// Shader-visible CBV/SRV/UAV heaps by GPU address: where the descriptors of a bound descriptor table are.
struct GpuHeap { uint64_t gpu_end = 0; SIZE_T cpu_start = 0; };
std::shared_mutex gpu_heaps_mutex;
std::map<uint64_t, GpuHeap> gpu_heaps; // GPU start -> end, CPU start
void register_gpu_heap(ID3D12DescriptorHeap* heap, UINT count) {
    const uint64_t start = heap->GetGPUDescriptorHandleForHeapStart().ptr, end = start + uint64_t(count) * descriptor_size;
    std::unique_lock lock(gpu_heaps_mutex);
    std::erase_if(gpu_heaps, [&](const auto& h) { return h.first < end && h.second.gpu_end > start; }); // released heaps there
    gpu_heaps[start] = {end, heap->GetCPUDescriptorHandleForHeapStart().ptr};
}
// CPU address of the descriptor at `offset` in the descriptor table that starts at GPU handle `table`, or 0.
uintptr_t descriptor_address(uint64_t table, uint32_t offset) {
    std::shared_lock lock(gpu_heaps_mutex);
    auto it = gpu_heaps.upper_bound(table);
    if (it == gpu_heaps.begin()) return 0;
    --it;
    const uint64_t at = table + uint64_t(offset) * descriptor_size;
    if (at >= it->second.gpu_end) return 0;
    return uintptr_t(it->second.cpu_start + SIZE_T(at - it->first));
}

using CreateHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
CreateHeap original_create_heap = nullptr;
HRESULT STDMETHODCALLTYPE create_heap_hook(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** heap) {
    const HRESULT hr = original_create_heap(device, desc, riid, heap);
    if (SUCCEEDED(hr) && !shutting_down.load(std::memory_order_relaxed) && heap && *heap && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && device == main_device) {
        ComPtr<ID3D12DescriptorHeap> created;
        if (SUCCEEDED(static_cast<IUnknown*>(*heap)->QueryInterface(IID_PPV_ARGS(&created)))) {
            if (track_descriptors.load(std::memory_order_relaxed)) register_heap(created->GetCPUDescriptorHandleForHeapStart().ptr, desc->NumDescriptors);
            if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) register_gpu_heap(created.Get(), desc->NumDescriptors);
        }
    }
    return hr;
}
using CopySimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using Copy = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
    const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
CopySimple original_copy_simple = nullptr;
Copy original_copy = nullptr;
void STDMETHODCALLTYPE copy_simple_hook(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE source,
    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy_simple(device, count, dest, source, type);
    if (shutting_down.load(std::memory_order_relaxed)) return;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) { if (device == main_device) copy_rtvs(dest.ptr, source.ptr, count); return; }
    if (!track_descriptors.load(std::memory_order_relaxed)) return; // NVIDIA: screens are identified without tracking
    CpuScope scope(cpu_descriptors);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && device == main_device) track_copy(dest.ptr, source.ptr, count);
}
void STDMETHODCALLTYPE copy_hook(ID3D12Device* device, UINT dest_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts, const UINT* dest_sizes,
    UINT source_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts, const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy(device, dest_ranges, dest_starts, dest_sizes, source_ranges, source_starts, source_sizes, type);
    if (shutting_down.load(std::memory_order_relaxed)) return;
    const bool rtv = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!rtv && !track_descriptors.load(std::memory_order_relaxed)) return; // NVIDIA: screens are identified without tracking
    CpuScope scope(cpu_descriptors);
    if ((!rtv && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) || device != main_device || !dest_starts || !source_starts) return;
    const SIZE_T size = rtv ? rtv_size : descriptor_size;
    // Both sides are lists of ranges with the same total length; walk them together.
    UINT d = 0, s = 0, d_done = 0, s_done = 0;
    while (d < dest_ranges && s < source_ranges) {
        const UINT d_size = dest_sizes ? dest_sizes[d] : 1, s_size = source_sizes ? source_sizes[s] : 1;
        const UINT n = std::min(d_size - std::min(d_done, d_size), s_size - std::min(s_done, s_size));
        if (n && rtv) copy_rtvs(dest_starts[d].ptr + SIZE_T(d_done) * size, source_starts[s].ptr + SIZE_T(s_done) * size, n);
        else if (n) track_copy(dest_starts[d].ptr + SIZE_T(d_done) * size, source_starts[s].ptr + SIZE_T(s_done) * size, n);
        d_done += n; s_done += n;
        if (d_done >= d_size) { ++d; d_done = 0; }
        if (s_done >= s_size) { ++s; s_done = 0; }
    }
}

// Diagnostics: what each 2D shader resource view's descriptor points at, so an unrecognised screen texture can be
// described in the log (size, format, usage). Only the game's own descriptor bytes are read; no views are created.
struct ViewInfo { uint32_t width = 0, height = 0, format = 0, usage = 0; uint16_t layers = 0, samples = 0, levels = 0; };
std::mutex diag_mutex;
std::unordered_map<Signature, ViewInfo, SignatureHash> diag_views;

bool read_bytes(const void* source, void* destination, size_t size) {
    __try { std::memcpy(destination, source, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool rgba8(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return true;
    default: return false;
    }
}
// The typed format a typeless one is read as ("unorm" or "float"); typed formats stay as they are (sRGB too).
DXGI_FORMAT typed(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
    }
}
bool typeless(DXGI_FORMAT format) { return typed(format) != format; }

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
    std::pair<UINT, UINT> mips{0, UINT(-1)};
    bool known = false;
    { std::lock_guard view_lock(view_mips_mutex); if (const auto found = view_mips.find(resource); found != view_mips.end()) { mips = found->second; known = true; } }
    slot_first_mip[chosen] = mips.first; slot_mip_count[chosen] = mips.second;
    slot_resources[chosen] = resource;
    bounds_reset.fetch_or(1u << chosen);
    const auto d = resource->GetDesc();
    log("display_slot slot=%d resource=%p w=%llu h=%u mips=%u format=%u game_view_mips=%u+%d%s", chosen, resource, d.Width, d.Height, d.MipLevels, unsigned(format),
        mips.first, int(mips.second), known ? "" : " (view not seen)");
    return chosen;
}

// ---------------------------------------------------------------------------------------------------------------
struct Map {
    UINT width = 0, height = 0, layers = 1; // one layer per view (VR: 2 eyes, or 4 views with foveated rendering)
    ComPtr<ID3D12Resource> texture, tiles, residual, glass_history, glass_history_uv; // glass_history(_uv): per render pixel and view
    ComPtr<ID3D12DescriptorHeap> rtv;
    bool initialised = false;
    std::atomic<uint32_t> views{0}; // layers drawn this frame and not yet composited (upscaler calls come in view order)
    std::atomic<uint64_t> recorded_present{0}; // present count when screens were last recorded into the map
    // Diagnostics (VR): upscaler calls since the last present, to tell each call's view apart without the views count.
    std::atomic<uint64_t> call_present{UINT64_MAX};
    std::atomic<uint32_t> calls_this_present{0};
    // Largest upscaler output seen for this render size, and when: another mod (e.g. RenoDX's DLSS add-on) can run an
    // extra DLSS pass at the same render size before the sim's own upscale; the fix belongs on the real (largest) one.
    std::atomic<uint64_t> largest_output{0}, largest_output_present{0};
    // Temporal anti-aliasing history (output size, two slices per view), made on first use. Under composite_mutex.
    ComPtr<ID3D12Resource> history, history_uv;
    ComPtr<ID3D12Resource> fit_input; // screen capture: what the brightness match read per render pixel (made on first capture)
    std::vector<ComPtr<ID3D12Resource>> retired; // replaced history the GPU may still be reading
    UINT history_w = 0, history_h = 0;
    std::array<uint32_t, 8> history_parity{}, history_frames{};
    std::array<uint64_t, 8> history_present{}; // present count + 1 of each view's last history write (0 = none)
    // With DLSS Frame Generation the sim runs the upscaler up to three times per frame into the same output, each run
    // replacing the picture (and the fix with it). The map of the last fixed call is kept, and a repeated call for the
    // same frame (same output, same camera jitter: a new frame always has another jitter) gets the fix again from it.
    // Under composite_mutex; flat (one view) only.
    ComPtr<ID3D12Resource> repeat_map;
    const void* repeat_output = nullptr;
    float repeat_jitter_x = 0, repeat_jitter_y = 0;
    bool repeat_valid = false;
};

struct __declspec(uuid("2B8D0E51-7C1A-4F43-9A07-8E4C51D2F6A3")) DeviceState {
    ID3D12Device* native = nullptr;
    // Screen capture: each screen texture's colour at the moment the sim drew it (render size, kept as a render target).
    std::mutex draw_colour_mutex;
    ComPtr<ID3D12Resource> draw_colour;
    std::vector<ComPtr<ID3D12Resource>> draw_colour_retired;
    ComPtr<ID3D12DescriptorHeap> draw_colour_rtv;
    // Pixel probe: one occlusion query per probed draw, resolved into a readback buffer.
    ComPtr<ID3D12QueryHeap> probe_queries;
    ComPtr<ID3D12Resource> probe_readback;
    std::mutex maps_mutex;
    std::vector<std::unique_ptr<Map>> maps;
    std::shared_mutex layouts_mutex;
    std::unordered_map<uint64_t, std::array<std::pair<uint32_t, uint32_t>, 2>> display_bindings; // layout -> root index, table offset of t0 and t1
    struct SrvRange { uint32_t param, offset, reg, space, count; };
    std::unordered_map<uint64_t, std::vector<SrvRange>> srv_ranges; // layout -> pixel shader texture ranges (screen probe)
    std::mutex composite_mutex;
    bool composite_ready = false, composite_failed = false, accumulators_zeroed = false, output_readable = false;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 6> passes; // zero, fit, solve, composite, clear, glass_filter
    ComPtr<ID3D12DescriptorHeap> heap, scratch;
    ComPtr<ID3D12Resource> sums, params, edge_bounds;
    ComPtr<ID3D12Resource> params_readback; // brightness match per screen, copied out now and then for the log
    // Diagnostics: per upscaler call, screen pixels in the map on the GPU (64-call ring after the edge bounds), copied out
    // now and then with what the CPU knew at each call.
    ComPtr<ID3D12Resource> diag_readback;
    struct DiagCall { uint64_t present = 0, recorded = 0; uint32_t pending = 0, views = 0; const void* output = nullptr; uint32_t thread = 0; };
    std::array<DiagCall, 64> diag_calls{}, diag_snapshot{};
    uint32_t diag_index = 0, diag_copied_at = 0;
    uint64_t diag_ready = UINT64_MAX, diag_next = 300;
    int diag_dumps = 0;
    std::atomic<uint64_t> readback_next{600}, readback_ready{UINT64_MAX};
    uint64_t frame = 0;
    ComPtr<ID3D12QueryHeap> timestamps; // cost diagnostics: start and end of the passes of each upscaler call
    ComPtr<ID3D12Resource> timestamps_readback;
    UINT timed_calls = 0; // under composite_mutex
};

struct __declspec(uuid("9E1F3C77-5A2B-4D8E-B0C4-71D6A2E9F015")) ListState {
    ID3D12GraphicsCommandList* native = nullptr;
    DeviceState* device = nullptr; // null: a list of another device (e.g. the sim's software device), not followed
    DisplayPipeline* display = nullptr;
    uint64_t pso = 0, layout = 0;
    std::array<uint64_t, 64> tables{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    UINT target_count = 0;
    bool in_pass = false;
    uint32_t generation = 0;
    D3D12_RECT scissor{}; // the game's first scissor rectangle (pixel probe restores it)
    uint64_t pass_rt0 = 0; UINT pass_targets = 0; // inside a render pass: its first render target (pixel probe)
    bool scissor_known = false;
};

Map* map_for(DeviceState& device, UINT width, UINT height, UINT layers) {
    std::lock_guard lock(device.maps_mutex);
    for (auto& map : device.maps) if (map->width == width && map->height == height && map->layers == layers) return map.get();
    if (device.maps.size() >= 6 || !width || !height || width > 8192 || height > 8192 || !layers || layers > 8) return nullptr;
    auto map = std::make_unique<Map>();
    map->width = width; map->height = height; map->layers = layers;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, UINT16(layers), 1, glass::map_format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_CLEAR_VALUE clear{glass::map_format, {}};
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&map->texture)))) return nullptr;
    desc.Width = (width + 7) / 8; desc.Height = (height + 7) / 8; desc.DepthOrArraySize = 1; desc.Format = DXGI_FORMAT_R32_UINT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->tiles)))) return nullptr;
    desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // per render pixel
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->residual)))) return nullptr;
    desc.DepthOrArraySize = UINT16(layers);
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->glass_history)))) return nullptr;
    desc.Format = DXGI_FORMAT_R16G16_UNORM;
    if (FAILED(device.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->glass_history_uv)))) return nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device.native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&map->rtv)))) return nullptr;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{glass::map_format, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
    rtv.Texture2DArray = {0, 0, layers, 0};
    device.native->CreateRenderTargetView(map->texture.Get(), &rtv, map->rtv->GetCPUDescriptorHandleForHeapStart());
    log("map_created w=%u h=%u layers=%u", width, height, layers);
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
void install_ngx_proxies();
void install_ffx();

// ---------------------------------------------------------------------------------------------------------------
// D3D12 hooks. What the sim records (pipelines, root signatures, views, bindings, render targets, draws) is followed
// through hooks on the D3D12 runtime's own functions, installed when the sim creates its device. Every caller goes
// through them, other mods' wrappers included: under ReShade (the add-on build, or ReShade installed for its effects)
// they see what ReShade's wrappers pass on to the runtime. They took the place of ReShade's add-on events, which earlier
// versions of the add-on used; the handlers below behave as those did.
void install_d3d12_hooks(ID3D12Device* device);
void install_dxgi_hooks(IUnknown* factory);
void install_upscaler_hooks();

// The fix's own D3D12 calls (the map draw, its views, the compute passes, captures) go through the same hooked
// functions: they pass straight through while this is set on the calling thread.
thread_local int internal_calls = 0;
bool passthrough() { return internal_calls || shutting_down.load(std::memory_order_relaxed); }
struct Internal {
    Internal() { ++internal_calls; }
    ~Internal() { --internal_calls; }
    Internal(const Internal&) = delete;
    Internal& operator=(const Internal&) = delete;
};

std::mutex device_mutex;
bool same_adapter(ID3D12Device* a, ID3D12Device* b) {
    const LUID x = a->GetAdapterLuid(), y = b->GetAdapterLuid();
    return x.LowPart == y.LowPart && x.HighPart == y.HighPart;
}
void device_created(ID3D12Device* native) noexcept try {
    std::lock_guard lock(device_mutex);
    if (native == main_device) return; // the same device again: D3D12 keeps one device per adapter
    // A device on another GPU after the sim's (e.g. a helper on a laptop's integrated GPU) is not the sim's.
    if (main_device && !same_adapter(native, main_device)) {
        LOG_ONCE("note a Direct3D 12 device on another GPU was created after the sim's; the fix stays with the sim's");
        return;
    }
    // Only the game's hardware device: MSFS also creates a WARP (software) device for a helper window,
    // whose descriptors have a different size and must not be mixed with the GPU's.
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
            std::lock_guard info_lock(info_mutex);
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
    // Kept for the session, like the device itself (the fix's own objects on it keep it alive).
    auto state = new DeviceState;
    state->native = native;
    descriptor_size = size;
    rtv_size = native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    {
        Internal internal;
        D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&state->scratch));
    }
    track_descriptors = !gpu_nvidia || force_tracking;
    if (track_descriptors && !texture_ids) texture_ids = std::make_unique<TextureId[]>(max_texture_ids);
    main_state = state;
    main_device = native;
    install_d3d12_hooks(native);
    if (track_descriptors) log("descriptor tracking on (screen textures are followed through descriptor copies)");
    log("device native=%p descriptor_size=%u", native, descriptor_size);
    install_upscaler_hooks();
} catch (...) { log("device_exception"); }

// Root signatures: where each one has the display shader's screen texture (t0, t1) and the pixel shader's other
// texture ranges (screen probe). Version 1.0 and 1.1+ root signatures differ only in their range and parameter types.
template <class Parameter> void layout_created(DeviceState& state, uint64_t layout, UINT count, const Parameter* params) {
    std::vector<DeviceState::SrvRange> found;
    // Where t0 and t1 (pixel shader, space 0) sit: the display shader reads its screen from t1, the variant from t0.
    std::array<std::pair<uint32_t, uint32_t>, 2> bindings{};
    std::array<bool, 2> have{};
    for (UINT i = 0; i < count && i < 64; ++i) {
        const auto& p = params[i];
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
            (p.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL && p.ShaderVisibility != D3D12_SHADER_VISIBILITY_PIXEL)) continue;
        uint32_t next = 0; // where a range declared with D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND starts
        for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
            const auto& r = p.DescriptorTable.pDescriptorRanges[j];
            const uint32_t offset = r.OffsetInDescriptorsFromTableStart == UINT(D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) ? next : r.OffsetInDescriptorsFromTableStart;
            if (r.NumDescriptors != UINT_MAX) next = offset + r.NumDescriptors; // only the last range of a table may be unbounded
            if (r.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV) continue;
            found.push_back({i, offset, r.BaseShaderRegister, r.RegisterSpace, std::min<uint32_t>(r.NumDescriptors, 16)});
            if (r.RegisterSpace) continue;
            for (uint32_t reg = 0; reg < 2; ++reg)
                if (!have[reg] && r.BaseShaderRegister <= reg && uint64_t(r.BaseShaderRegister) + r.NumDescriptors > reg) {
                    bindings[reg] = {i, offset + reg - r.BaseShaderRegister};
                    have[reg] = true;
                }
        }
    }
    if (!found.empty()) { std::unique_lock lock(state.layouts_mutex); state.srv_ranges[layout] = std::move(found); }
    if (have[1]) { // every cockpit display layout has t1; t0 is checked when used
        std::unique_lock lock(state.layouts_mutex);
        state.display_bindings[layout] = have[0] ? bindings : std::array<std::pair<uint32_t, uint32_t>, 2>{{{UINT32_MAX, 0}, bindings[1]}};
    }
}
void root_signature_created(const void* blob, SIZE_T size, ID3D12RootSignature* root) noexcept try {
    auto state = main_state;
    if (!state) return;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&reader)))) desc = reader->GetUnconvertedRootSignatureDesc();
    if (!desc) {
        LOG_ONCE("note a root signature could not be read (its draws are not inspected)");
        return;
    }
    const auto layout = reinterpret_cast<uint64_t>(root);
    // Versions 1.1 and 1.2 have the same parameters (1.2 only adds to static samplers).
    if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) layout_created(*state, layout, desc->Desc_1_0.NumParameters, desc->Desc_1_0.pParameters);
    else layout_created(*state, layout, desc->Desc_1_1.NumParameters, desc->Desc_1_1.pParameters);
} catch (...) {}

// Destruction of screen texture candidates: a small COM object is attached to each and released by the runtime when the
// texture is destroyed.
void texture_destroyed(ID3D12Resource* native) {
    { std::shared_lock lock(textures_mutex); if (!known_textures.contains(native)) return; }
    std::unique_lock lock(textures_mutex);
    known_textures.erase(native);
    { std::lock_guard view_lock(view_mips_mutex); view_mips.erase(native); }
    std::erase_if(signatures, [&](const auto& p) { return p.second.resource == native; });
    if (track_descriptors.load(std::memory_order_relaxed)) forget_texture_ids(native);
    for (auto& slot : slot_resources) { auto expected = native; slot.compare_exchange_strong(expected, nullptr); }
}
struct __declspec(uuid("5C0E6A2D-3B1F-4E8A-9D47-0F2B8C61A3E9")) ReleaseNotifier final : IUnknown {
    ID3D12Resource* resource;
    bool attached = true;
    std::atomic<ULONG> references{1};
    explicit ReleaseNotifier(ID3D12Resource* r) : resource(r) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown)) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references;
        if (!left) {
            if (attached && !shutting_down) texture_destroyed(resource);
            delete this;
        }
        return left;
    }
};
void notify_on_destroy(ID3D12Resource* native) {
    auto notifier = new ReleaseNotifier(native);
    if (FAILED(native->SetPrivateDataInterface(__uuidof(ReleaseNotifier), notifier))) notifier->attached = false;
    notifier->Release(); // the texture holds the only reference now
}

// Shader resource views of the sim's textures: which ones are screen texture candidates, and their descriptor bytes.
void view_created(ID3D12Resource* native, const D3D12_SHADER_RESOURCE_VIEW_DESC* view, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept try {
    CpuScope scope(cpu_views);
    if (track_descriptors.load(std::memory_order_relaxed) && handle.ptr) set_tracked(uintptr_t(handle.ptr), 0); // a new view replaces what was there
    // A default view (no view description) shows the texture as it is.
    if (!native || (view && view->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D && view->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY)) return;
    auto state = main_state;
    if (!state || !state->scratch || descriptor_size > 32) return;
    const auto r = native->GetDesc();
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && handle.ptr) {
        Signature d{};
        if (read_bytes(reinterpret_cast<const void*>(handle.ptr), d.bytes.data(), descriptor_size)) {
            std::lock_guard lock(diag_mutex);
            if (diag_views.size() >= 262144) diag_views.clear();
            diag_views[d] = {uint32_t(r.Width), r.Height, uint32_t(r.Format), uint32_t(r.Flags), r.DepthOrArraySize, uint16_t(r.SampleDesc.Count), r.MipLevels};
        }
    }
    // Cockpit screens are plain single-sample 2D textures.
    if (r.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || r.SampleDesc.Count != 1 || r.DepthOrArraySize != 1 || r.Width < 64 || r.Height < 64) return;
    // Only RGBA8 colour textures the GPU writes (render target or UAV), the kind cockpit screens are. Recreating views
    // of other textures (depth/stencil planes, video, streamed material textures) at sim startup removed the device.
    const bool written = (r.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0;
    if (!written || !rgba8(r.Format) || (r.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return;
    // Recreate exactly the view the game made (a null desc stays null), so the call is valid by construction.
    const bool default_view = view == nullptr;
    const auto format = default_view ? r.Format : view->Format;
    if (format == DXGI_FORMAT_UNKNOWN || typeless(format)) return;
    // Any size: Fenix PFD 768x768; the A220 draws its main screens side by side into one 7410x1110 texture.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    if (!default_view) {
        const bool array = view->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        const uint32_t first = array ? view->Texture2DArray.MostDetailedMip : view->Texture2D.MostDetailedMip;
        const uint32_t count = array ? view->Texture2DArray.MipLevels : view->Texture2D.MipLevels;
        if (first >= r.MipLevels || (count != UINT32_MAX && (count == 0 || uint64_t(first) + count > r.MipLevels))) return;
        srv.Texture2D.MostDetailedMip = first;
        srv.Texture2D.MipLevels = count; // UINT(-1): all from the first
    }
    {
        // The game's mip range for this texture; a second, different view of it is logged (the first one is used).
        const std::pair<UINT, UINT> mips = default_view ? std::pair<UINT, UINT>{0, UINT(-1)}
            : std::pair<UINT, UINT>{srv.Texture2D.MostDetailedMip, srv.Texture2D.MipLevels};
        std::lock_guard view_lock(view_mips_mutex);
        const auto [it, added] = view_mips.try_emplace(native, mips);
        static std::atomic<int> logged{0};
        if (!added && it->second != mips && logged++ < 16)
            log("note screen texture %p has views with different mips: %u+%d and %u+%d (the first is used)", native, it->second.first, int(it->second.second), mips.first, int(mips.second));
    }
    if (track_descriptors.load(std::memory_order_relaxed) && handle.ptr) set_tracked(uintptr_t(handle.ptr), texture_id(native, format));
    const auto slot = state->scratch->GetCPUDescriptorHandleForHeapStart();
    Signature s{};
    bool readable = false;
    {
        // One scratch descriptor: its own lock, so that the texture tables' lock (also taken when a texture is destroyed,
        // inside the runtime) is never held across a D3D12 call.
        static std::mutex scratch_mutex;
        std::lock_guard scratch_lock(scratch_mutex);
        Internal internal;
        state->native->CreateShaderResourceView(native, default_view ? nullptr : &srv, slot);
        readable = read_bytes(reinterpret_cast<const void*>(slot.ptr), s.bytes.data(), descriptor_size);
    }
    bool first_view = false;
    {
        std::unique_lock lock(textures_mutex);
        if (readable && signatures.size() < 16384) {
            signatures[s] = {native, format};
            static std::atomic<int> logged{0};
            if (logged++ < 40) log("texture_candidate resource=%p w=%llu h=%u mips=%u format=%u default_view=%d", native, r.Width, r.Height,
                unsigned(r.MipLevels), unsigned(format), default_view);
        }
        first_view = known_textures.insert(native).second;
    }
    if (first_view) notify_on_destroy(native);
} catch (...) {}

// Command lists: the state each one has bound (what ReShade kept per list). Looked up on every hooked call, so first a
// thread-local cache of the last list, then a lock-free open-addressing table. Entries are never removed: a list
// created later at the same address gets the same entry, and its creation resets the state.
constexpr size_t list_table_size = size_t(1) << 16;
struct ListEntry { std::atomic<ID3D12GraphicsCommandList*> list{nullptr}; std::atomic<ListState*> state{nullptr}; };
std::unique_ptr<ListEntry[]> list_table = std::make_unique<ListEntry[]>(list_table_size);
std::mutex list_table_mutex;
size_t list_table_count = 0;
size_t list_slot(const void* list) { return size_t((uint64_t(reinterpret_cast<uintptr_t>(list)) >> 4) * 0x9E3779B97F4A7C15ull >> 48) & (list_table_size - 1); }
ListState* find_list(ID3D12GraphicsCommandList* list) {
    for (size_t i = list_slot(list), n = 0; n < list_table_size; i = (i + 1) & (list_table_size - 1), ++n) {
        const auto key = list_table[i].list.load(std::memory_order_acquire);
        if (key == list) return list_table[i].state.load(std::memory_order_acquire);
        if (!key) return nullptr;
    }
    return nullptr;
}
ListState* add_list(ID3D12GraphicsCommandList* list) {
    std::lock_guard lock(list_table_mutex);
    if (auto state = find_list(list)) return state;
    if (list_table_count >= list_table_size * 3 / 4) { LOG_ONCE("WARNING command list table full: lists created from now on are not followed"); return nullptr; }
    auto state = new ListState;
    state->native = list;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))) && device.Get() == main_device) state->device = main_state;
    state->generation = generation;
    // The upscaler hooks find a list's state through this, also when another mod hands them its own wrapper of the list.
    list->SetPrivateData(__uuidof(ListState), sizeof(state), &state);
    size_t i = list_slot(list);
    while (list_table[i].list.load(std::memory_order_relaxed)) i = (i + 1) & (list_table_size - 1);
    list_table[i].state.store(state, std::memory_order_release);
    list_table[i].list.store(list, std::memory_order_release);
    ++list_table_count;
    return state;
}
// A list's entry, whichever device it belongs to (null only when the table is full).
ListState* list_entry(ID3D12GraphicsCommandList* list) {
    thread_local ID3D12GraphicsCommandList* cached_list = nullptr;
    thread_local ListState* cached_state = nullptr;
    if (list != cached_list) {
        auto state = find_list(list);
        if (!state) state = add_list(list);
        if (!state) return nullptr;
        cached_list = list; cached_state = state;
    }
    return cached_state;
}
// The state of a list of the sim's device, or null.
ListState* list_state(ID3D12GraphicsCommandList* list) {
    auto state = list_entry(list);
    return state && state->device ? state : nullptr;
}

void bind_pipeline(ListState& state, ID3D12PipelineState* pipeline) {
    state.pso = reinterpret_cast<uint64_t>(pipeline); state.display = find_pipeline(state.pso);
}
void restart(ListState& state, DeviceState* device, ID3D12PipelineState* initial) {
    const auto list = state.native;
    state = ListState{}; state.native = list; state.device = device; state.generation = generation;
    if (device && initial && enabled.load(std::memory_order_relaxed)) bind_pipeline(state, initial);
}
// A new list (possibly at the address of a destroyed one): nothing bound yet, and which device it belongs to.
void list_created(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12PipelineState* initial) {
    CpuScope scope(cpu_passes);
    auto state = list_entry(list);
    if (!state) return;
    restart(*state, device == main_device ? main_state : nullptr, initial);
    list->SetPrivateData(__uuidof(ListState), sizeof(state), &state); // a new list object at an old list's address has none yet
}
// A list reset to record again: nothing bound.
void list_reset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* initial) {
    CpuScope scope(cpu_passes);
    if (auto state = list_state(list)) restart(*state, state->device, initial);
}
void pipeline_bound(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    CpuScope scope(cpu_binds);
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (auto state = list_state(list)) bind_pipeline(*state, pipeline);
}
void root_signature_bound(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    CpuScope scope(cpu_binds);
    if (!enabled.load(std::memory_order_relaxed)) return;
    auto state = list_state(list);
    if (!state) return;
    const auto layout = reinterpret_cast<uint64_t>(root);
    if (state->layout != layout) { state->layout = layout; state->tables = {}; } // a new root signature unbinds all tables
}
void table_bound(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    CpuScope scope(cpu_binds);
    if (!enabled.load(std::memory_order_relaxed)) return;
    auto state = list_state(list);
    if (state && index < state->tables.size()) state->tables[index] = table.ptr;
}
void targets_bound(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    CpuScope scope(cpu_targets);
    if (!enabled.load(std::memory_order_relaxed)) return;
    auto state = list_state(list);
    if (!state) return;
    state->target_count = count <= 8 && targets ? count : 0;
    for (UINT i = 0; i < state->target_count; ++i)
        state->targets[i].ptr = single_range ? targets[0].ptr + SIZE_T(i) * rtv_size : targets[i].ptr; // consecutive descriptors from the first
    state->depth.ptr = depth ? depth->ptr : 0;
}
void scissors_bound(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RECT* rects) {
    if (!enabled.load(std::memory_order_relaxed) || !count || !rects) return;
    if (auto state = list_state(list)) { state->scissor = rects[0]; state->scissor_known = true; }
}
void pass_begun(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets) {
    CpuScope scope(cpu_passes);
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (auto state = list_state(list)) { state->in_pass = true; state->pass_targets = count; state->pass_rt0 = count && targets ? targets[0].cpuDescriptor.ptr : 0; }
}
void pass_ended(ID3D12GraphicsCommandList* list) {
    CpuScope scope(cpu_passes);
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (auto state = list_state(list)) { state->in_pass = false; state->target_count = 0; state->depth = {}; state->pass_targets = 0; state->pass_rt0 = 0; }
}

extern std::atomic<int> capture_requested; // screen capture (below)
// Returns true when it issued the game's draw itself (the original call is then skipped).
bool replay(ListState& state, const glass::Draw& draw) noexcept try {
    Internal internal; // the draws below go through the hooked functions
    auto& device = *state.device;
    if (state.in_pass || !state.target_count || !state.depth.ptr || !draw.count || !draw.instances) { note(stats.unsupported_draw, "render_pass_or_targets"); return false; }
    std::pair<uint32_t, uint32_t> binding;
    {
        std::shared_lock lock(device.layouts_mutex);
        const auto found = device.display_bindings.find(state.layout);
        if (found == device.display_bindings.end() || state.layout != reinterpret_cast<uint64_t>(state.display->root.Get())) { note(stats.unsupported_draw, "root_layout"); return false; }
        binding = found->second[state.display->screen_t0 ? 0 : 1];
    }
    if (binding.first >= state.tables.size()) { note(stats.unsupported_draw, "root_layout"); return false; }
    const auto table = state.tables[binding.first];
    if (!table) { note(stats.unsupported_draw, "t1_table_unbound"); return false; }
    const auto address = descriptor_address(table, binding.second);
    if (!address) { note(stats.unsupported_draw, "descriptor_heap_unknown"); return false; }
    Signature s{};
    if (!read_bytes(reinterpret_cast<const void*>(address), s.bytes.data(), descriptor_size)) {
        note(stats.unknown_texture, "descriptor_unreadable"); return false;
    }
    Texture texture;
    bool recognised = false;
    if (!force_tracking) {
        std::shared_lock lock(textures_mutex);
        if (const auto found = signatures.find(s); found != signatures.end()) { texture = found->second; recognised = true; }
    }
    if (!recognised && track_descriptors.load(std::memory_order_relaxed)) {
        recognised = tracked_texture(address, texture.resource, texture.format);
        if (recognised) LOG_ONCE("milestone screen texture found through descriptor tracking");
    }
    if (!recognised) {
        static std::mutex logged_mutex;
        static std::unordered_set<Signature, SignatureHash> logged; // each unrecognised texture once
        bool first;
        { std::lock_guard lock(logged_mutex); first = logged.size() < 24 && logged.insert(s).second; }
        if (first) {
            size_t tracked_count;
            tracked_count = texture_id_count.load() - 1;
            ViewInfo info{};
            bool described = false;
            { std::lock_guard lock(diag_mutex); if (const auto d = diag_views.find(s); d != diag_views.end()) { info = d->second; described = true; } }
            log("unrecognised_descriptor address=%p bytes=%s known=%llu tracked=%llu texture=%s w=%u h=%u format=%u flags=0x%x layers=%u samples=%u mips=%u",
                reinterpret_cast<void*>(address), hex(s).c_str(), uint64_t(signatures.size()), uint64_t(tracked_count), described ? "described" : "unknown",
                info.width, info.height, info.format, info.usage, info.layers, info.samples, info.levels);
        }
        note(stats.unknown_texture, "texture_not_recognised"); return false;
    }
    const int slot = slot_of(texture.resource, texture.format);
    if (slot < 0) { note(stats.unsupported_draw, "slots_full"); return false; }
    {
        // Diagnostics: each distinct draw of a screen texture once (several panels can share one texture, e.g. the iFly
        // 737's CDU and radios: are they separate draws?).
        static std::mutex draws_mutex;
        static std::set<std::tuple<ID3D12Resource*, uint32_t, uint32_t, int32_t, uint32_t>> draws;
        std::lock_guard lock(draws_mutex);
        if (draws.size() < 400 && draws.emplace(texture.resource, draw.count, draw.first, draw.base_vertex, draw.instances).second)
            log("screen_draw slot=%d texture=%p %s count=%u first=%u base_vertex=%d instances=%u", slot, texture.resource, draw.indexed ? "indexed" : "plain",
                draw.count, draw.first, draw.base_vertex, draw.instances);
    }
    auto pso = variant(state.display, unsigned(slot));
    if (!pso) return false;
    const auto target = rtv_resource(state.targets[0]);
    if (!target) { note(stats.unsupported_draw, "target_unknown"); return false; }
    const auto desc = target->GetDesc();
    // VR draws all views at once into a layered target; the map gets the same layers.
    const UINT layers = state.display->vr ? std::clamp<UINT>(desc.DepthOrArraySize, 1, 8) : 1;
    auto map = map_for(device, UINT(desc.Width), desc.Height, layers);
    if (!map) { note(stats.unsupported_draw, "map_allocation"); return false; }
    auto native = state.native;
    const auto rtv = map->rtv->GetCPUDescriptorHandleForHeapStart();
    {
        std::lock_guard lock(device.maps_mutex);
        if (!map->initialised) {
            constexpr float zero[4]{};
            native->ClearRenderTargetView(rtv, zero, 0, nullptr);
            map->initialised = true;
        } else transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // The game's draw first (its state is still bound), then the map against the depth it wrote (see clone_pipeline).
    if (draw.indexed) native->DrawIndexedInstanced(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
    else native->DrawInstanced(draw.count, draw.instances, draw.first, draw.first_instance);
    // Viewport, scissor, root arguments and mesh buffers stay exactly as the game bound them.
    native->SetPipelineState(pso);
    native->OMSetRenderTargets(1, &rtv, FALSE, &state.depth);
    if (draw.indexed) native->DrawIndexedInstanced(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
    else native->DrawInstanced(draw.count, draw.instances, draw.first, draw.first_instance);
    // Screen capture: the screen texture's colour as the sim draws it (it can differ from what the fix reads later).
    // Only at the upscaler's render size: the sim also draws the screens into smaller targets (reflections).
    if (capture_requested.load(std::memory_order_relaxed) > 0 && layers == 1 && desc.Width == last_render_w.load() && desc.Height == last_render_h.load()) {
        if (auto colour = colour_variant(state.display)) {
            D3D12_CPU_DESCRIPTOR_HANDLE colour_rtv{};
            {
                std::lock_guard lock(device.draw_colour_mutex);
                // Another render size: the old target is kept (the GPU may still be drawing into it), a new one made.
                if (device.draw_colour && (device.draw_colour->GetDesc().Width != desc.Width || device.draw_colour->GetDesc().Height != desc.Height)) {
                    device.draw_colour_retired.push_back(device.draw_colour);
                    device.draw_colour = nullptr;
                }
                if (!device.draw_colour && device.draw_colour_retired.size() < 4) {
                    D3D12_HEAP_PROPERTIES colour_heap{D3D12_HEAP_TYPE_DEFAULT};
                    D3D12_RESOURCE_DESC colour_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, desc.Width, desc.Height, 1, 1, glass::map_format, {1, 0},
                        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
                    D3D12_CLEAR_VALUE clear{glass::map_format, {}};
                    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
                    if (FAILED(device.native->CreateCommittedResource(&colour_heap, D3D12_HEAP_FLAG_NONE, &colour_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&device.draw_colour))) ||
                        (!device.draw_colour_rtv && FAILED(device.native->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&device.draw_colour_rtv))))) device.draw_colour = nullptr;
                    else {
                        device.native->CreateRenderTargetView(device.draw_colour.Get(), nullptr, device.draw_colour_rtv->GetCPUDescriptorHandleForHeapStart());
                        constexpr float zero[4]{};
                        native->ClearRenderTargetView(device.draw_colour_rtv->GetCPUDescriptorHandleForHeapStart(), zero, 0, nullptr);
                    }
                }
                if (device.draw_colour) colour_rtv = device.draw_colour_rtv->GetCPUDescriptorHandleForHeapStart();
            }
            if (colour_rtv.ptr) {
                native->SetPipelineState(colour);
                native->OMSetRenderTargets(1, &colour_rtv, FALSE, &state.depth);
                if (draw.indexed) native->DrawIndexedInstanced(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
                else native->DrawInstanced(draw.count, draw.instances, draw.first, draw.first_instance);
            }
        }
    }
    native->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pso));
    native->OMSetRenderTargets(state.target_count, state.targets.data(), FALSE, &state.depth);
    transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, map_resting);
    map->views = map->layers;
    map->recorded_present = present_count.load(std::memory_order_relaxed);
    if (stats.replays++ == 0) log("milestone first cockpit screen recorded (render target %ux%u)", UINT(desc.Width), desc.Height);
    if (layers > 1) LOG_ONCE("milestone first VR cockpit screen recorded (render target %ux%u, %u views)", UINT(desc.Width), desc.Height, layers);
    return true;
} catch (...) { note(stats.unsupported_draw, "exception"); return false; }

// VR probe window (in presents), started when the upscaler runs at a size with no cockpit map (VR or another view).
std::atomic<uint64_t> probe_until{0};
// The first two render targets of a draw, for the probes' log lines.
std::string describe_targets(const ListState& state) {
    char targets[160]{};
    for (uint32_t i = 0; i < state.target_count && i < 2; ++i) {
        const auto rt = rtv_resource(state.targets[i]);
        if (!rt) continue;
        const auto r = rt->GetDesc();
        snprintf(targets + strlen(targets), sizeof(targets) - strlen(targets), " rt%u=%llux%u layers=%u format=%u", i, uint64_t(r.Width), r.Height,
            unsigned(r.DepthOrArraySize), unsigned(r.Format));
    }
    return targets;
}
void probe_screen_draw(const ListState& state, uint32_t count, uint32_t instances) noexcept try {
    if (!state.device || !state.layout) return;
    std::pair<uint32_t, uint32_t> binding;
    {
        std::shared_lock lock(state.device->layouts_mutex);
        const auto found = state.device->display_bindings.find(state.layout);
        if (found == state.device->display_bindings.end()) return;
        binding = found->second[1];
    }
    const auto table = state.tables[binding.first];
    if (!table) return;
    const auto address = descriptor_address(table, binding.second);
    if (!address) return;
    Signature s{};
    if (!read_bytes(reinterpret_cast<const void*>(address), s.bytes.data(), descriptor_size)) return;
    ID3D12Resource* resource = nullptr;
    {
        std::shared_lock lock(textures_mutex);
        const auto found = signatures.find(s);
        if (found == signatures.end()) return;
        resource = found->second.resource;
    }
    bool screen = false;
    for (const auto& slot : slot_resources) screen |= slot.load() == resource;
    if (!screen) return;
    static std::mutex logged_mutex;
    static std::set<uint64_t> logged;
    {
        std::lock_guard lock(logged_mutex);
        if (logged.size() >= 24 || !logged.insert(state.pso).second) return;
    }
    std::pair<uint64_t, uint64_t> hashes{};
    {
        std::shared_lock lock(hashes_mutex);
        if (const auto found = pipeline_hashes.find(state.pso); found != pipeline_hashes.end()) hashes = found->second;
    }
    log("probe_screen_draw pso=%p vs=%016llx ps=%016llx screen=%p count=%u instances=%u in_pass=%d targets=%u%s", reinterpret_cast<void*>(state.pso),
        hashes.first, hashes.second, resource, count, instances, state.in_pass, state.target_count, describe_targets(state).c_str());
} catch (...) {}

// Screen probe (button in the Bug reports section, 600 frames; costs a lot of CPU while it runs): draws with any other pipeline whose pixel shader reads a gauge-like texture
// (GPU-written RGBA8 with mip maps, like every cockpit screen texture), to find screens drawn with another shader. Every
// pass, not just the 3D scene's (the iFly 737 draws its MCP, CDU and radio digits over their screens in a later one).
std::atomic<uint64_t> screen_probe_until{0};
void screen_probe(const ListState& state, uint32_t count, uint32_t instances) noexcept try {
    if (!state.device || !state.layout || !state.target_count) return;
    std::vector<DeviceState::SrvRange> ranges;
    {
        std::shared_lock lock(state.device->layouts_mutex);
        const auto found = state.device->srv_ranges.find(state.layout);
        if (found == state.device->srv_ranges.end()) return;
        ranges = found->second;
    }
    for (const auto& r : ranges) {
        const auto table = r.param < state.tables.size() ? state.tables[r.param] : 0;
        if (!table) continue;
        for (uint32_t k = 0; k < r.count; ++k) {
            const auto address = descriptor_address(table, r.offset + k);
            if (!address) continue;
            Signature sig{};
            if (!read_bytes(reinterpret_cast<const void*>(address), sig.bytes.data(), descriptor_size)) continue;
            ID3D12Resource* resource = nullptr;
            {
                std::shared_lock lock(textures_mutex);
                if (const auto found = signatures.find(sig); found != signatures.end()) resource = found->second.resource;
            }
            if (!resource && track_descriptors.load(std::memory_order_relaxed)) {
                DXGI_FORMAT format;
                if (!tracked_texture(address, resource, format)) resource = nullptr;
            }
            if (!resource) continue;
            const auto desc = resource->GetDesc();
            if (desc.MipLevels < 2) continue; // scene render targets have one mip; cockpit screens have several
            static std::mutex logged_mutex;
            static std::set<std::pair<uint64_t, ID3D12Resource*>> logged;
            {
                std::lock_guard lock(logged_mutex);
                if (logged.size() >= 256 || !logged.insert({state.pso, resource}).second) continue;
            }
            bool slot = false;
            for (const auto& s : slot_resources) slot |= s.load() == resource;
            std::pair<uint64_t, uint64_t> hashes{};
            {
                std::shared_lock lock(hashes_mutex);
                if (const auto found = pipeline_hashes.find(state.pso); found != pipeline_hashes.end()) hashes = found->second;
            }
            log("screen_probe pso=%p vs=%016llx ps=%016llx t%u space%u texture=%p %llux%u mips=%u format=%u screen_slot=%d count=%u instances=%u layout=%p targets=%u%s",
                reinterpret_cast<void*>(state.pso), hashes.first, hashes.second, r.reg + k, r.space, resource, uint64_t(desc.Width), desc.Height,
                unsigned(desc.MipLevels), unsigned(desc.Format), slot, count, instances, reinterpret_cast<void*>(state.layout), state.target_count,
                describe_targets(state).c_str());
        }
    }
} catch (...) {}

// Pixel probe (button in the Bug reports section): for one frame, every draw is first drawn once more clipped to the
// pixel in the middle of its render target, inside an occlusion query (before the game's own draw, so the depth test is
// the same as for it). The draws that reach that pixel are logged with their shaders, render target and the cockpit-
// screen textures their pixel shader can read: shows how whatever is in the middle of the view is drawn.
constexpr uint32_t pixel_probe_max = 8192;
std::atomic<uint64_t> pixel_probe_frame{UINT64_MAX}; // first present count whose draws are probed (then a few more:
                                                     // with frame generation the scene is recorded across presents)
constexpr uint64_t pixel_probe_frames = 4;
std::atomic<uint32_t> pixel_probe_used{0};
std::atomic<int> pixel_probe_hits{-1}; // offline test: draws that reached the pixel in the last probe (-1: none done)
std::atomic<int> pixel_probe_display_hits{-1};
std::mutex pixel_probe_pending_mutex;
std::vector<uint32_t> pixel_probe_pending; // queries ended inside render passes, not yet resolved
struct ProbedDraw { uint64_t pso = 0, vs = 0, ps = 0; uint32_t count = 0, instances = 0, first = 0; int32_t base_vertex = 0; bool indexed = false, display = false;
    bool in_pass = false; int indirect = -1; // indirect: the command type (not re-issued: its count buffer is unknown)
    UINT targets = 0; uint32_t rt_w = 0, rt_h = 0, rt_format = 0; std::string textures; };
std::mutex pixel_probe_mutex;
std::vector<ProbedDraw> pixel_probe_draws;
bool pixel_probing() { return present_count.load(std::memory_order_relaxed) - pixel_probe_frame.load(std::memory_order_relaxed) < pixel_probe_frames; }
void pixel_probe(ListState& state, const glass::Draw& draw, int indirect = -1) noexcept try {
    Internal internal; // the probe draw and the scissor changes below go through the hooked functions
    const D3D12_CPU_DESCRIPTOR_HANDLE rt0{state.in_pass ? SIZE_T(state.pass_rt0) : state.targets[0].ptr};
    const UINT target_count = state.in_pass ? state.pass_targets : state.target_count;
    if (!state.device || !state.device->probe_queries || !target_count || !rt0.ptr) return;
    const auto rt = rtv_resource(rt0);
    if (!rt) return;
    const auto t = rt->GetDesc();
    // The scene at the upscaler's render size only (not shadow maps, reflections, gauges or post-processing).
    if (t.Width != last_render_w.load() || t.Height != last_render_h.load()) return;
    const uint32_t index = pixel_probe_used.fetch_add(1);
    if (index >= pixel_probe_max) return;
    auto native = state.native;
    // Indirect draws are not re-issued (ExecuteIndirect may take its draw count from a buffer): they are only listed as
    // found at this size.
    if (indirect < 0) {
        const LONG cx = LONG(t.Width / 2), cy = LONG(t.Height / 2);
        const D3D12_RECT pixel{cx, cy, cx + 1, cy + 1};
        native->RSSetScissorRects(1, &pixel);
        native->BeginQuery(state.device->probe_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        if (draw.indexed) native->DrawIndexedInstanced(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
        else native->DrawInstanced(draw.count, draw.instances, draw.first, draw.first_instance);
        native->EndQuery(state.device->probe_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        if (state.in_pass) {
            // No copies inside a render pass: resolved at a later present (pixel_probe_resolve).
            std::lock_guard lock(pixel_probe_pending_mutex);
            pixel_probe_pending.push_back(index);
        } else {
            native->ResolveQueryData(state.device->probe_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, index, 1, state.device->probe_readback.Get(), uint64_t(index) * 8);
        }
        const D3D12_RECT whole{0, 0, LONG(t.Width), LONG(t.Height)};
        native->RSSetScissorRects(1, state.scissor_known ? &state.scissor : &whole);
    }
    ProbedDraw p;
    p.in_pass = state.in_pass; p.indirect = indirect;
    p.pso = state.pso; p.count = draw.count; p.instances = draw.instances; p.first = draw.first; p.base_vertex = draw.base_vertex; p.indexed = draw.indexed;
    p.display = state.display != nullptr; p.targets = target_count; p.rt_w = uint32_t(t.Width); p.rt_h = t.Height; p.rt_format = uint32_t(t.Format);
    {
        std::shared_lock lock(hashes_mutex);
        if (const auto found = pipeline_hashes.find(state.pso); found != pipeline_hashes.end()) { p.vs = found->second.first; p.ps = found->second.second; }
    }
    // Cockpit-screen-like textures the pixel shader's tables hold (like the screen probe).
    std::vector<DeviceState::SrvRange> ranges;
    {
        std::shared_lock lock(state.device->layouts_mutex);
        if (const auto found = state.device->srv_ranges.find(state.layout); found != state.device->srv_ranges.end()) ranges = found->second;
    }
    for (const auto& r : ranges) {
        const auto table = r.param < state.tables.size() ? state.tables[r.param] : 0;
        if (!table) continue;
        for (uint32_t k = 0; k < r.count; ++k) {
            const auto address = descriptor_address(table, r.offset + k);
            if (!address) continue;
            Signature sig{};
            if (!read_bytes(reinterpret_cast<const void*>(address), sig.bytes.data(), descriptor_size)) continue;
            ID3D12Resource* resource = nullptr;
            {
                std::shared_lock lock(textures_mutex);
                if (const auto found = signatures.find(sig); found != signatures.end()) resource = found->second.resource;
            }
            char item[160];
            if (!resource) {
                // Any other 2D texture: what its view was made of (no resource pointer; resource flags: 0x1 render target,
                // 0x4 unordered access).
                ViewInfo info{};
                bool described = false;
                { std::lock_guard lock(diag_mutex); if (const auto d = diag_views.find(sig); d != diag_views.end()) { info = d->second; described = true; } }
                if (!described) {
                    // Bound, but its view is unknown (made before the fix loaded, or forgotten).
                    snprintf(item, sizeof(item), " t%u=?", r.reg + k);
                    p.textures += item;
                    continue;
                }
                snprintf(item, sizeof(item), " t%u=other(%ux%u mips=%u fmt=%u flags=0x%x)", r.reg + k, info.width, info.height, unsigned(info.levels), info.format, info.usage);
                p.textures += item;
                continue;
            }
            const auto d = resource->GetDesc();
            int slot = -1;
            for (unsigned i = 0; i < slot_count; ++i) if (slot_resources[i].load() == resource) slot = int(i);
            snprintf(item, sizeof(item), " t%u=%p(%llux%u mips=%u fmt=%u slot=%d)", r.reg + k, resource, uint64_t(d.Width), d.Height, unsigned(d.MipLevels), unsigned(d.Format), slot);
            p.textures += item;
        }
    }
    std::lock_guard lock(pixel_probe_mutex);
    if (pixel_probe_draws.size() <= index) pixel_probe_draws.resize(index + 1);
    pixel_probe_draws[index] = std::move(p);
} catch (...) {}

// Called once per present: once the probed frames are submitted, resolves the queries ended inside render passes, with
// a command list of its own on the queue the sim presents with.
void pixel_probe_resolve(ID3D12CommandQueue* queue, uint64_t now) {
    const uint64_t frame = pixel_probe_frame.load();
    if (frame == UINT64_MAX || now != frame + pixel_probe_frames + 1 || !queue || !main_state || !main_state->probe_queries) return;
    std::vector<uint32_t> pending;
    { std::lock_guard lock(pixel_probe_pending_mutex); pending.swap(pixel_probe_pending); }
    if (pending.empty()) return;
    Internal internal;
    // Kept until the next probe's resolve, by when the GPU has long finished this one.
    static ComPtr<ID3D12CommandAllocator> allocator;
    static ComPtr<ID3D12GraphicsCommandList> list;
    allocator = nullptr; list = nullptr;
    if (FAILED(main_state->native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(main_state->native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return;
    for (const uint32_t index : pending)
        list->ResolveQueryData(main_state->probe_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, index, 1, main_state->probe_readback.Get(), uint64_t(index) * 8);
    if (FAILED(list->Close())) return;
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
}

// Called once per present: logs a finished pixel probe.
void pixel_probe_flush(uint64_t now) {
    const uint64_t frame = pixel_probe_frame.load();
    if (frame == UINT64_MAX || now < frame + pixel_probe_frames + 4 || !main_state || !main_state->probe_readback) return;
    pixel_probe_frame = UINT64_MAX;
    const uint32_t used = std::min(pixel_probe_used.load(), pixel_probe_max);
    void* data = nullptr;
    const D3D12_RANGE range{0, SIZE_T(used) * 8};
    if (FAILED(main_state->probe_readback->Map(0, &range, &data))) return;
    std::vector<uint64_t> samples(static_cast<const uint64_t*>(data), static_cast<const uint64_t*>(data) + used);
    const D3D12_RANGE none{0, 0};
    main_state->probe_readback->Unmap(0, &none);
    std::lock_guard lock(pixel_probe_mutex);
    int hits = 0, display_hits = 0;
    std::set<std::tuple<uint64_t, uint32_t, uint32_t, int32_t>> seen; // the same draw in several frames: once
    std::set<uint64_t> indirect_seen;
    for (uint32_t i = 0; i < used && i < pixel_probe_draws.size(); ++i) {
        const auto& p = pixel_probe_draws[i];
        if (p.indirect >= 0) {
            if (indirect_seen.insert(p.pso).second)
                log("pixel_probe indirect (not testable) type=%d pso=%p vs=%016llx ps=%016llx draws=%u stride=%u in_pass=%d targets=%u rt0=%ux%u format=%u%s",
                    p.indirect, reinterpret_cast<void*>(p.pso), p.vs, p.ps, p.count, p.first, int(p.in_pass), p.targets, p.rt_w, p.rt_h, p.rt_format, p.textures.c_str());
            continue;
        }
        if (!samples[i]) continue;
        if (!seen.emplace(p.pso, p.count, p.first, p.base_vertex).second) continue;
        ++hits; display_hits += p.display;
        log("pixel_probe hit samples=%llu pso=%p vs=%016llx ps=%016llx display=%d %s count=%u first=%u base_vertex=%d instances=%u in_pass=%d targets=%u rt0=%ux%u format=%u%s",
            samples[i], reinterpret_cast<void*>(p.pso), p.vs, p.ps, int(p.display), p.indexed ? "indexed" : "plain", p.count, p.first, p.base_vertex, p.instances,
            int(p.in_pass), p.targets, p.rt_w, p.rt_h, p.rt_format, p.textures.c_str());
    }
    log("pixel_probe done: %u draws probed at %ux%u over %llu frames, %d distinct draws reach the middle pixel (%d of them screen draws)", used,
        last_render_w.load(), last_render_h.load(), pixel_probe_frames, hits, display_hits);
    pixel_probe_hits = hits; pixel_probe_display_hits = display_hits;
    pixel_probe_draws.clear();
}

// Indirect commands (ExecuteIndirect): what each command signature runs, as ReShade's add-on events report it.
enum class Indirect { unknown = 0, draw = 1, draw_indexed = 2, dispatch = 3 };
struct __declspec(uuid("7D3B9A1E-2C64-4F0B-8E51-A9C02F6D4B37")) IndirectCommand { Indirect type = Indirect::unknown; UINT stride = 0; };
void command_signature_created(const D3D12_COMMAND_SIGNATURE_DESC& desc, ID3D12CommandSignature* signature) {
    IndirectCommand command{Indirect::unknown, desc.ByteStride};
    if (desc.NumArgumentDescs && desc.pArgumentDescs) {
        switch (desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type) { // the last argument is the command itself
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: command.type = Indirect::draw; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: command.type = Indirect::draw_indexed; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: command.type = Indirect::dispatch; break;
        default: break;
        }
    }
    signature->SetPrivateData(__uuidof(IndirectCommand), sizeof(command), &command);
}
// Indirect draws at render size are only listed by the pixel probe (see pixel_probe).
void indirect_recorded(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT max_count) {
    if (!enabled.load(std::memory_order_relaxed) || duplicate_fix.load(std::memory_order_relaxed) || !signature || !pixel_probing()) return;
    IndirectCommand command;
    UINT size = sizeof(command);
    if (FAILED(signature->GetPrivateData(__uuidof(IndirectCommand), &size, &command)) || size != sizeof(command)) command = {};
    if (command.type == Indirect::dispatch) return;
    // Logged like a draw: draw count in count, stride in first.
    if (auto state = list_state(list)) pixel_probe(*state, {command.type == Indirect::draw_indexed, max_count, 1, command.stride, 0, 0}, int(command.type));
}

// Called before the sim's draw runs. Returns true when the fix issued the draw itself (the screen's map is drawn right
// after it, against the depth it wrote): the hook then skips the original call.
bool draw_recorded(ID3D12GraphicsCommandList* list, const glass::Draw& draw) {
    CpuScope scope(cpu_draws);
    if (!enabled.load(std::memory_order_relaxed) || duplicate_fix.load(std::memory_order_relaxed)) return false;
    auto state = list_state(list);
    if (!state) return false;
    const auto now = present_count.load(std::memory_order_relaxed);
    if (pixel_probing()) pixel_probe(*state, draw);
    if (!state->display && now < screen_probe_until.load(std::memory_order_relaxed)) screen_probe(*state, draw.count, draw.instances);
    if (!state->display && now < probe_until.load(std::memory_order_relaxed)) probe_screen_draw(*state, draw.count, draw.instances);
    if (!state->display) return false;
    ++stats.display_draws;
    return state->generation == generation.load(std::memory_order_relaxed) && replay(*state, draw);
}


// ---------------------------------------------------------------------------------------------------------------
// Composite after DLSS.
bool create_composite(DeviceState& d) {
    D3D12_DESCRIPTOR_RANGE ranges[5]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, uav_base, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, uav_base},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 5, 0, uav_base + 4}, // u4 is the root UAV (fit sums)
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, uav_base, 0, uav_base + 10}, // t34: the upscaler's depth input
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 11, 0, uav_base + 11}}; // u11: screen capture (what the brightness match reads)
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 22};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {5, ranges};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {4, 0};
    D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 8, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX, 0, 0,
        D3D12_SHADER_VISIBILITY_ALL};
    D3D12_ROOT_SIGNATURE_DESC desc{3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(d.native->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&d.root)))) return false;
    const std::array<std::pair<const unsigned char*, size_t>, 6> code{{
        {cs_zero, sizeof(cs_zero)}, {cs_fit, sizeof(cs_fit)}, {cs_solve, sizeof(cs_solve)}, {cs_composite, sizeof(cs_composite)}, {cs_clear, sizeof(cs_clear)},
        {cs_glass_filter, sizeof(cs_glass_filter)}}};
    for (size_t i = 0; i < code.size(); ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{d.root.Get(), {code[i].first, code[i].second}};
        if (FAILED(d.native->CreateComputePipelineState(&p, IID_PPV_ARGS(&d.passes[i])))) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ring_size * descriptors_per_frame, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(d.native->CreateDescriptorHeap(&h, IID_PPV_ARGS(&d.heap)))) return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, slot_count * 20 * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.sums)))) return false;
    buffer.Width = slot_count * 3 * 16;
    if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.params)))) return false;
    buffer.Width = 2 * slot_count * bounds_cells * bounds_cells * 16 + 256 + 512; // two halves of learnt edges, call counters,
                                                                                   // per-slot brightness references
    if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d.edge_bounds)))) return false;
    D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(d.native->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.params_readback))))
        d.params_readback = nullptr; // diagnostics only
    buffer.Width = 256;
    if (FAILED(d.native->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.diag_readback))))
        d.diag_readback = nullptr;
    D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, max_timed_calls * 2, 0};
    buffer.Width = max_timed_calls * 2 * sizeof(uint64_t);
    if (FAILED(d.native->CreateQueryHeap(&queries, IID_PPV_ARGS(&d.timestamps))) ||
        FAILED(d.native->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.timestamps_readback))))
        d.timestamps = nullptr, d.timestamps_readback = nullptr; // diagnostics only
    return true;
}

// Log the brightness match (scene = gain * screen + offset, per colour channel) of every screen in use. Copied from
// the GPU every ~600 frames and read ~30 frames later, when the copy has long finished.
void log_brightness(DeviceState& d) {
    const uint64_t now = present_count.load(std::memory_order_relaxed);
    uint64_t ready = d.readback_ready.load();
    if (now < ready || !d.readback_ready.compare_exchange_strong(ready, UINT64_MAX)) return;
    float* data = nullptr;
    const D3D12_RANGE range{0, slot_count * 3 * 16};
    if (FAILED(d.params_readback->Map(0, &range, reinterpret_cast<void**>(&data)))) return;
    // Every dump for the first ~10 minutes, then about once a minute.
    static std::atomic<int> dumps{0};
    const int dump = dumps++;
    if (dump < 60 || dump % 6 == 0) {
        for (unsigned slot = 0; slot < slot_count; ++slot) {
            const float* a = data + slot * 12;
            const float* b = a + 4;
            const float* q = a + 8;
            const auto resource = slot_resources[slot].load();
            if (a[3] == 0 || !resource) continue;
            const auto desc = resource->GetDesc();
            log("brightness slot=%u screen=%llux%u gain=%.3f,%.3f,%.3f offset=%.4f,%.4f,%.4f mean=%.4f,%.4f,%.4f quality=%.3f samples=%.0f%s", slot,
                uint64_t(desc.Width), desc.Height, a[0], a[1], a[2], b[0], b[1], b[2], q[0], q[1], q[2], q[3], b[3],
                a[3] < 0 ? " skipped (screen doesn't match the sim's picture)" : "");
        }
    }
    const D3D12_RANGE none{0, 0};
    d.params_readback->Unmap(0, &none);
}

ID3D12Resource* ngx_resource(const NVSDK_NGX_Parameter& params, const char* name) {
    ID3D12Resource* resource = nullptr;
    if (params.Get(name, &resource) == NVSDK_NGX_Result_Success) return resource;
    void* pointer = nullptr;
    return params.Get(name, &pointer) == NVSDK_NGX_Result_Success ? static_cast<ID3D12Resource*>(pointer) : nullptr;
}

// VR view order. The sim upscales its views one by one (into a temporary texture, then copies it into its layer of
// the final layered image) and need not do that in layer order: with foveated rendering it doesn't. The copies tell
// which layer each call of a frame belongs to; learnt on the first frames, used from then on.
struct ViewOrder {
    std::mutex mutex;
    UINT views = 0;                                  // views per frame (map layers) the order below belongs to
    ID3D12Resource* last_output = nullptr;           // output of the latest call (the sim reuses one temporary texture)
    UINT last_call = 0;
    std::array<int, 8> layer{-1, -1, -1, -1, -1, -1, -1, -1};
} view_order;

UINT layer_for_call(UINT call, UINT views, ID3D12Resource* output) {
    std::lock_guard lock(view_order.mutex);
    if (view_order.views != views) {
        view_order.views = views;
        view_order.layer.fill(-1);
    }
    if (call >= 8) return call;
    view_order.last_output = output;
    view_order.last_call = call;
    if (view_order.layer[call] >= 0) return UINT(view_order.layer[call]);
    // Only one call left unknown: it gets the remaining layer.
    UINT known = 0, used = 0;
    for (UINT i = 0; i < views; ++i) if (view_order.layer[i] >= 0) { ++known; used |= 1u << view_order.layer[i]; }
    if (known + 1 == views) for (UINT l = 0; l < views; ++l) if (!(used & (1u << l))) return l;
    return call;
}

// A texture-to-texture copy (CopyTextureRegion between two subresources).
void texture_copied(ID3D12Resource* src, ID3D12Resource* dest, UINT dest_subresource) {
    CpuScope scope(cpu_copies);
    if (!view_order.views) return;
    std::lock_guard lock(view_order.mutex);
    // A copy of the latest call's output into a layered texture: that call's view is the destination layer.
    if (!src || !dest || src != view_order.last_output) return;
    const auto desc = dest->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != view_order.views) return;
    const UINT call = view_order.last_call;
    view_order.last_output = nullptr; // one copy per call; frames without a fixed view must not reuse it
    const int layer = int(dest_subresource / std::max<UINT16>(desc.MipLevels, 1));
    if (call < 8 && view_order.layer[call] != layer) {
        view_order.layer[call] = layer;
        log("vr view order: upscaler call %u of %u -> layer %d", call, view_order.views, layer);
    }
}

// What the upscaler (DLSS or FSR) was given and wrote, and the states it left its colour input and output in.
struct Upscale {
    const char* name;
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned render_w = 0, render_h = 0;
    float jitter_x = 0, jitter_y = 0;
    D3D12_RESOURCE_STATES colour_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* depth = nullptr; // render-resolution depth input (occlusion check)
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};

// Shader-readable format of a depth buffer.
DXGI_FORMAT depth_srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
void composite(ID3D12GraphicsCommandList* supplied, const Upscale& u) noexcept;


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
    {
        // The first calls of every render size in detail: shows how the sim lays out views (one per eye in VR, side by
        // side, arrays...).
        static std::mutex sizes_mutex;
        static std::unordered_map<uint64_t, int> sizes;
        std::unique_lock sizes_lock(sizes_mutex);
        auto& seen = sizes[(uint64_t(render_w) << 32) | render_h];
        const bool log_call = sizes.size() <= 8 && seen++ < 6;
        sizes_lock.unlock();
        if (log_call) {
            D3D12_RESOURCE_DESC c{}, o{};
            if (colour) c = colour->GetDesc();
            if (output) o = output->GetDesc();
            log("dlss_call render=%ux%u colour=%p %llux%u layers=%u base=%u,%u output=%p %llux%u layers=%u base=%u,%u jitter=%g,%g",
                render_w, render_h, colour, c.Width, c.Height, unsigned(c.DepthOrArraySize), base[0], base[1],
                output, o.Width, o.Height, unsigned(o.DepthOrArraySize), base[2], base[3], jitter_x, jitter_y);
        }
    }
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
    unsigned depth_base[2]{};
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &depth_base[0]);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &depth_base[1]);
    if (!depth_base[0] && !depth_base[1]) u.depth = ngx_resource(*params, NVSDK_NGX_Parameter_Depth);
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
    u.depth = static_cast<ID3D12Resource*>(desc.depth.resource);
    u.depth_state = d3d12_state(desc.depth.state);
    {
        const auto o = u.output->GetDesc(), c = u.colour->GetDesc();
        LOG_ONCE("milestone first FSR frame render=%ux%u output=%llux%u output_format=%u colour_format=%u jitter=%g,%g colour_state=0x%x output_state=0x%x",
            u.render_w, u.render_h, o.Width, o.Height, unsigned(o.Format), unsigned(c.Format), u.jitter_x, u.jitter_y, desc.color.state, desc.output.state);
    }
    composite(static_cast<ID3D12GraphicsCommandList*>(desc.commandList), u);
} catch (...) { ++stats.composite_skipped; }

// ---------------------------------------------------------------------------------------------------------------
// Screen capture for bug reports (Troubleshooting -> Capture screens). For a few consecutive fixed frames it saves the
// sim's raw frame (the upscaler's input), the upscaler's output before and after the fix and the screen map as PNG
// files in a folder next to the log, plus a short summary. Shows what each stage drew, pixel by pixel. Costs nothing
// until requested; the copies are made on the upscaler's command list and written to disk on a background thread.
struct CaptureImage {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string name;
};
struct CaptureFrame {
    std::vector<CaptureImage> images;
    ComPtr<ID3D12Resource> params; // per-slot colour match (a, b, quality) after this frame's fit
    std::string upscaler;
    UINT render_w = 0, render_h = 0, out_w = 0, out_h = 0, layer = 0;
    float jitter_x = 0, jitter_y = 0;
    int presented = -1; // >= 0: a frame sent to the monitor (index), not an upscaler call
    bool real = false;  // presented frame: the fix ran since the previous present (a real frame, not a generated one)
    ComPtr<ID3D12CommandAllocator> allocator; // presented frame: the copy's own command list, kept until it is read back
    ComPtr<ID3D12GraphicsCommandList> list;
};
constexpr int capture_frame_count = 4, capture_spacing = 5; // 4 frames, every 5th fixed frame (~0.3 s)
std::atomic<int> capture_countdown{0};
// The capture waits (up to capture_wait_limit presents) until the draw-time colour shaders of every cockpit display
// pipeline are compiled: they compile in the background, and the captured frames span only a third of a second.
std::atomic<uint64_t> capture_wait_until{0};
constexpr uint64_t capture_wait_limit = 300;
bool colour_variants_ready() {
    std::lock_guard lock(pipelines_mutex);
    for (size_t i = 0; i < max_pipelines; ++i)
        if (pipeline_keys[i].load() && pipeline_values[i] && !pipeline_values[i]->vr) {
            if (pipeline_values[i]->colour_variant.load()) continue;
            colour_variant(pipeline_values[i]); // requests it if not yet
            return false;
        }
    return true;
}
// Frames sent to the monitor, consecutively from the request (frame generation's frames included).
constexpr int capture_present_count = 8;
std::atomic<int> capture_presents_left{0};
std::mutex capture_mutex;
std::atomic<int> capture_requested{0};
std::vector<CaptureFrame> capture_frames;
uint64_t capture_ready_at = 0; // present count by which the GPU has surely finished the copies
std::filesystem::path capture_folder;
std::string capture_message;

void capture_copy(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT subresource,
    D3D12_RESOURCE_STATES state, DXGI_FORMAT format, const char* name, CaptureFrame& frame) {
    if (!resource) return;
    const auto desc = resource->GetDesc();
    CaptureImage image;
    image.format = format; image.name = name;
    UINT64 row_bytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &image.footprint, &image.rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image.buffer)))) return;
    transition(list, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{image.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, from{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    to.PlacedFootprint = image.footprint;
    from.SubresourceIndex = subresource;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    frame.images.push_back(std::move(image));
}

float half_to_float(uint16_t h) {
    const uint32_t exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? (mantissa ? NAN : INFINITY) : std::ldexp(float(mantissa | 1024), int(exponent) - 25);
    return (h & 0x8000) ? -value : value;
}
float small_float(uint32_t bits, int mantissa_bits) {
    const uint32_t exponent = bits >> mantissa_bits, mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0) return std::ldexp(float(mantissa), -14 - mantissa_bits);
    if (exponent == 31) return INFINITY;
    return std::ldexp(1.0f + float(mantissa) / float(1u << mantissa_bits), int(exponent) - 15);
}
float srgb_decode(uint8_t v) { const float c = v / 255.0f; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
// Linear RGBA of one texel, or false for an unsupported format.
bool decode_texel(DXGI_FORMAT format, const uint8_t* p, std::array<float, 4>& c) {
    switch (format) {
    case DXGI_FORMAT_R11G11B10_FLOAT: { uint32_t v; std::memcpy(&v, p, 4); c = {small_float(v & 0x7ff, 6), small_float((v >> 11) & 0x7ff, 6), small_float(v >> 22, 5), 1}; return true; }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: { uint16_t v[4]; std::memcpy(v, p, 8); c = {half_to_float(v[0]), half_to_float(v[1]), half_to_float(v[2]), half_to_float(v[3])}; return true; }
    case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(c.data(), p, 16); return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: c = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f}; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: c = {srgb_decode(p[0]), srgb_decode(p[1]), srgb_decode(p[2]), p[3] / 255.0f}; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: c = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f}; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: { uint32_t v; std::memcpy(&v, p, 4); c = {(v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f, 1}; return true; }
    default: return false;
    }
}
UINT texel_bytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}

HRESULT save_png(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgba) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT result = S_OK;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> converter;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
        if (SUCCEEDED(result)) result = frame->SetSize(width, height);
        if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
        if (SUCCEEDED(result)) result = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA, width * 4, UINT(rgba.size()),
            const_cast<BYTE*>(rgba.data()), &bitmap);
        if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(result)) result = frame->WriteSource(converter.Get(), nullptr);
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

// A plain zip (stored, no compression: the PNGs are compressed already) so a capture is one file to attach.
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
bool write_zip(const std::filesystem::path& zip, const std::vector<std::pair<std::string, std::filesystem::path>>& entries) {
    std::ofstream out(zip, std::ios::binary);
    if (!out) return false;
    SYSTEMTIME t; GetLocalTime(&t);
    const uint16_t time = uint16_t((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2)), date = uint16_t(((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
    std::vector<uint8_t> directory;
    const auto put = [](std::vector<uint8_t>& v, uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) v.push_back(uint8_t(value >> (8 * i))); };
    uint32_t offset = 0, count = 0;
    for (const auto& [name, path] : entries) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() >= 0xffffffffu || offset >= 0x7fffffffu) break;
        const uint32_t crc = crc32(data.data(), data.size()), size = uint32_t(data.size());
        std::vector<uint8_t> header;
        put(header, 0x04034b50, 4); put(header, 10, 2); put(header, 0, 2); put(header, 0, 2); put(header, time, 2); put(header, date, 2);
        put(header, crc, 4); put(header, size, 4); put(header, size, 4); put(header, uint32_t(name.size()), 2); put(header, 0, 2);
        header.insert(header.end(), name.begin(), name.end());
        out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        put(directory, 0x02014b50, 4); put(directory, 20, 2); put(directory, 10, 2); put(directory, 0, 2); put(directory, 0, 2); put(directory, time, 2);
        put(directory, date, 2); put(directory, crc, 4); put(directory, size, 4); put(directory, size, 4); put(directory, uint32_t(name.size()), 2);
        put(directory, 0, 2); put(directory, 0, 2); put(directory, 0, 2); put(directory, 0, 2); put(directory, 0, 4); put(directory, offset, 4);
        directory.insert(directory.end(), name.begin(), name.end());
        offset += uint32_t(header.size()) + size;
        ++count;
    }
    std::vector<uint8_t> end;
    put(end, 0x06054b50, 4); put(end, 0, 2); put(end, 0, 2); put(end, count, 2); put(end, count, 2);
    put(end, uint32_t(directory.size()), 4); put(end, offset, 4); put(end, 0, 2);
    out.write(reinterpret_cast<const char*>(directory.data()), std::streamsize(directory.size()));
    out.write(reinterpret_cast<const char*>(end.data()), std::streamsize(end.size()));
    return bool(out);
}

struct CapturedPixels { std::string name; DXGI_FORMAT format; UINT width = 0, height = 0, pitch = 0; std::vector<uint8_t> bytes; };
struct CapturedFrame { CaptureFrame info; std::vector<CapturedPixels> images; std::vector<float> params; };

// Writes the frames: colour images share one exposure (from the upscaler output) so they compare 1:1; the map shows
// u in red, v in green and the screen slot in blue.
void write_capture(std::vector<CapturedFrame> frames, std::filesystem::path folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::ofstream summary(folder / L"summary.txt");
    summary << "Glass Cockpit DLSS Fix " << GLASS_VERSION_STRING << " screen capture\n";
    float scale = 1;
    for (const auto& frame : frames) for (const auto& image : frame.images) if (image.name == "2-upscaler-output") {
        std::vector<float> peaks;
        for (UINT y = 0; y < image.height; y += 2) for (UINT x = 0; x < image.width; x += 2) {
            std::array<float, 4> c{};
            if (decode_texel(image.format, image.bytes.data() + size_t(y) * image.pitch + size_t(x) * texel_bytes(image.format), c)) {
                const float peak = std::max({c[0], c[1], c[2]});
                if (peak > 0 && std::isfinite(peak)) peaks.push_back(peak);
            }
        }
        if (!peaks.empty()) {
            auto at = peaks.begin() + std::ptrdiff_t(peaks.size() * 995 / 1000);
            std::nth_element(peaks.begin(), at, peaks.end());
            if (*at > 0) scale = 1 / *at;
        }
        break;
    }
    summary << "exposure scale (all colour images): " << scale << "\n";
    // Colour images are cropped to the screens (all maps together, plus a margin) to keep the folder small; with no
    // screens recognised they stay whole.
    UINT map_w = 0, map_h = 0, crop[4]{UINT_MAX, UINT_MAX, 0, 0};
    for (const auto& frame : frames) for (const auto& image : frame.images) if (image.name == "map") {
        map_w = image.width; map_h = image.height;
        for (UINT y = 0; y < image.height; y += 2) for (UINT x = 0; x < image.width; x += 2) {
            std::array<float, 4> c{};
            if (decode_texel(image.format, image.bytes.data() + size_t(y) * image.pitch + size_t(x) * texel_bytes(image.format), c) && c[3] > 0.5f) {
                crop[0] = std::min(crop[0], x); crop[1] = std::min(crop[1], y); crop[2] = std::max(crop[2], x + 2); crop[3] = std::max(crop[3], y + 2);
            }
        }
    }
    const bool cropping = map_w && map_h && crop[2] > crop[0] && crop[3] > crop[1];
    if (cropping) {
        constexpr UINT margin = 32;
        crop[0] = crop[0] > margin ? crop[0] - margin : 0; crop[1] = crop[1] > margin ? crop[1] - margin : 0;
        crop[2] = std::min(crop[2] + margin, map_w); crop[3] = std::min(crop[3] + margin, map_h);
        summary << "colour images cropped to map pixels " << crop[0] << "," << crop[1] << " to " << crop[2] << "," << crop[3]
            << " (scaled to each image's size; the map image is whole)\n";
    }
    // Area of an image to save: x0, y0, x1, y1 (end exclusive).
    const auto area = [&](UINT width, UINT height, bool whole) {
        std::array<UINT, 4> a{0, 0, width, height};
        if (!cropping || whole || !width || !height) return a;
        const double sx = double(width) / map_w, sy = double(height) / map_h;
        a[0] = std::min(width - 1, UINT(crop[0] * sx)); a[1] = std::min(height - 1, UINT(crop[1] * sy));
        a[2] = std::clamp(UINT(std::ceil(crop[2] * sx)), a[0] + 1, width); a[3] = std::clamp(UINT(std::ceil(crop[3] * sy)), a[1] + 1, height);
        return a;
    };
    std::vector<CapturedFrame> fixed, presented;
    for (auto& frame : frames) (frame.info.presented >= 0 ? presented : fixed).push_back(std::move(frame));
    if (!presented.empty()) summary << "\npresented frames (sent to the monitor, consecutive; real = the fix ran since the previous one, generated = frame generation):\n";
    for (const auto& frame : presented) {
        for (const auto& image : frame.images) {
            const auto a = area(image.width, image.height, false);
            const UINT w = a[2] - a[0], h = a[3] - a[1];
            std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
            const bool linear = image.format == DXGI_FORMAT_R16G16B16A16_FLOAT; // scRGB; the others are already display-encoded
            bool decoded = true;
            for (UINT y = a[1]; y < a[3] && decoded; ++y) for (UINT x = a[0]; x < a[2]; ++x) {
                std::array<float, 4> c{};
                if (!decode_texel(image.format, image.bytes.data() + size_t(y) * image.pitch + size_t(x) * texel_bytes(image.format), c)) { decoded = false; break; }
                uint8_t* out = &rgba[(size_t(y - a[1]) * w + (x - a[0])) * 4];
                for (int i = 0; i < 3; ++i) {
                    const float v = std::clamp(std::isfinite(c[i]) ? c[i] : 0.0f, 0.0f, 1.0f);
                    out[i] = uint8_t(std::lround((linear ? (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f) : v) * 255));
                }
                out[3] = 255;
            }
            const auto name = "present" + std::to_string(frame.info.presented) + (frame.info.real ? "-real" : "-generated");
            const auto file = folder / (std::wstring(name.begin(), name.end()) + L".png");
            const HRESULT result = decoded ? save_png(file, w, h, rgba) : E_NOTIMPL;
            summary << "  " << name << ": " << image.width << "x" << image.height << " saved area " << a[0] << "," << a[1] << " to " << a[2] << "," << a[3] << " format " << unsigned(image.format)
                << (decoded ? (SUCCEEDED(result) ? " saved" : " save failed") : " format not supported") << "\n";
        }
    }
    frames = std::move(fixed);
    for (size_t f = 0; f < frames.size(); ++f) {
        const auto& info = frames[f].info;
        summary << "\nframe " << f << ": " << info.upscaler << " render " << info.render_w << "x" << info.render_h << " output " << info.out_w << "x" << info.out_h
            << " jitter " << info.jitter_x << "," << info.jitter_y << " map layer " << info.layer << "\n";
        const auto& params = frames[f].params;
        for (unsigned slot = 0; params.size() >= slot_count * 12 && slot < slot_count; ++slot) {
            const float* a = params.data() + slot * 12;
            if (a[3] == 0) continue;
            char line[256];
            snprintf(line, sizeof(line), "    colour match slot %u: gain=%.4f,%.4f,%.4f offset=%.5f,%.5f,%.5f quality=%.3f %s\n", slot, a[0], a[1], a[2], a[4], a[5], a[6],
                a[11], a[3] > 0 ? "fixed" : "skipped");
            summary << line;
        }
        std::vector<uint8_t> map_records;
        for (const auto& image : frames[f].images) {
            const bool map_image = image.name == "map";
            const auto a = area(image.width, image.height, map_image);
            const UINT w = a[2] - a[0], h = a[3] - a[1];
            std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
            std::map<int, std::array<UINT, 5>> slots; // slot -> min x, min y, max x, max y, pixels
            bool decoded = true;
            for (UINT y = a[1]; y < a[3] && decoded; ++y) for (UINT x = a[0]; x < a[2]; ++x) {
                std::array<float, 4> c{};
                if (!decode_texel(image.format, image.bytes.data() + size_t(y) * image.pitch + size_t(x) * texel_bytes(image.format), c)) { decoded = false; break; }
                uint8_t* out = &rgba[(size_t(y - a[1]) * w + (x - a[0])) * 4];
                if (map_image) {
                    const int slot = int(c[3] + 0.5f) - 1;
                    if (slot >= 0) {
                        out[0] = uint8_t(std::clamp(c[0], 0.0f, 1.0f) * 255); out[1] = uint8_t(std::clamp(c[1], 0.0f, 1.0f) * 255);
                        out[2] = uint8_t(64 + (slot * 37) % 192);
                        auto [it, added] = slots.try_emplace(slot, std::array<UINT, 5>{x, y, x, y, 0});
                        auto& box = it->second;
                        box[0] = std::min(box[0], x); box[1] = std::min(box[1], y); box[2] = std::max(box[2], x); box[3] = std::max(box[3], y); ++box[4];
                    }
                } else for (int i = 0; i < 3; ++i) {
                    const float v = std::clamp(std::isfinite(c[i]) ? c[i] * (image.name == "fit-input" || image.name == "draw-input" ? 1.0f : scale) : 0.0f, 0.0f, 1.0f);
                    out[i] = uint8_t(std::lround((v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f) * 255));
                }
                // fit-input: alpha = slot + 1 where the brightness match counted the pixel, 0 where it didn't.
                out[3] = image.name == "fit-input" ? uint8_t(std::clamp(std::isfinite(c[3]) ? c[3] : 0.0f, 0.0f, 255.0f) + 0.5f) : 255;
                // The first frame's map at full precision, screen pixels only (the PNG holds 8 bits of each coordinate):
                // per pixel uint16 x, y; float u, v, depth; uint16 slot; float brightness (the mesh's, see display_map.hlsl).
                if (map_image && f == 0 && c[3] > 0.5f) {
                    const uint16_t xy[2]{uint16_t(x), uint16_t(y)}, slot16 = uint16_t(c[3] + 0.5f) - 1;
                    const float level = std::exp2((c[3] - std::round(c[3])) * 64);
                    const size_t at = map_records.size();
                    map_records.resize(at + 22);
                    std::memcpy(&map_records[at], xy, 4); std::memcpy(&map_records[at + 4], c.data(), 12); std::memcpy(&map_records[at + 16], &slot16, 2);
                    std::memcpy(&map_records[at + 18], &level, 4);
                }
            }
            const auto file = folder / (L"frame" + std::to_wstring(f) + L"-" + std::wstring(image.name.begin(), image.name.end()) + L".png");
            const HRESULT result = decoded ? save_png(file, w, h, rgba) : E_NOTIMPL;
            summary << "  " << image.name << ": " << image.width << "x" << image.height << " saved area " << a[0] << "," << a[1] << " to " << a[2] << "," << a[3] << " format " << unsigned(image.format)
                << (decoded ? (SUCCEEDED(result) ? " saved" : " save failed") : " format not supported") << "\n";
            for (const auto& [slot, box] : slots)
                summary << "    screen slot " << slot << ": map pixels " << box[0] << "," << box[1] << " to " << box[2] << "," << box[3] << " (" << box[4] << " pixels)\n";
        }
        if (!map_records.empty()) {
            std::ofstream raw(folder / (L"frame" + std::to_wstring(f) + L"-map-precise.bin"), std::ios::binary);
            raw.write(reinterpret_cast<const char*>(map_records.data()), std::streamsize(map_records.size()));
            summary << "  map-precise: " << map_records.size() / 22 << " screen pixels (uint16 x, y; float u, v, depth; uint16 slot; float brightness)\n";
        }
    }
    summary.close();
    // One zip with the pictures, the summary and both logs, ready to attach to a bug report.
    log("capture saved: %ls", folder.c_str());
    std::vector<std::pair<std::string, std::filesystem::path>> entries;
    for (const auto& file : std::filesystem::directory_iterator(folder, error))
        if (file.is_regular_file(error)) entries.emplace_back(folder.filename().string() + "/" + file.path().filename().string(), file.path());
    std::sort(entries.begin(), entries.end());
    const auto logs = folder.parent_path();
    entries.emplace_back(log_file_path.empty() ? std::string("glass-cockpit-dlss-fix.log") : log_file_path.filename().string(),
        log_file_path.empty() ? logs / L"glass-cockpit-dlss-fix.log" : log_file_path);
    entries.emplace_back("glass-cockpit-dlss-fix.old.log", logs / L"glass-cockpit-dlss-fix.old.log");
    auto zip = folder; zip += L".zip";
    const bool zipped = write_zip(zip, entries);
    log("capture zip %s: %ls", zipped ? "saved" : "failed", zip.c_str());
    std::lock_guard lock(capture_mutex);
    capture_message = zipped ? "Saved " + zip.filename().string() + " next to the log: attach it to your bug report"
                             : "Saved to " + folder.filename().string() + " (next to the log)";
}

// Called once per present: hands finished captures to the writer thread.
void capture_flush(uint64_t now) {
    std::vector<CaptureFrame> frames;
    std::filesystem::path folder;
    {
        std::lock_guard lock(capture_mutex);
        if (!capture_ready_at || now < capture_ready_at || capture_presents_left.load()) return;
        capture_ready_at = 0;
        frames.swap(capture_frames);
        folder = capture_folder;
    }
    std::vector<CapturedFrame> copies;
    for (auto& frame : frames) {
        CapturedFrame copy{frame, {}, {}};
        copy.info.images.clear();
        copy.info.params.Reset();
        copy.info.allocator.Reset();
        copy.info.list.Reset();
        if (frame.params) {
            void* data = nullptr;
            const D3D12_RANGE range{0, slot_count * 3 * 16};
            if (SUCCEEDED(frame.params->Map(0, &range, &data))) {
                copy.params.assign(static_cast<float*>(data), static_cast<float*>(data) + slot_count * 12);
                const D3D12_RANGE none{0, 0};
                frame.params->Unmap(0, &none);
            }
        }
        for (auto& image : frame.images) {
            CapturedPixels pixels{image.name, image.format, image.footprint.Footprint.Width, image.footprint.Footprint.Height, image.footprint.Footprint.RowPitch, {}};
            void* data = nullptr;
            const D3D12_RANGE range{0, SIZE_T(image.footprint.Footprint.RowPitch) * image.rows};
            if (SUCCEEDED(image.buffer->Map(0, &range, &data))) {
                pixels.bytes.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + range.End);
                const D3D12_RANGE none{0, 0};
                image.buffer->Unmap(0, &none);
                copy.images.push_back(std::move(pixels));
            }
        }
        copies.push_back(std::move(copy));
    }
    std::thread(write_capture, std::move(copies), folder).detach();
}

void composite(ID3D12GraphicsCommandList* supplied, const Upscale& u) noexcept try {
    CpuScope scope(cpu_composite);
    if (duplicate_fix.load(std::memory_order_relaxed)) return;
    // The list's state, also when the upscaler was handed another mod's wrapper of it (the wrapper passes private data on).
    ListState* state = nullptr; UINT size = sizeof(state);
    if (FAILED(supplied->GetPrivateData(__uuidof(ListState), &size, &state)) || size != sizeof(state) || !state) state = list_state(supplied);
    if (!state || !state->device) {
        note(stats.composite_skipped, "upscaler_command_list_unknown (the upscaler ran on a command list the fix has not seen)"); return;
    }
    auto& d = *state->device;
    Internal internal; // the passes below go through the hooked functions
    if (state->native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
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
        for (auto& m : d.maps) if (m->views) {
            LOG_ONCE("note %s render size %ux%u differs from the cockpit render target %ux%u (screens drawn in another view?)", u.name, render_w, render_h, m->width, m->height);
            static std::atomic<uint64_t> probed_size{0};
            const uint64_t render_size = (uint64_t(render_w) << 32) | render_h;
            if (probed_size.exchange(render_size) != render_size) {
                probe_until = present_count.load() + 900;
                log("vr_probe started for %ux%u: looking for draws that read a cockpit screen texture with another pipeline", render_w, render_h);
            }
            break;
        }
        return;
    }
    // Which view this call upscales: the sim runs the upscaler once per view, in layer order.
    {
        // Several upscaler calls at this render size: only the one with the largest output (the sim's real upscale,
        // seen within the last ~2 s) gets the fix.
        const auto o = output->GetDesc();
        const uint64_t area = uint64_t(o.Width) * o.Height, now = present_count.load(std::memory_order_relaxed);
        uint64_t largest = map->largest_output.load();
        if (area < largest && now - map->largest_output_present.load() < 120) {
            static std::atomic<int> logged{0};
            if (logged++ < 4) log("note %s call at %ux%u with output %llux%u skipped: another call at this render size has a larger output (the fix goes on that one)",
                u.name, render_w, render_h, o.Width, o.Height);
            return;
        }
        if (area >= largest) { map->largest_output = area; map->largest_output_present = now; }
    }
    uint32_t pending = map->views.load();
    uint32_t call_in_present = 0;
    int recovered_call = -1; // VR: the views count was used up, this call's view from its place since the last present
    if (map->layers > 1) {
        // VR: which view this call is, counted from the last present. In VR sessions it matched the views count on every
        // call (the sim upscales each frame's views in one go between presents).
        const uint64_t now = present_count.load(std::memory_order_relaxed);
        if (map->call_present.exchange(now) != now) map->calls_this_present = 0;
        call_in_present = map->calls_this_present++;
        static std::atomic<int> logged{0};
        if (logged++ < 64) log("vr_call present=%llu call_in_present=%u pending=%u layers=%u recorded=%llu output=%p", now, call_in_present, pending,
            map->layers, map->recorded_present.load(), output);
    }
    do {
        // The sim records frames on several threads: the next frame's screens can be recorded before this frame's
        // upscaler call, which then finds the count used up although its screens are in the map (the GPU runs them in
        // frame order). Without views to tell apart, fix it anyway while screens are being drawn; a second call in the
        // same frame finds the map already cleared and changes nothing.
        if (!pending && map->layers == 1 && present_count.load(std::memory_order_relaxed) - map->recorded_present.load() <= 8) break;
        // VR, the same: screens recorded lately but the count used up (the next frame's screens recorded first). The view
        // comes from the call's place since the last present instead of the count.
        if (!pending && map->layers > 1 && call_in_present < map->layers &&
            present_count.load(std::memory_order_relaxed) - map->recorded_present.load() <= 8) {
            recovered_call = int(call_in_present);
            static std::atomic<int> logged{0};
            if (logged++ < 8) log("note VR %s call %u found the views count used up: fixed by its place since the last present", u.name, call_in_present);
            break;
        }
        if (!pending) {
            // Diagnostics: an upscaler call that found this frame's screens already composited.
            static std::atomic<int> logged{0};
            if (logged++ < 6) log("note %s call at %ux%u output=%p found the screens already fixed this frame (another call at the same render size came first)",
                u.name, render_w, render_h, output);
            return;
        }
    } while (!map->views.compare_exchange_weak(pending, pending - 1));
    const bool fresh = pending > 0;
    const UINT layer = map->layers > 1 ? layer_for_call(recovered_call >= 0 ? UINT(recovered_call) : map->layers - pending, map->layers, output) : 0;
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
    D3D12_SHADER_RESOURCE_VIEW_DESC map_srv{glass::map_format, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    map_srv.Texture2DArray = {0, 1, 0, map->layers, 0, 0};
    d.native->CreateShaderResourceView(map->texture.Get(), &map_srv, at(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{glass::map_format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Texture2D.MipLevels = 1;
    srv.Format = in_format;
    if (in.DepthOrArraySize > 1) {
        // VR: layered textures (one layer per view). The upscaler reads and writes layer 0.
        D3D12_SHADER_RESOURCE_VIEW_DESC first_layer{in_format, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        first_layer.Texture2DArray = {0, 1, 0, 1, 0, 0};
        d.native->CreateShaderResourceView(colour, &first_layer, at(1));
    } else d.native->CreateShaderResourceView(colour, &srv, at(1));
    for (unsigned i = 0; i < slot_count; ++i) {
        auto resource = slot_resources[i].load();
        srv.Format = resource ? slot_formats[i] : DXGI_FORMAT_R8G8B8A8_UNORM;
        // The game's own mip range (the fit pass samples like the sim); full-size sampling reads level 0 in the shader.
        srv.Texture2D.MostDetailedMip = resource ? slot_first_mip[i] : 0;
        srv.Texture2D.MipLevels = !resource ? 1 : slot_mip_count[i];
        d.native->CreateShaderResourceView(resource, &srv, at(2 + i));
        srv.Texture2D.MostDetailedMip = 0;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{out_format, D3D12_UAV_DIMENSION_TEXTURE2D};
    if (out.DepthOrArraySize > 1) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC first_layer{out_format, D3D12_UAV_DIMENSION_TEXTURE2DARRAY};
        first_layer.Texture2DArray = {0, 0, 1, 0};
        d.native->CreateUnorderedAccessView(output, nullptr, &first_layer, at(uav_base + 0));
    } else d.native->CreateUnorderedAccessView(output, nullptr, &uav, at(uav_base + 0));
    uav.Format = DXGI_FORMAT_R32_UINT;
    d.native->CreateUnorderedAccessView(map->tiles.Get(), nullptr, &uav, at(uav_base + 1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
    structured.Buffer = {0, slot_count * 3, 16, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    d.native->CreateUnorderedAccessView(d.params.Get(), nullptr, &structured, at(uav_base + 2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC map_uav{glass::map_format, D3D12_UAV_DIMENSION_TEXTURE2DARRAY};
    map_uav.Texture2DArray = {0, 0, map->layers, 0};
    d.native->CreateUnorderedAccessView(map->texture.Get(), nullptr, &map_uav, at(uav_base + 3));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.native->CreateUnorderedAccessView(map->residual.Get(), nullptr, &uav, at(uav_base + 4));
    // Temporal anti-aliasing history: made when first used, again when the output size changes.
    bool temporal = temporal_aa.load(std::memory_order_relaxed) != 0 && layer < 8;
    if (temporal && (!map->history || map->history_w != out_w || map->history_h != out_h)) {
        if (map->history) { map->retired.push_back(map->history); map->retired.push_back(map->history_uv); }
        map->history = map->history_uv = nullptr;
        map->history_present.fill(0);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, out_w, out_h, UINT16(map->layers * 2), 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool made = SUCCEEDED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->history)));
        desc.Format = DXGI_FORMAT_R16G16_UNORM;
        made = made && SUCCEEDED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->history_uv)));
        if (made) { map->history_w = out_w; map->history_h = out_h; }
        else map->history = map->history_uv = nullptr;
        log("temporal_aa history %ux%u x%u views: %s (%.0f MB)", out_w, out_h, map->layers, made ? "made" : "FAILED",
            double(out_w) * out_h * map->layers * 2 * 12 / 1048576);
    }
    temporal = temporal && map->history;
    uint32_t history_info = 0, frame_index = 0;
    if (temporal) {
        // Last frame's history is used only if it is this view's from the previous frame or two (not from before the
        // option was switched off and on, a pause, or another view).
        const uint64_t now = present_count.load(std::memory_order_relaxed) + 1;
        const bool recent = map->history_present[layer] && now - map->history_present[layer] <= 3;
        map->history_parity[layer] ^= 1;
        history_info = map->history_parity[layer] | (recent ? 2u : 0u);
        frame_index = map->history_frames[layer]++;
        map->history_present[layer] = now;
    } else map->history_present.fill(0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC history_view{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2DARRAY};
    history_view.Texture2DArray = {0, 0, map->layers * 2, 0};
    d.native->CreateUnorderedAccessView(temporal ? map->history.Get() : nullptr, nullptr, &history_view, at(uav_base + 5));
    history_view.Format = DXGI_FORMAT_R16G16_UNORM;
    d.native->CreateUnorderedAccessView(temporal ? map->history_uv.Get() : nullptr, nullptr, &history_view, at(uav_base + 6));
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
    raw.Buffer = {0, 2 * slot_count * bounds_cells * bounds_cells * 4 + 64 + 128, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    d.native->CreateUnorderedAccessView(d.edge_bounds.Get(), nullptr, &raw, at(uav_base + 7));
    D3D12_UNORDERED_ACCESS_VIEW_DESC glass_view{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2DARRAY};
    glass_view.Texture2DArray = {0, 0, map->layers, 0};
    d.native->CreateUnorderedAccessView(map->glass_history.Get(), nullptr, &glass_view, at(uav_base + 8));
    glass_view.Format = DXGI_FORMAT_R16G16_UNORM;
    d.native->CreateUnorderedAccessView(map->glass_history_uv.Get(), nullptr, &glass_view, at(uav_base + 9));
    // The upscaler's depth input for the occlusion check (a null view when the check is off or the input unusable: not at
    // render size, multisampled, an array, or a depth direction the display shader doesn't tell). In VR the sim calls the
    // upscaler once per eye with that eye's depth, which matches the map layer of this call.
    ID3D12Resource* depth = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_view{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    depth_view.Texture2D.MipLevels = 1;
    const int direction = depth_direction.load(std::memory_order_relaxed);
    if (occlusion_check.load(std::memory_order_relaxed) && u.depth) {
        const auto z = u.depth->GetDesc();
        const DXGI_FORMAT format = depth_srv_format(z.Format);
        const bool usable = direction && format != DXGI_FORMAT_UNKNOWN && z.Width == render_w && z.Height == render_h &&
            z.DepthOrArraySize == 1 && z.SampleDesc.Count == 1;
        if (usable) { depth = u.depth; depth_view.Format = format; }
        static std::atomic<int> logged[2]{};
        if (logged[map->layers > 1]++ < 4) log("occlusion_check %s depth input %p %llux%u format=%u layers=%u samples=%u state=0x%x usable=%d direction=%d vr=%d", u.name, u.depth,
            z.Width, z.Height, unsigned(z.Format), unsigned(z.DepthOrArraySize), z.SampleDesc.Count, unsigned(u.depth_state), usable, direction, int(map->layers > 1));
    }
    d.native->CreateShaderResourceView(depth, &depth_view, at(uav_base + 10));

    struct { UINT map_w, map_h, out_w, out_h; float scale_x, scale_y, jitter_x, jitter_y, smoothing; UINT flags; float line_width; UINT layer; float glass; float edge_fade, edge_stretch, detail_clamp, angle_soften;
        UINT history_info, frame_index, bounds_reset, diag_index; float edge_trim; } constants{
        render_w, render_h, out_w, out_h, float(render_w) / out_w, float(render_h) / out_h,
        jitter_positive ? jitter_x : -jitter_x, jitter_positive ? jitter_y : -jitter_y, 0.25f, (tint ? 2u : 0u) | (output_readable ? 4u : 0u) | (smoothing_percent > 0 ? 8u : 0u) | (dark_screen_fix ? 16u : 0u) | (seam_fix ? 32u : 0u) | (temporal ? 64u : 0u) | (soft_smoothing ? 128u : 0u) | (stable_edges ? 256u : 0u) | (mip0_only ? 512u : 0u) | (calm_glass ? 4096u : 0u) | (depth ? 16384u : 0u) | (direction == 1 ? 32768u : 0u) | debug_flags.load(),
        smoothing_percent / 50.0f, layer, glass_percent / 100.0f, edge_fade_percent / 100.0f,
        edge_stretch_percent / 100.0f, detail_clamp_percent / 100.0f,
        angle_soften_percent / 100.0f, history_info, frame_index, bounds_reset.exchange(0), d.diag_index, edge_trim_percent / 100.0f};
    d.diag_calls[d.diag_index & 63] = {present_count.load(std::memory_order_relaxed), map->recorded_present.load(), pending, map->views.load(), output, GetCurrentThreadId()};

    auto native = state->native;
    ID3D12DescriptorHeap* heaps[]{d.heap.Get()};
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(d.root.Get());
    native->SetComputeRoot32BitConstants(0, 22, &constants, 0);
    native->SetComputeRootDescriptorTable(1, gpu);
    native->SetComputeRootUnorderedAccessView(2, d.sums->GetGPUVirtualAddress());
    // DLSS leaves its colour input compute-readable and the output as UAV; FSR reports the states it restores.
    const bool colour_moved = !(u.colour_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool output_moved = u.output_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (colour_moved) transition(native, colour, u.colour_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (output_moved) transition(native, output, u.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool depth_moved = depth && !(u.depth_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depth_moved) transition(native, depth, u.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uav_barrier(native); // the upscaler just wrote the output
    // Screen capture: the sim's raw frame, the upscaler's output and the screen map, before the fix.
    CaptureFrame capture;
    // Every capture_spacing-th fixed frame while a capture is requested (the frames then span ~0.5 s of motion).
    bool waiting = false;
    if (const uint64_t until = capture_wait_until.load(std::memory_order_relaxed); until && capture_requested.load(std::memory_order_relaxed) > 0) {
        if (colour_variants_ready() || present_count.load(std::memory_order_relaxed) >= until) {
            if (capture_wait_until.exchange(0)) log("capture starting (draw-time colour shaders %s)", colour_variants_ready() ? "ready" : "not all ready: some screens missing in draw-input");
            capture_countdown = 1; // this frame's screens were drawn before the shaders were ready
        }
        waiting = true;
    }
    const bool capturing = !waiting && capture_requested.load(std::memory_order_relaxed) > 0 && capture_countdown.fetch_sub(1) <= 0;
    if (capturing) capture_countdown = capture_spacing - 1;
    if (capturing) {
        capture = {{}, {}, u.name, render_w, render_h, out_w, out_h, layer, jitter_x, jitter_y, -1, false};
        capture_copy(d.native, native, colour, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in_format, "1-sim-raw-frame", capture);
        capture_copy(d.native, native, output, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_format, "2-upscaler-output", capture);
        if (!map->fit_input) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, render_w, render_h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
                D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&map->fit_input))))
                map->fit_input = nullptr;
        }
        constants.flags |= map->fit_input ? 65536u : 0u;
        native->SetComputeRoot32BitConstants(0, 22, &constants, 0);
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC fit_view{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D};
        d.native->CreateUnorderedAccessView(capturing ? map->fit_input.Get() : nullptr, nullptr, &fit_view, at(uav_base + 11));
    }
    const UINT timed = measuring.load(std::memory_order_relaxed) && d.timestamps && d.timed_calls < max_timed_calls ? ++d.timed_calls : 0;
    if (timed) native->EndQuery(d.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (timed - 1) * 2);
    // Frame generation: a repeated upscaler call for the frame just fixed. Its screens were cleared from the map after the
    // first call; put them back.
    const bool repeat = !fresh && map->layers == 1 && map->repeat_valid && map->repeat_output == output &&
        map->repeat_jitter_x == jitter_x && map->repeat_jitter_y == jitter_y;
    if (repeat) {
        transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_COPY_DEST);
        native->CopyResource(map->texture.Get(), map->repeat_map.Get());
        transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, map_resting);
        if (stats.repeated_calls++ == 0) log("milestone repeated %s call for the same frame (frame generation): the fix is drawn again on it", u.name);
    }
    // The map as this call uses it (a repeated call's screens are only back after the copy above).
    if (capturing) capture_copy(d.native, native, map->texture.Get(), layer /* one mip: subresource = layer */, map_resting, glass::map_format, "map", capture);
    if (!d.accumulators_zeroed) {
        native->SetPipelineState(d.passes[0].Get());
        native->Dispatch(1, 1, 1);
        uav_barrier(native);
        d.accumulators_zeroed = true;
        constants.flags |= 1;
        native->SetComputeRoot32BitConstants(0, 22, &constants, 0);
    }
    native->SetPipelineState(d.passes[1].Get());
    native->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    uav_barrier(native);
    native->SetPipelineState(d.passes[2].Get());
    native->Dispatch(1, 1, 1);
    native->SetPipelineState(d.passes[5].Get());
    native->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    uav_barrier(native);
    if (d.params_readback) {
        log_brightness(d);
        const uint64_t now = present_count.load(std::memory_order_relaxed);
        uint64_t next = d.readback_next.load();
        if (now >= next && d.readback_ready.load() == UINT64_MAX && d.readback_next.compare_exchange_strong(next, now + 600)) {
            transition(native, d.params.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            native->CopyBufferRegion(d.params_readback.Get(), 0, d.params.Get(), 0, slot_count * 3 * 16);
            transition(native, d.params.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.readback_ready = now + 30;
        }
    }
    native->SetPipelineState(d.passes[3].Get());
    native->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    if (fresh && map->layers == 1) {
        if (!map->repeat_map) {
            auto desc = map->texture->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            if (FAILED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&map->repeat_map))))
                map->repeat_map = nullptr;
        }
        if (map->repeat_map) {
            transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(native, map->repeat_map.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            native->CopyResource(map->repeat_map.Get(), map->texture.Get());
            transition(native, map->repeat_map.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, map_resting);
            map->repeat_valid = true;
            map->repeat_output = output;
            map->repeat_jitter_x = jitter_x;
            map->repeat_jitter_y = jitter_y;
        }
    }
    transition(native, map->texture.Get(), map_resting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(native);
    native->SetPipelineState(d.passes[4].Get());
    native->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    transition(native, map->texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, map_resting);
    uav_barrier(native);
    if (d.diag_readback) {
        const uint64_t now = present_count.load(std::memory_order_relaxed);
        if (d.diag_ready != UINT64_MAX && now >= d.diag_ready) {
            const uint32_t* counts = nullptr;
            const D3D12_RANGE range{0, 256};
            if (SUCCEEDED(d.diag_readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint32_t**>(&counts))))) {
                // One line per copy: the 32 calls before it, oldest first: screen pixels in the map on the GPU (0 = the fix
                // had nothing to draw), presents since the screens were recorded, views pending, output (a/b...).
                std::string line;
                std::vector<const void*> outputs;
                for (uint32_t k = 31; k >= 1; --k) {
                    const uint32_t i = (d.diag_copied_at - k) & 63;
                    const auto& c = d.diag_snapshot[i];
                    size_t o = std::find(outputs.begin(), outputs.end(), c.output) - outputs.begin();
                    if (o == outputs.size()) outputs.push_back(c.output);
                    char item[64];
                    std::snprintf(item, sizeof(item), " %u/%lld/%u%c", counts[i], (long long)(c.present - c.recorded), c.pending, char('a' + std::min<size_t>(o, 25)));
                    line += item;
                }
                const D3D12_RANGE none{};
                d.diag_readback->Unmap(0, &none);
                log("diag calls (pixels/presents since recorded/pending/output):%s", line.c_str());
            }
            d.diag_ready = UINT64_MAX;
        }
        if (d.diag_ready == UINT64_MAX && now >= d.diag_next && d.diag_dumps < 400) {
            // The copy runs after this call's passes: counters of the last 63 calls (the next one was just zeroed).
            transition(native, d.edge_bounds.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            native->CopyBufferRegion(d.diag_readback.Get(), 0, d.edge_bounds.Get(), 2 * UINT64(slot_count) * bounds_cells * bounds_cells * 16, 256);
            transition(native, d.edge_bounds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.diag_snapshot = d.diag_calls;
            d.diag_copied_at = d.diag_index + 1; // the copy holds calls up to diag_index
            d.diag_ready = now + 30;
            d.diag_next = now + (d.diag_dumps < 60 ? 120 : 1200);
            ++d.diag_dumps;
        }
    }
    ++d.diag_index;
    if (timed) {
        native->EndQuery(d.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (timed - 1) * 2 + 1);
        native->ResolveQueryData(d.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (timed - 1) * 2, 2, d.timestamps_readback.Get(), uint64_t(timed - 1) * 2 * sizeof(uint64_t));
    }
    if (capturing) {
        capture_copy(d.native, native, output, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_format, "3-after-fix", capture);
        if (map->fit_input && render_w <= map->fit_input->GetDesc().Width && render_h <= map->fit_input->GetDesc().Height)
            capture_copy(d.native, native, map->fit_input.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, DXGI_FORMAT_R16G16B16A16_FLOAT, "fit-input", capture);
        {
            std::lock_guard colour_lock(d.draw_colour_mutex);
            if (d.draw_colour && d.draw_colour->GetDesc().Width == render_w && d.draw_colour->GetDesc().Height == render_h)
                capture_copy(d.native, native, d.draw_colour.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, glass::map_format, "draw-input", capture);
        }
        // The colour match this frame used (the solve pass wrote it before the composite).
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, slot_count * 3 * 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        if (SUCCEEDED(d.native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.params)))) {
            transition(native, d.params.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            native->CopyBufferRegion(capture.params.Get(), 0, d.params.Get(), 0, slot_count * 3 * 16);
            transition(native, d.params.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        std::lock_guard capture_lock(capture_mutex);
        capture_frames.push_back(std::move(capture));
        if (capture_requested.fetch_sub(1) == 1) capture_ready_at = present_count.load() + 10;
    }
    if (colour_moved) transition(native, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, u.colour_state);
    if (output_moved) transition(native, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, u.output_state);
    if (depth_moved) transition(native, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, u.depth_state);
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
// Upscaler calls running on this thread. A mod can run one upscaler inside another (OptiScaler answers the sim's FSR
// call with DLSS): only the outermost call, the one the sim made, draws the fix, once, on its final output. The inner
// call would take a view meant for the outer one (VR: screens of the other eye drawn all over the picture).
thread_local int upscaler_depth = 0;
// DLSS Super Resolution calls carry a render size; DLSS Frame Generation calls (same evaluate function) don't.
void count_evaluation(const NVSDK_NGX_Parameter* params) {
    ++stats.evaluations;
    unsigned width = 0;
    if (params && params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width) == NVSDK_NGX_Result_Success) ++stats.dlss_upscales;
}
bool nested_upscaler() {
    if (upscaler_depth == 0) return false;
    LOG_ONCE("note an upscaler ran inside another one (e.g. OptiScaler answering the sim's FSR with DLSS): the fix is drawn once, after the outer one");
    return true;
}
template <class F, class C> NVSDK_NGX_Result evaluate_body(F original, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, C callback) {
    ++upscaler_depth;
    const auto result = original(list, handle, params, callback);
    --upscaler_depth;
    if (!upscaler_depth) count_evaluation(params);
    if (result == NVSDK_NGX_Result_Success) { if (!nested_upscaler()) composite_dlss(list, params); }
    else LOG_ONCE("note DLSS evaluate returned 0x%x (not success)", unsigned(result));
    return result;
}
// Second attachment points, used when another mod replaces the add-on's hook (see check_ngx_hooks).
Evaluate original_evaluate_again = nullptr;
EvaluateC original_evaluate_c_again = nullptr;
NVSDK_NGX_Result evaluate_again_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
    return evaluate_body(original_evaluate_again, list, handle, params, callback);
}
NVSDK_NGX_Result evaluate_c_again_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback) {
    return evaluate_body(original_evaluate_c_again, list, handle, params, callback);
}
NVSDK_NGX_Result evaluate_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
    ++upscaler_depth;
    const auto result = original_evaluate(list, handle, params, callback);
    --upscaler_depth;
    if (!upscaler_depth) count_evaluation(params);
    if (result == NVSDK_NGX_Result_Success) { if (!nested_upscaler()) composite_dlss(list, params); }
    else LOG_ONCE("note DLSS evaluate returned 0x%x (not success)", unsigned(result));
    return result;
}
NVSDK_NGX_Result evaluate_c_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback) {
    ++upscaler_depth;
    const auto result = original_evaluate_c(list, handle, params, callback);
    --upscaler_depth;
    if (!upscaler_depth) count_evaluation(params);
    if (result == NVSDK_NGX_Result_Success) { if (!nested_upscaler()) composite_dlss(list, params); }
    else LOG_ONCE("note DLSS evaluate returned 0x%x (not success)", unsigned(result));
    return result;
}
// FSR (FidelityFX API): ffxDispatch with an upscale description. The sim's own amd_fidelityfx_dx12.dll, and the
// FidelityFX files other mods bring (OptiScaler: amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll in its
// folder, used when it answers the sim's DLSS or FSR with FSR). They all export the same ffxDispatch; each gets its own
// hook. Other dispatch kinds (frame generation) pass through untouched.
constexpr int max_ffx_modules = 8;
std::array<FfxDispatch, max_ffx_modules> original_ffx_dispatch{};
std::array<HMODULE, max_ffx_modules> ffx_modules{};
int ffx_module_count = 0;
ffxReturnCode_t ffx_dispatch(FfxDispatch original, ffxContext* context, const ffxApiHeader* desc) {
    ++upscaler_depth;
    const auto result = original(context, desc);
    --upscaler_depth;
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        if (result != FFX_API_RETURN_OK) LOG_ONCE("note FSR dispatch returned 0x%x (not success)", unsigned(result));
        else if (!nested_upscaler()) { ++stats.fsr_dispatches; composite_fsr(*reinterpret_cast<const ffxDispatchDescUpscale*>(desc)); }
    }
    return result;
}
template <int I> ffxReturnCode_t ffx_dispatch_hook(ffxContext* context, const ffxApiHeader* desc) { return ffx_dispatch(original_ffx_dispatch[I], context, desc); }
template <int... I> constexpr std::array<FfxDispatch, sizeof...(I)> ffx_hooks(std::integer_sequence<int, I...>) { return {&ffx_dispatch_hook<I>...}; }
constexpr auto ffx_dispatch_hooks = ffx_hooks(std::make_integer_sequence<int, max_ffx_modules>{});
bool ffx_hooked = false; // at least one FidelityFX module hooked
std::string describe_code(const void* address);
void* follow_jumps(void* address);
void install_ffx() {
    HMODULE modules[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024) && ffx_module_count < max_ffx_modules; ++i) {
        const HMODULE module = modules[i];
        if (std::find(ffx_modules.begin(), ffx_modules.begin() + ffx_module_count, module) != ffx_modules.begin() + ffx_module_count) continue;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module, path, MAX_PATH)) continue;
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (auto& c : name) c = wchar_t(towlower(c));
        if (!name.starts_with(L"amd_fidelityfx_") || !name.ends_with(L"dx12.dll")) continue;
        const int index = ffx_module_count++;
        ffx_modules[index] = module;
        auto dispatch = GetProcAddress(module, "ffxDispatch");
        HMODULE pinned = nullptr;
        if (dispatch) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(dispatch), &pinned);
        // Another mod already hooked ffxDispatch (a jump into its own module): the fix attaches at that mod's handler and
        // leaves the export's bytes as that mod wrote them. Hooking over its jump makes a mod that checks its hook (e.g.
        // NeuralHub) hook again on top of the fix's, and the two then call each other until the stack overflows.
        void* target = reinterpret_cast<void*>(dispatch);
        if (target) {
            void* const leads = follow_jumps(target);
            HMODULE owner = nullptr;
            if (leads != target && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(leads), &owner) && owner != module) {
                log("FSR ffxDispatch in %ls is redirected by another mod: fix attached at its destination (%s)", name.c_str(), describe_code(leads).c_str());
                target = leads;
            }
        }
        const bool hooked = target && hook(target, reinterpret_cast<void*>(ffx_dispatch_hooks[index]), original_ffx_dispatch[index]);
        const auto version = file_version(path);
        log("FSR runtime found: %ls %s path=%ls hook=%d", name.c_str(), version.c_str(), path, hooked);
        if (!hooked) continue;
        // The panel shows the sim's own FSR, or the upscaler another mod runs.
        const bool sims_own = name == L"amd_fidelityfx_dx12.dll";
        if (!ffx_hooked || sims_own || name.find(L"upscaler") != std::wstring::npos) { std::lock_guard lock(info_mutex); fsr_version = version; }
        ffx_hooked = true;
    }
}

// Which module a code address belongs to, and its first bytes (diagnostics for a failed hook).
std::string describe_code(const void* address) {
    if (!address) return "missing export";
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &owner))
        GetModuleFileNameW(owner, path, MAX_PATH);
    uint8_t bytes[16]{};
    char text[160]{};
    const bool readable = read_bytes(address, bytes, sizeof(bytes));
    snprintf(text, sizeof(text), "in %s, bytes ", owner ? utf8(std::filesystem::path(path).filename().c_str()).c_str() : "no module");
    for (size_t i = 0; i < sizeof(bytes) && readable; ++i) snprintf(text + strlen(text), sizeof(text) - strlen(text), "%02x", bytes[i]);
    return text;
}

// Where a function that another mod has redirected really starts: follows jumps written over its first bytes (e.g. a
// two-byte jump back to a jump placed just before it, as the RTX 40 MFG unlock does), up to a few hops.
void* follow_jumps(void* address) {
    for (int hop = 0; hop < 8 && address; ++hop) {
        uint8_t code[14]{};
        if (!read_bytes(address, code, sizeof(code))) break;
        auto* at = static_cast<uint8_t*>(address);
        if (code[0] == 0xeb) address = at + 2 + int8_t(code[1]);                                     // jmp rel8
        else if (code[0] == 0xe9) { int32_t rel; memcpy(&rel, code + 1, 4); address = at + 5 + rel; }  // jmp rel32
        else if (code[0] == 0xff && code[1] == 0x25) {                                                 // jmp [rip + disp32]
            int32_t rel; memcpy(&rel, code + 2, 4);
            void* target = nullptr;
            if (!read_bytes(at + 6 + rel, &target, sizeof(target))) break;
            address = target;
        } else break;
    }
    return address;
}

// Hook a DLSS entry point; if another mod has redirected it so that it can't be hooked directly, hook the code it
// leads to instead (that mod's handler, which then calls DLSS itself): the fix then runs after both.
// Where each DLSS hook sits and the bytes there before and after hooking: another mod that attaches to the sim later
// (e.g. GlassOut) can overwrite them, and the DLSS calls then stop reaching the fix (see check_ngx_hooks).
struct NgxWatch {
    const char* name = nullptr;
    void* at = nullptr;
    void* detour = nullptr;
    std::array<uint8_t, 16> before{}, after{};
    bool changed = false, repaired = false;
};
NgxWatch ngx_watch[2];
template <class T> bool hook_watched(void* target, void* detour, T& original, NgxWatch& watch) {
    std::array<uint8_t, 16> before{};
    read_bytes(target, before.data(), before.size());
    if (!hook(target, detour, original)) return false;
    watch.at = target; watch.detour = detour; watch.before = before;
    read_bytes(target, watch.after.data(), watch.after.size());
    return true;
}
template <class T> bool hook_ngx(void* target, void* detour, T& original, const char* name, NgxWatch& watch) {
    watch.name = name;
    if (hook_watched(target, detour, original, watch)) return true;
    const MH_STATUS status = last_hook_status;
    void* real = follow_jumps(target);
    if (real == target) return false;
    const std::string where = describe_code(real);
    if (hook_watched(real, detour, original, watch)) {
        log("DLSS %s is redirected by another mod (%s): fix attached at its destination (%s)", name, MH_StatusToString(status), where.c_str());
        return true;
    }
    last_hook_status = status;
    return false;
}

// Called every 10 s. Logs a DLSS hook that another mod has changed; if the DLSS calls have also stopped reaching the
// fix, attaches again: the hook is written back if the original code was restored, or the fix attaches at the other
// mod's handler if it jumps there (that handler then calls DLSS; a nested call is drawn once, after the outer one).
void check_ngx_hooks(bool stopped, bool just_stopped) {
    for (int i = 0; i < 2; ++i) {
        auto& w = ngx_watch[i];
        if (!w.at) continue;
        std::array<uint8_t, 16> now{};
        if (!read_bytes(w.at, now.data(), now.size())) continue;
        if (now == w.after) {
            if (just_stopped) log("note DLSS %s hook is still in place (%s)", w.name, describe_code(w.at).c_str());
            continue;
        }
        void* leads = follow_jumps(w.at);
        if (!w.changed || just_stopped) {
            w.changed = true;
            log("WARNING DLSS %s hook was changed by another mod: now %s, leads to %s%s", w.name, describe_code(w.at).c_str(),
                describe_code(leads).c_str(), leads == w.detour ? " (still the fix)" : now == w.before ? " (original code restored)" : "");
        }
        if (!stopped || w.repaired || leads == w.detour) continue;
        w.repaired = true;
        if (now == w.before) {
            const MH_STATUS off = MH_DisableHook(w.at), on = MH_EnableHook(w.at);
            read_bytes(w.at, w.after.data(), w.after.size());
            log("DLSS %s hook written again: %s / %s, now %s", w.name, MH_StatusToString(off), MH_StatusToString(on), describe_code(w.at).c_str());
        } else if (leads != w.at) {
            const bool ok = i == 0 ? hook(leads, reinterpret_cast<void*>(&evaluate_again_hook), original_evaluate_again)
                                   : hook(leads, reinterpret_cast<void*>(&evaluate_c_again_hook), original_evaluate_c_again);
            log("DLSS %s: fix attached at the other mod's handler %s: %s", w.name, describe_code(leads).c_str(), ok ? "ok" : MH_StatusToString(last_hook_status));
        } else log("DLSS %s: no way to attach again (code changed without a jump)", w.name);
    }
}

bool ngx_hooked = false, ngx_hook_failed = false; // failed: DLSS is loaded but its evaluate could not be hooked
void install_ngx() {
    if (ngx_hooked) return;
    HMODULE module = GetModuleHandleW(L"_nvngx.dll");
    if (!module) return;
    // Another mod may be patching the same function at the same moment: retry for about a minute (called every 2 s).
    static int attempts = 0;
    ++attempts;
    auto evaluate = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto evaluate_c = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature_C");
    HMODULE pinned = nullptr;
    if (evaluate) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(evaluate), &pinned);
    const bool a = evaluate && hook_ngx(reinterpret_cast<void*>(evaluate), reinterpret_cast<void*>(&evaluate_hook), original_evaluate, "evaluate", ngx_watch[0]);
    const MH_STATUS status_a = evaluate ? last_hook_status : MH_ERROR_FUNCTION_NOT_FOUND;
    const bool b = evaluate_c && hook_ngx(reinterpret_cast<void*>(evaluate_c), reinterpret_cast<void*>(&evaluate_c_hook), original_evaluate_c, "evaluate_c", ngx_watch[1]);
    const MH_STATUS status_b = evaluate_c ? last_hook_status : MH_ERROR_FUNCTION_NOT_FOUND;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    if (!a && !b) {
        log("DLSS hook attempt %d failed: evaluate %s (%s), evaluate_c %s (%s)", attempts, MH_StatusToString(status_a),
            describe_code(reinterpret_cast<void*>(evaluate)).c_str(), MH_StatusToString(status_b), describe_code(reinterpret_cast<void*>(evaluate_c)).c_str());
        if (attempts < 30) return;
        ngx_hook_failed = true;
    }
    ngx_hooked = true;
    log("milestone DLSS runtime found: _nvngx.dll %s path=%ls hooks evaluate=%d evaluate_c=%d", file_version(path).c_str(), path, a, b);
    if (!a && !b) log("ERROR could not hook DLSS evaluate - the fix cannot run with DLSS. Another mod that hooks DLSS (e.g. a DLSS 5 add-on) may be blocking it: try without it, and send the log");
}

// DLSS answered by another mod: OptiScaler (and similar DLSS proxies) export the same evaluate function as the driver's
// _nvngx.dll and the sim's DLSS calls go to theirs. They may answer with FSR, XeSS or (on AMD/Intel GPUs) anything but
// real DLSS, which the hooks above never see. Hooking their evaluate catches the sim's call itself; when they run real
// DLSS inside, that inner call is skipped (drawn once, after the outer one). NVIDIA's own feature files export a
// function of the same name that the driver calls internally, Frame Generation's among them: left alone. They are
// nvngx_*.dll next to the game, or files the driver downloads by itself (C:\ProgramData\NVIDIA\NGX\models\...\*.bin),
// so they are recognised by name, folder and the company in their version resource.
bool nvidia_feature_file(const wchar_t* path) {
    std::wstring lower = path;
    for (auto& c : lower) c = wchar_t(towlower(c));
    const std::wstring name = std::filesystem::path(lower).filename().wstring();
    if (name == L"_nvngx.dll" || name.starts_with(L"nvngx_") || name.ends_with(L".bin") || lower.find(L"\\nvidia\\ngx\\") != std::wstring::npos)
        return true;
    std::wstring company = file_company(path);
    for (auto& c : company) c = wchar_t(towlower(c));
    return company.find(L"nvidia") != std::wstring::npos;
}
constexpr int max_ngx_proxies = 4;
std::array<Evaluate, max_ngx_proxies> original_proxy_evaluate{};
std::array<HMODULE, max_ngx_proxies> ngx_proxies{};
int ngx_proxy_count = 0;
bool ngx_proxy_hooked = false;
template <int I> NVSDK_NGX_Result proxy_evaluate_hook(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback callback) {
    ++upscaler_depth;
    const auto result = original_proxy_evaluate[I](list, handle, params, callback);
    --upscaler_depth;
    if (!upscaler_depth) count_evaluation(params);
    if (result == NVSDK_NGX_Result_Success) { if (!nested_upscaler()) composite_dlss(list, params); }
    else LOG_ONCE("note DLSS evaluate (another mod's) returned 0x%x (not success)", unsigned(result));
    return result;
}
template <int... I> constexpr std::array<Evaluate, sizeof...(I)> proxy_hooks(std::integer_sequence<int, I...>) { return {&proxy_evaluate_hook<I>...}; }
constexpr auto proxy_evaluate_hooks = proxy_hooks(std::make_integer_sequence<int, max_ngx_proxies>{});
void install_ngx_proxies() {
    HMODULE modules[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&install_ngx_proxies), &self);
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024) && ngx_proxy_count < max_ngx_proxies; ++i) {
        const HMODULE module = modules[i];
        if (module == self || std::find(ngx_proxies.begin(), ngx_proxies.begin() + ngx_proxy_count, module) != ngx_proxies.begin() + ngx_proxy_count) continue;
        auto evaluate = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (!evaluate) continue;
        static std::set<HMODULE> nvidia_files;
        if (nvidia_files.contains(module)) continue;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module, path, MAX_PATH)) continue;
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (auto& c : name) c = wchar_t(towlower(c));
        if (nvidia_feature_file(path)) {
            nvidia_files.insert(module);
            log("NVIDIA DLSS feature file left alone: %ls %s path=%ls", name.c_str(), file_version(path).c_str(), path);
            continue;
        }
        const int index = ngx_proxy_count++;
        ngx_proxies[index] = module;
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(evaluate), &pinned);
        static NgxWatch proxy_watch; // not watched: proxies are another mod's own DLSS
        const bool hooked = hook_ngx(reinterpret_cast<void*>(evaluate), reinterpret_cast<void*>(proxy_evaluate_hooks[index]), original_proxy_evaluate[index], "evaluate (another mod's)", proxy_watch);
        log("DLSS answered by another mod: %ls %s path=%ls hook=%d", name.c_str(), file_version(path).c_str(), path, hooked);
        ngx_proxy_hooked |= hooked;
    }
}

// Settings live in an .ini next to the DLL.
std::wstring settings_path() { return (addon_directory / L"glass-cockpit-dlss-fix.ini").wstring(); }
void load_settings() {
    enabled = GetPrivateProfileIntW(L"overlay", L"enabled", 1, settings_path().c_str()) != 0;
    jitter_positive = GetPrivateProfileIntW(L"overlay", L"jitter_positive", 1, settings_path().c_str()) != 0;
    extra_smoothing = GetPrivateProfileIntW(L"overlay", L"extra_smoothing", 0, settings_path().c_str()) != 0;
    smoothing_percent = std::clamp(int(GetPrivateProfileIntW(L"overlay", L"line_smoothing", default_smoothing, settings_path().c_str())), 0, smoothing_limit());
    glass_percent = std::clamp(int(GetPrivateProfileIntW(L"advanced", L"glass_strength", 100, settings_path().c_str())), 0, 100);
    detail_clamp_percent = std::clamp(int(GetPrivateProfileIntW(L"experimental", L"detail_clamp", default_detail_clamp, settings_path().c_str())), 0, 100);
    edge_fade_percent = std::clamp(int(GetPrivateProfileIntW(L"experimental", L"edge_fade", 0, settings_path().c_str())), 0, max_edge_fade);
    angle_soften_percent = std::clamp(int(GetPrivateProfileIntW(L"experimental", L"angle_softening", 0, settings_path().c_str())), 0, 100);
    edge_stretch_percent = std::clamp(int(GetPrivateProfileIntW(L"experimental", L"edge_stretch", 0, settings_path().c_str())), 0, max_edge_stretch);
    edge_trim_percent = std::clamp(int(GetPrivateProfileIntW(L"experimental", L"edge_trim", 0, settings_path().c_str())), 0, max_edge_trim);
    dark_screen_fix = GetPrivateProfileIntW(L"experimental", L"dark_screen_fix", 0, settings_path().c_str()) != 0;
    seam_fix = GetPrivateProfileIntW(L"experimental", L"seam_fix", 1, settings_path().c_str()) != 0;
    temporal_aa = GetPrivateProfileIntW(L"experimental", L"temporal_aa", 0, settings_path().c_str()) != 0;
    soft_smoothing = GetPrivateProfileIntW(L"experimental", L"soft_smoothing", 0, settings_path().c_str()) != 0;
    if (soft_smoothing) temporal_aa = 0; // one or the other
    stable_edges = GetPrivateProfileIntW(L"experimental", L"stable_edges", 1, settings_path().c_str()) != 0;
    mip0_only = GetPrivateProfileIntW(L"experimental", L"full_size_sampling", 1, settings_path().c_str()) != 0;
    calm_glass = GetPrivateProfileIntW(L"experimental", L"calm_glass", 0, settings_path().c_str()) != 0;
    occlusion_check = GetPrivateProfileIntW(L"experimental", L"occlusion_check", 1, settings_path().c_str()) != 0;
    {
        wchar_t key[64]{};
        GetPrivateProfileStringW(L"hotkeys", L"settings_window", L"", key, 64, settings_path().c_str());
        const uint32_t parsed = glass::parse_hotkey(utf8(key));
        settings_window_key = parsed && !glass::hotkey_problem(parsed) ? parsed : glass::default_settings_window_key;
    }
    // Diagnostics: identify screens the way AMD/Intel GPUs do (descriptor tracking only), also on NVIDIA.
    if (GetPrivateProfileIntW(L"debug", L"descriptor_tracking", 0, settings_path().c_str())) {
        force_tracking = true;
        log("setting descriptor_tracking=1 (screens identified by descriptor tracking only, as on AMD/Intel)");
    }
}
void save_settings() {
    WritePrivateProfileStringW(L"overlay", L"enabled", enabled ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"overlay", L"jitter_positive", jitter_positive ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"overlay", L"line_smoothing", std::to_wstring(smoothing_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"overlay", L"extra_smoothing", extra_smoothing.load() ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"advanced", L"glass_strength", std::to_wstring(glass_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"detail_clamp", std::to_wstring(detail_clamp_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"edge_fade", std::to_wstring(edge_fade_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"angle_softening", std::to_wstring(angle_soften_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"edge_stretch", std::to_wstring(edge_stretch_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"edge_trim", std::to_wstring(edge_trim_percent.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"dark_screen_fix", std::to_wstring(dark_screen_fix.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"seam_fix", std::to_wstring(seam_fix.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"temporal_aa", std::to_wstring(temporal_aa.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"soft_smoothing", std::to_wstring(soft_smoothing.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"stable_edges", std::to_wstring(stable_edges.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"full_size_sampling", mip0_only.load() ? L"1" : L"0", settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"calm_glass", std::to_wstring(calm_glass.load()).c_str(), settings_path().c_str());
    WritePrivateProfileStringW(L"experimental", L"occlusion_check", std::to_wstring(occlusion_check.load()).c_str(), settings_path().c_str());
    const auto key = glass::hotkey_name(settings_window_key);
    WritePrivateProfileStringW(L"hotkeys", L"settings_window", std::wstring(key.begin(), key.end()).c_str(), settings_path().c_str());
}
// The settings in effect, for the log (at start and when the file is re-read).
std::string settings_text() {
    char text[768];
    snprintf(text, sizeof(text), "enabled=%d jitter_positive=%d line_smoothing=%d extra_smoothing=%d glass_strength=%d detail_clamp=%d edge_fade=%d angle_softening=%d edge_stretch=%d edge_trim=%d dark_screen_fix=%d seam_fix=%d full_size_sampling=%d stable_edges=%d temporal_aa=%d soft_smoothing=%d calm_glass=%d occlusion_check=%d",
        enabled.load(), jitter_positive.load(), smoothing_percent.load(), int(extra_smoothing.load()), glass_percent.load(), detail_clamp_percent.load(),
        edge_fade_percent.load(), angle_soften_percent.load(), edge_stretch_percent.load(), edge_trim_percent.load(), dark_screen_fix.load(), seam_fix.load(), int(mip0_only.load()),
        stable_edges.load(), temporal_aa.load(), soft_smoothing.load(), calm_glass.load(), occlusion_check.load());
    return text;
}

// One measurement window every 10 s: 120 frames, reported ~30 frames later (the GPU has finished them by then).
// The offline test host has no swapchain; it calls this through GlassFixCostTick with a short window.
uint64_t cost_window = 120, cost_delay = 30, cost_spacing_ms = 10000;
void measure_cost(ID3D12CommandQueue* queue, DeviceState* d, uint64_t now) {
    static uint64_t next_window = 0, window_start = 0, report_at = 0, frames = 0;
    static LARGE_INTEGER started{}, ended{};
    const uint64_t tick = GetTickCount64();
    if (!timestamp_frequency.load(std::memory_order_relaxed)) {
        UINT64 frequency = 0;
        if (queue && SUCCEEDED(queue->GetTimestampFrequency(&frequency))) timestamp_frequency = frequency;
    }
    if (!measuring && !report_at && tick >= next_window) {
        next_window = tick + cost_spacing_ms;
        for (auto& t : cpu_ticks) t = 0;
        for (auto& c : cpu_calls) c = 0;
        if (d) { std::lock_guard lock(d->composite_mutex); d->timed_calls = 0; }
        window_start = now;
        QueryPerformanceCounter(&started);
        clock_overhead(); // calibrated here, not inside a hot event
        measuring = true;
    } else if (measuring && now - window_start >= cost_window) {
        measuring = false;
        QueryPerformanceCounter(&ended);
        frames = now - window_start;
        report_at = now + cost_delay;
    } else if (report_at && now >= report_at) {
        report_at = 0;
        LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
        const double ms_per_tick = 1000.0 / double(qpf.QuadPart), seconds = double(ended.QuadPart - started.QuadPart) / double(qpf.QuadPart);
        char cpu[512]{};
        double cpu_total = 0;
        for (int i = 0; i < cpu_slots; ++i) {
            const double ms = double(cpu_ticks[i].load()) * ms_per_tick / double(frames);
            cpu_total += ms;
            snprintf(cpu + strlen(cpu), sizeof(cpu) - strlen(cpu), " %s=%.3f(%llu)", cpu_slot_names[i], ms, cpu_calls[i].load() / frames);
        }
        char gpu[160] = "gpu=unavailable";
        const uint64_t frequency = timestamp_frequency.load();
        if (d && d->timestamps_readback && frequency) {
            std::lock_guard lock(d->composite_mutex);
            const UINT calls = d->timed_calls;
            uint64_t* stamps = nullptr;
            const D3D12_RANGE range{0, SIZE_T(calls) * 2 * sizeof(uint64_t)};
            if (calls && SUCCEEDED(d->timestamps_readback->Map(0, &range, reinterpret_cast<void**>(&stamps)))) {
                double sum = 0, worst = 0; UINT valid = 0;
                for (UINT i = 0; i < calls; ++i) {
                    if (stamps[i * 2 + 1] <= stamps[i * 2]) continue;
                    const double ms = double(stamps[i * 2 + 1] - stamps[i * 2]) * 1000.0 / double(frequency);
                    if (ms > 100) continue; // not finished or garbage
                    sum += ms; worst = std::max(worst, ms); ++valid;
                }
                const D3D12_RANGE none{0, 0};
                d->timestamps_readback->Unmap(0, &none);
                if (valid) snprintf(gpu, sizeof(gpu), "gpu_ms_per_frame=%.3f (per upscaler call avg=%.3f max=%.3f, %u calls)", sum / double(frames), sum / valid, worst, valid);
                else snprintf(gpu, sizeof(gpu), "gpu=no finished samples (%u calls)", calls);
            } else if (!calls) snprintf(gpu, sizeof(gpu), "gpu=no upscaler calls drawn");
        }
        log("cost frames=%llu fps=%.1f cpu_ms_per_frame=%.3f [%s ] %s", frames, seconds > 0 ? double(frames) / seconds : 0.0, cpu_total, cpu + 1, gpu);
    }
}

// Copies the frame about to be shown into a readback buffer (only while a capture is running), on the queue the sim
// presents with.
void capture_present(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue) {
    static uint64_t composites_seen = 0;
    const uint64_t composites = stats.composites.load();
    const bool real = composites != composites_seen;
    composites_seen = composites;
    if (capture_presents_left.load() <= 0) return;
    Internal internal;
    ComPtr<IDXGISwapChain3> swapchain3;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!queue || FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
        FAILED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&resource))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        capture_presents_left = 0; return;
    }
    auto format = resource->GetDesc().Format;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    default: break;
    }
    const int index = capture_present_count - capture_presents_left.load();
    CaptureFrame frame;
    frame.presented = index; frame.real = real;
    capture_copy(device.Get(), list.Get(), resource.Get(), 0, D3D12_RESOURCE_STATE_PRESENT, format, "presented", frame);
    if (FAILED(list->Close())) { capture_presents_left = 0; return; }
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    frame.allocator = allocator; frame.list = list; // kept until the copy is read back, by when the GPU has long finished it
    std::lock_guard lock(capture_mutex);
    capture_frames.push_back(std::move(frame));
    --capture_presents_left;
}

// The frame clock: one tick per frame the sim presents. Without presents (the swapchain hooks could not be installed,
// or the offline test host, which has no swapchain) it is driven by the background thread instead.
std::mutex frame_mutex;
std::atomic<uint64_t> last_present_tick{0};
void frame_tick(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue) {
    std::lock_guard lock(frame_mutex);
    const auto now = ++present_count;
    if (swapchain) capture_present(swapchain, queue);
    else capture_presents_left = 0; // no presents to capture: the capture finishes with the upscaler frames alone
    capture_flush(now);
    measure_cost(queue, main_state, now);
    pixel_probe_resolve(queue, now);
    pixel_probe_flush(now);
}

// The swapchain the sim presents with (its queue is on the sim's device). Presents of other swapchains are not frames.
std::mutex swapchains_mutex;
IDXGISwapChain* main_swapchain = nullptr;
HWND main_window = nullptr; // the window of the sim's swapchain (pop-out windows have their own), and its size
uint64_t main_area = 0;
ComPtr<ID3D12CommandQueue> main_queue;
void install_present_hooks(IDXGISwapChain* swapchain);
void swapchain_created(IUnknown* device, IUnknown* created) noexcept try {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain> swapchain;
    if (!device || !created || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || FAILED(created->QueryInterface(IID_PPV_ARGS(&swapchain)))) return;
    // Pop-out windows (a screen or panel dragged out of the sim into a window of its own) get swapchains too: the frame
    // clock and captures stay with the sim's main window (a new swapchain for it, e.g. after a resolution change,
    // replaces the old one). Another window takes over only if it's larger (pop-outs are smaller than the main view).
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    const uint64_t area = uint64_t(desc.BufferDesc.Width) * desc.BufferDesc.Height;
    {
        std::lock_guard lock(swapchains_mutex);
        if (main_window && desc.OutputWindow && desc.OutputWindow != main_window && area <= main_area) {
            log("note swapchain %p %ux%u is for another window (e.g. a pop-out): not followed", created, desc.BufferDesc.Width, desc.BufferDesc.Height);
            return;
        }
    }
    install_present_hooks(swapchain.Get());
    ComPtr<ID3D12Device> queue_device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device)))) return;
    if (!main_device) {
        log("WARNING the sim's device was created in a way the fix did not see; found it through its swapchain. What the sim set up before is unknown, so screens will most likely stay unfixed: please report this with your log");
        device_created(queue_device.Get());
    }
    // Under another mod's wrapper (e.g. ReShade loaded for its effects) the queue may be that mod's, on its own device
    // object: the swapchain is still the sim's if it is on the sim's GPU. Only a queue of the sim's own device is used
    // for the fix's own work (GPU cost, presented frames in captures).
    const bool own_queue = queue_device.Get() == main_device;
    if (!own_queue && (!main_device || !same_adapter(queue_device.Get(), main_device))) {
        log("note swapchain %p is on another device than the sim's: not followed", created);
        return;
    }
    if (!own_queue) log("note the sim's swapchain was created through another mod's wrapper: GPU cost and presented frames in captures are unavailable");
    std::lock_guard lock(swapchains_mutex);
    if (desc.OutputWindow) main_window = desc.OutputWindow, main_area = area;
    main_swapchain = swapchain.Get();
    main_queue = own_queue ? queue : nullptr;
    log("swapchain %p %ux%u format=%u buffers=%u", swapchain.Get(), desc.BufferDesc.Width, desc.BufferDesc.Height, unsigned(desc.BufferDesc.Format), desc.BufferCount);
} catch (...) {}
void presented(IDXGISwapChain* swapchain) noexcept try {
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard lock(swapchains_mutex);
        if (main_swapchain && swapchain != main_swapchain) return;
        queue = main_queue;
    }
    last_present_tick = GetTickCount64();
    frame_tick(swapchain, queue.Get());
} catch (...) {}

// Settings: the .ini next to the DLL, re-read when it changes. Written at start with every setting and the value in
// effect, so that settings an update adds show up in it too (what each one does is in the README).
void write_settings_file() {
    const std::filesystem::path path = settings_path();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        std::ofstream file(path);
        file << "; " GLASS_NAME " settings. Saved changes apply while the sim runs (within a second). The ReShade add-on\n"
                "; also has them in its tab in the ReShade overlay. What each setting does: " GLASS_WEBSITE "#settings\n"
                "; Hotkeys in the sim: Ctrl+Shift+F9 fix on/off, Ctrl+Shift+F11 green tint on every fixed screen.\n"
                "; Standalone: settings window with [hotkeys] settings_window (Ctrl+Shift+F6).\n"
                "; What the fix is doing: glass-cockpit-dlss-fix-status.txt.\n"
                "\n";
    }
    save_settings();
}
// The file is re-read by the background thread when its time changes; settings changed in the add-on's overlay tab (or
// by a hotkey) are saved under the same lock, and the new time remembered, so they aren't taken for an edit of the file.
std::mutex settings_mutex;
FILETIME settings_written{};
bool settings_changed() { // under settings_mutex
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(settings_path().c_str(), GetFileExInfoStandard, &data) || CompareFileTime(&data.ftLastWriteTime, &settings_written) == 0) return false;
    settings_written = data.ftLastWriteTime;
    return true;
}
void reload_settings_locked() { // under settings_mutex
    if (!settings_changed()) return;
    const bool was_enabled = enabled;
    load_settings();
    if (enabled && !was_enabled) ++generation; // lists recorded while it was off have stale binding state
    log("settings reloaded: %s", settings_text().c_str());
}
template <class Change> void change_settings(Change&& change) {
    std::lock_guard lock(settings_mutex);
    // The file may have been saved since the last look (by another settings app, or Notepad): take that in first, or
    // saving the values in memory would undo it.
    reload_settings_locked();
    change();
    save_settings();
    settings_changed();
}
void reload_settings() {
    std::lock_guard lock(settings_mutex);
    reload_settings_locked();
}

// Hotkeys (Ctrl+Shift+...), polled about 30 times a second while one of the sim's windows has the focus.
void poll_hotkeys() {
    // Only these two: other add-ons use Ctrl+Shift+F-keys too (GSX opens its menu with Ctrl+Shift+F12), so the capture
    // and the debug tools are buttons in the Bug reports section instead.
    constexpr std::array<int, 2> keys{VK_F9, VK_F11};
    static std::array<bool, keys.size()> was_down{};
    DWORD process = 0;
    if (const HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &process);
    const bool chord = process == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    for (size_t i = 0; i < keys.size(); ++i) {
        const bool down = chord && (GetAsyncKeyState(keys[i]) & 0x8000);
        if (down && !was_down[i]) {
            if (keys[i] == VK_F9) glass::set_enabled(!enabled);
            else glass::set_tint(!tint);
        }
        was_down[i] = down;
    }
    // The settings window's hotkey (standalone; changeable): exactly its modifiers, so Ctrl+Shift+F6 doesn't also fire
    // a plain Shift+F6. Not while the window is recording a new hotkey.
    static bool window_key_was_down = false;
    const uint32_t key = settings_window_key.load();
    const auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool window_key_down = process == GetCurrentProcessId() && !glass::settings_window_capturing.load() &&
        held(VK_CONTROL) == ((key & glass::key_ctrl) != 0) && held(VK_SHIFT) == ((key & glass::key_shift) != 0) &&
        held(VK_MENU) == ((key & glass::key_alt) != 0) && held(int(key & 0xFF));
    if (window_key_down && !window_key_was_down && glass::settings_window_hotkey) glass::settings_window_hotkey();
    window_key_was_down = window_key_down;
}

// Loaded after the sim created its device (e.g. by an ASI loader rather than as dxgi.dll): D3D12 hands back an
// adapter's existing device instead of making a new one. Views, root signatures and pipelines made before are missed.
bool loaded_late = false;
void find_late_device() {
    static bool tried = false;
    if (!loaded_late || tried || main_device) return;
    tried = true;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); // goes through the device hook
    // What the sim set up before (its descriptor heaps, render target views) stays unknown, so screens most likely can't
    // be fixed; the device is still followed so that the log shows what does work.
    log(main_device ? "WARNING loaded after the sim created its graphics device: the fix can't see what the sim set up before, so screens will most likely stay unfixed. Install the fix as dxgi.dll (or version.dll or winmm.dll) next to FlightSimulator2024.exe"
                    : "WARNING loaded after the sim created its graphics device, which could not be found: install the fix as dxgi.dll (or version.dll or winmm.dll) next to FlightSimulator2024.exe");
}

std::mutex install_mutex;
void install_upscaler_hooks() {
    std::lock_guard lock(install_mutex);
    install_ngx();
    install_ngx_proxies();
    install_ffx();
}

// Every 10 s: what is missing, in plain words (status file and log), and the counters when they changed.
void periodic_checks(uint64_t now) {
    scan_modules();
    if (!reshade_addon && !duplicate_fix && GetModuleHandleW(L"glass-cockpit-dlss-fix.addon64")) {
        duplicate_fix = true;
        log("WARNING the ReShade add-on version of this fix (glass-cockpit-dlss-fix.addon64) is installed too: this copy stays off. Remove one of them");
    }
    // A second copy of the standalone (under another of its names) stays off by itself (start); say so once.
    static bool other_copy_logged = false;
    if (!reshade_addon && !other_copy_logged) {
        HMODULE self = nullptr, modules[1024];
        DWORD bytes = 0;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&periodic_checks), &self);
        if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
            for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) {
                if (modules[i] == self || !GetProcAddress(modules[i], "GlassFixSettingsWindow")) continue;
                wchar_t other[MAX_PATH]{};
                GetModuleFileNameW(modules[i], other, MAX_PATH);
                log("WARNING the standalone version of this fix is installed twice (also as %ls): only the first one loaded runs. Remove the other one", other);
                other_copy_logged = true;
                break;
            }
    }
    {
        // DLSS calls that stop reaching the fix while the sim keeps drawing cockpit screens: DLSS turned off in the sim,
        // or another mod took over the DLSS call.
        static uint64_t last_evaluations = 0, last_draws = 0, last_fsr = 0;
        static bool stopped = false;
        const uint64_t evaluations = stats.evaluations.load(), draws = stats.display_draws.load(), fsr = stats.fsr_dispatches.load();
        const bool now_stopped = ngx_hooked && last_evaluations && evaluations == last_evaluations && fsr == last_fsr && draws > last_draws;
        if (now_stopped && !stopped)
            log("WARNING DLSS calls stopped reaching the fix while the sim still draws cockpit screens (DLSS turned off in the sim, or another mod took over the DLSS call)");
        if (stopped && evaluations != last_evaluations) log("note DLSS calls reach the fix again");
        check_ngx_hooks(now_stopped, now_stopped && !stopped);
        stopped = now_stopped;
        last_evaluations = evaluations; last_draws = draws; last_fsr = fsr;
    }
    if (main_device) {
        const HRESULT removed = main_device->GetDeviceRemovedReason();
        if (removed != S_OK) LOG_ONCE("ERROR graphics device removed, reason=0x%08lx (0x887a0005 = hung/TDR, 0x887a0006 = driver error, 0x887a0001 = invalid call, 0x887a0020 = driver internal)", static_cast<unsigned long>(removed));
    }
    const auto minutes = (now - init_tick) / 60000.0;
    const char* found = nullptr;
    if (duplicate_fix) found = "The ReShade add-on version of this fix (glass-cockpit-dlss-fix.addon64) is installed too, so this copy stays off. Remove one of them.";
    if (!main_device && minutes > 2 && !found) {
        if (reshade_addon) {
            LOG_ONCE("WARNING no Direct3D 12 device reported by ReShade after 2 minutes: is ReShade installed for Direct3D 12?");
            found = "ReShade has not reported the sim's graphics device. Install ReShade (with add-on support) for Direct3D 12.";
        } else {
            LOG_ONCE("WARNING no Direct3D 12 device seen after 2 minutes: is the fix installed as dxgi.dll in the folder of FlightSimulator2024.exe?");
            found = "The sim's graphics device was not seen. Install the fix as dxgi.dll in the folder of FlightSimulator2024.exe.";
        }
    }
    if (!ngx_hooked && !ngx_proxy_hooked && !ffx_hooked && minutes > 2 && !found) {
        LOG_ONCE("WARNING neither DLSS (_nvngx.dll) nor FSR (amd_fidelityfx_dx12.dll) loaded after 2 minutes: is DLSS or FSR selected in the sim's graphics settings?");
        found = "No upscaler loaded. Select DLSS or FSR in the sim's graphics settings.";
    }
    if (!stats.display_pipelines && minutes > 5) {
        LOG_ONCE("WARNING no cockpit display shader seen after 5 minutes: not in a glass cockpit yet, or a sim update changed the shader (fix needs an update)");
        if (!found) found = "No cockpit display shader seen yet. Load a flight with a glass cockpit. If you are in one, a sim update may have changed the shader and the fix needs an update.";
    }
    if (ngx_hook_failed && !stats.fsr_dispatches && !found)
        found = "DLSS is loaded, but the fix could not attach to it. Another mod that hooks DLSS (e.g. a DLSS 5 add-on) may be blocking it: try without it, and send your log.";
    static uint64_t display_since = 0;
    if (stats.display_draws && !display_since) display_since = now;
    if (display_since && now - display_since > 20000 && !found) {
        if (!stats.dlss_upscales && !stats.fsr_dispatches && stats.evaluations) {
            LOG_ONCE("WARNING only DLSS Frame Generation is running, no DLSS or FSR upscaling: select DLSS or FSR as anti-aliasing in the sim's graphics settings");
            found = "Only DLSS Frame Generation is running. Set Anti-Aliasing to DLSS (any mode, DLAA too) or FSR in the sim's graphics settings; Frame Generation can stay on.";
        } else if (!stats.dlss_upscales && !stats.fsr_dispatches) {
            LOG_ONCE("WARNING cockpit screens are drawn but neither DLSS nor FSR is called: select DLSS or FSR in the sim's graphics settings");
            found = "Cockpit screens are drawn but neither DLSS nor FSR is running. Select DLSS or FSR in the sim's graphics settings.";
        } else if (!stats.replays) {
            LOG_ONCE("WARNING cockpit screens are drawn but none could be recorded: see skip reasons above");
            found = "Cockpit screens are drawn but could not be recorded. Please report this with your log files.";
        } else if (!stats.composites && enabled) {
            LOG_ONCE("WARNING screens recorded and the upscaler running, but no overlay frame yet: see skip reasons above");
            found = "The upscaler is running and screens are recorded, but the fix could not be drawn yet. Please report this with your log files.";
        }
    }
    if (stats.composites && !duplicate_fix) found = nullptr;
    problem = found;
    // Stats only when something changed, to keep the log small.
    static std::array<uint64_t, 6> last{};
    const std::array<uint64_t, 6> current{stats.replays.load(), stats.composites.load(), stats.composite_skipped.load(), stats.unknown_texture.load(),
        stats.unsupported_draw.load(), uint64_t(enabled.load())};
    if (current != last) {
        last = current;
        log("stats overlay=%d jitter_positive=%d dlss_calls=%llu dlss_upscales=%llu fsr_calls=%llu screen_draws=%llu recorded=%llu overlay_frames=%llu skipped=%llu shader_pending=%llu unknown_texture=%llu unsupported=%llu",
            enabled.load(), jitter_positive.load(), stats.evaluations.load(), stats.dlss_upscales.load(), stats.fsr_dispatches.load(), stats.display_draws.load(), stats.replays.load(), stats.composites.load(),
            stats.composite_skipped.load(), stats.variant_pending.load(), stats.unknown_texture.load(), stats.unsupported_draw.load());
    }
}

// The status as a text file next to the log, rewritten when it changes (the add-on also shows it in its overlay tab).
void write_status_file() {
    static std::string last;
    const auto text = glass::status_text(glass::status());
    if (text == last) return;
    last = text;
    std::ofstream(addon_directory / L"glass-cockpit-dlss-fix-status.txt") << text;
}

bool fixture_host = false; // the offline test host (no swapchain: it ticks the frame clock itself)
void housekeeping() {
    uint64_t next_settings = 0, next_install = 0, next_check = 0;
    for (;;) {
        Sleep(33);
        const uint64_t now = GetTickCount64();
        poll_hotkeys();
        // No presents for a second: keep the frame clock going (about 60 ticks a second).
        if (!fixture_host && now - last_present_tick.load() > 1000) { frame_tick(nullptr, nullptr); frame_tick(nullptr, nullptr); }
        if (now >= next_settings) { next_settings = now + 1000; reload_settings(); }
        // (The offline test host loads its stand-in upscalers and patches them itself before creating its device.)
        if (now >= next_install && !fixture_host) { next_install = now + 2000; find_late_device(); install_upscaler_hooks(); }
        if (now >= next_check) { next_check = now + 10000; periodic_checks(now); write_status_file(); }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The hook functions and their installation.
template <class T> bool hook_slot(void* object, size_t slot, void* detour, T& original) {
    return hook((*reinterpret_cast<void***>(object))[slot], detour, original);
}

// Device creation (d3d12.dll export).
using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDevice original_create_device = nullptr;
void hook_swapchain_creation();
HRESULT WINAPI create_device_hook(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device) {
    const HRESULT hr = original_create_device(adapter, level, iid, device);
    if (SUCCEEDED(hr) && device && *device) {
        ComPtr<ID3D12Device> created;
        if (SUCCEEDED(static_cast<IUnknown*>(*device)->QueryInterface(IID_PPV_ARGS(&created)))) device_created(created.Get());
        hook_swapchain_creation();
    }
    return hr;
}

// Device methods.
using CreateRootSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using CreateSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateUav = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRtv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using CreateCommandList1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using CreateCommandSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
CreateRootSignature original_create_root_signature = nullptr;
CreateSrv original_create_srv = nullptr;
CreateUav original_create_uav = nullptr;
CreateRtv original_create_rtv = nullptr;
CreateCommandList original_create_command_list = nullptr;
CreateCommandList1 original_create_command_list1 = nullptr;
CreateCommandSignature original_create_command_signature = nullptr;
HRESULT STDMETHODCALLTYPE create_root_signature_hook(ID3D12Device* device, UINT mask, const void* blob, SIZE_T size, REFIID iid, void** out) {
    const HRESULT hr = original_create_root_signature(device, mask, blob, size, iid, out);
    if (SUCCEEDED(hr) && !passthrough() && device == main_device && blob && out && *out) {
        ComPtr<ID3D12RootSignature> root;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&root)))) root_signature_created(blob, size, root.Get());
    }
    return hr;
}
void STDMETHODCALLTYPE create_srv_hook(ID3D12Device* device, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    original_create_srv(device, resource, desc, handle);
    if (!passthrough() && device == main_device) view_created(resource, desc, handle);
}
void STDMETHODCALLTYPE create_uav_hook(ID3D12Device* device, ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    original_create_uav(device, resource, counter, desc, handle);
    if (!passthrough() && device == main_device && handle.ptr && track_descriptors.load(std::memory_order_relaxed)) set_tracked(uintptr_t(handle.ptr), 0); // a new view replaces what was there
}
void STDMETHODCALLTYPE create_rtv_hook(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    original_create_rtv(device, resource, desc, handle);
    if (passthrough() || device != main_device) return;
    std::unique_lock lock(rtv_mutex);
    rtv_resources[handle.ptr] = resource;
}
// The vtable of the fix's own command list, through which the list functions were hooked (install_d3d12_hooks).
void** hooked_list_vtable = nullptr;
void install_list_variants(ID3D12GraphicsCommandList* list) noexcept;
void list_vtable_seen(ID3D12GraphicsCommandList* list, const char* where) noexcept;
// Diagnostics: whether a new command list runs the functions the fix hooked. A list class with its own functions (e.g.
// another D3D12 runtime version) would never reach the hooks. Logged once per distinct vtable.
void check_list_functions(ID3D12GraphicsCommandList* list, ID3D12Device* device, const char* created_by) {
    void** const vtable = *reinterpret_cast<void***>(list);
    {
        static std::mutex mutex;
        static std::vector<void**> seen;
        std::lock_guard lock(mutex);
        if (std::find(seen.begin(), seen.end(), vtable) != seen.end() || seen.size() >= 8) return;
        seen.push_back(vtable);
    }
    const std::pair<const char*, size_t> slots[]{{"draw_indexed", list_draw_indexed_slot()}, {"set_pipeline_state", list_set_pipeline_state_slot()},
        {"set_root_table", list_set_graphics_root_table_slot()}, {"set_render_targets", list_set_render_targets_slot()}};
    int same = 0;
    for (const auto& [name, slot] : slots) same += hooked_list_vtable && vtable[slot] == hooked_list_vtable[slot];
    log("command_list vtable=%p hooked_vtable=%p type=%d created_by=%s sim_device=%d hooked_functions=%d/4 vtable_%s", vtable, hooked_list_vtable,
        int(list->GetType()), created_by, int(device == main_device), same, describe_code(vtable).c_str());
    install_list_variants(list); // a list class of its own (direct lists only)
    if (same == 4 || !hooked_list_vtable) return;
    for (const auto& [name, slot] : slots)
        if (vtable[slot] != hooked_list_vtable[slot])
            log("  %s differs: list's %s, hooked %s", name, describe_code(vtable[slot]).c_str(), describe_code(hooked_list_vtable[slot]).c_str());
}
// Diagnostics: the interface the sim asked for, and for its first direct lists every command list version's vtable
// (a version on another vtable may run other functions than the hooked ones). "hooked" = the function the fix hooked.
std::string code_bytes(const void* address) {
    uint8_t bytes[32]{};
    if (!address || !read_bytes(address, bytes, sizeof(bytes))) return "unreadable";
    std::string text;
    char byte[3];
    for (uint8_t b : bytes) snprintf(byte, sizeof(byte), "%02x", b), text += byte;
    return text;
}
void probe_list_interfaces(IUnknown* object, ID3D12GraphicsCommandList* list, REFIID iid, const char* created_by) {
    static const IID* const versions[]{&__uuidof(ID3D12GraphicsCommandList), &__uuidof(ID3D12GraphicsCommandList1), &__uuidof(ID3D12GraphicsCommandList2),
        &__uuidof(ID3D12GraphicsCommandList3), &__uuidof(ID3D12GraphicsCommandList4), &__uuidof(ID3D12GraphicsCommandList5), &__uuidof(ID3D12GraphicsCommandList6),
        &__uuidof(ID3D12GraphicsCommandList7), &__uuidof(ID3D12GraphicsCommandList8), &__uuidof(ID3D12GraphicsCommandList9), &__uuidof(ID3D12GraphicsCommandList10)};
    static std::atomic<int> lists{0}, direct{0};
    const int n = lists++;
    if (n >= 16) return;
    const auto type = list->GetType();
    int requested = -1;
    for (int v = 0; v < 11; ++v) if (iid == *versions[v]) requested = v;
    log("command_list #%d type=%d created_by=%s requested=v%d out=%+td vtable=%p", n, int(type), created_by, requested,
        reinterpret_cast<char*>(object) - reinterpret_cast<char*>(list), *reinterpret_cast<void***>(object));
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT || direct++ >= 2 || !hooked_list_vtable) return;
    const std::pair<const char*, size_t> slots[]{{"draw_indexed", list_draw_indexed_slot()}, {"set_pipeline_state", list_set_pipeline_state_slot()},
        {"set_root_table", list_set_graphics_root_table_slot()}, {"set_render_targets", list_set_render_targets_slot()}};
    for (int v = 0; v < 11; ++v) {
        ComPtr<IUnknown> version;
        if (FAILED(object->QueryInterface(*versions[v], reinterpret_cast<void**>(version.GetAddressOf())))) { log("  v%d not supported", v); continue; }
        void** const vtable = *reinterpret_cast<void***>(version.Get());
        std::string text;
        for (const auto& [name, slot] : slots) text += std::string(" ") + name + "=" + (vtable[slot] == hooked_list_vtable[slot] ? "hooked" : code_bytes(vtable[slot]));
        log("  v%d offset=%+td vtable=%p%s", v, reinterpret_cast<char*>(version.Get()) - reinterpret_cast<char*>(list), vtable, text.c_str());
    }
}
void command_list_created(ID3D12Device* device, void** out, ID3D12PipelineState* initial, const char* created_by, REFIID iid) {
    if (passthrough() || !out || !*out) return;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list)))) {
        probe_list_interfaces(static_cast<IUnknown*>(*out), list.Get(), iid, created_by);
        check_list_functions(list.Get(), device, created_by);
        list_created(list.Get(), device, initial);
    }
}
HRESULT STDMETHODCALLTYPE create_command_list_hook(ID3D12Device* device, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initial, REFIID iid, void** out) {
    const HRESULT hr = original_create_command_list(device, mask, type, allocator, initial, iid, out);
    if (SUCCEEDED(hr)) command_list_created(device, out, initial, "CreateCommandList", iid);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_command_list1_hook(ID3D12Device4* device, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** out) {
    const HRESULT hr = original_create_command_list1(device, mask, type, flags, iid, out);
    if (SUCCEEDED(hr)) command_list_created(device, out, nullptr, "CreateCommandList1", iid); // created closed: Reset comes before recording
    return hr;
}
HRESULT STDMETHODCALLTYPE create_command_signature_hook(ID3D12Device* device, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* root, REFIID iid,
    void** out) {
    const HRESULT hr = original_create_command_signature(device, desc, root, iid, out);
    if (SUCCEEDED(hr) && !passthrough() && desc && out && *out) {
        ComPtr<ID3D12CommandSignature> signature;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&signature)))) command_signature_created(*desc, signature.Get());
    }
    return hr;
}

// Command list methods. Draws and copies are seen before they run; bindings after.
using ListReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using ListClearState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using ListDraw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using ListDrawIndexed = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using ListCopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using ListSetPipelineState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using ListSetRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using ListSetRootTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using ListSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ListBeginRenderPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using ListEndRenderPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
using ListSetScissorRects = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using ListExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
ListReset original_reset = nullptr;
// The offset of the fix's own list's ID3D12GraphicsCommandList4 interface from its ID3D12GraphicsCommandList one (0
// unless the runtime gives them vtables of their own), and that list's vtables: the functions the main hooks went on.
ptrdiff_t hooked_list4_offset = 0;
void** hooked_list4_vtable = nullptr;
ID3D12GraphicsCommandList* base_list(void* self, ptrdiff_t offset) { return reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<char*>(self) - offset); }
ListClearState original_clear_state = nullptr;
ListDraw original_draw = nullptr;
ListDrawIndexed original_draw_indexed = nullptr;
ListCopyTextureRegion original_copy_texture_region = nullptr;
ListSetPipelineState original_set_pipeline_state = nullptr;
ListSetRootSignature original_set_root_signature = nullptr;
ListSetRootTable original_set_root_table = nullptr;
ListSetRenderTargets original_set_render_targets = nullptr;
ListBeginRenderPass original_begin_render_pass = nullptr;
ListEndRenderPass original_end_render_pass = nullptr;
ListSetScissorRects original_set_scissor_rects = nullptr;
ListExecuteIndirect original_execute_indirect = nullptr;
HRESULT STDMETHODCALLTYPE reset_hook(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) {
    const HRESULT hr = original_reset(list, allocator, initial);
    if (SUCCEEDED(hr) && !passthrough()) { list_vtable_seen(list, "Reset"); list_reset(list, initial); }
    return hr;
}
void STDMETHODCALLTYPE clear_state_hook(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    original_clear_state(list, pipeline);
    if (!passthrough()) pipeline_bound(list, pipeline);
}
// A cockpit screen's draw is issued by the fix itself, followed by its map draw (draw_recorded returns true then).
void STDMETHODCALLTYPE draw_hook(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT first, UINT first_instance) {
    if (!passthrough() && draw_recorded(list, {false, count, instances, first, first_instance, 0})) return;
    original_draw(list, count, instances, first, first_instance);
}
void STDMETHODCALLTYPE draw_indexed_hook(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT first, INT vertex, UINT first_instance) {
    if (!passthrough() && draw_recorded(list, {true, count, instances, first, first_instance, vertex})) return;
    original_draw_indexed(list, count, instances, first, vertex, first_instance);
}
void STDMETHODCALLTYPE execute_indirect_hook(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT max_count, ID3D12Resource* arguments,
    UINT64 arguments_offset, ID3D12Resource* count, UINT64 count_offset) {
    if (!passthrough()) indirect_recorded(list, signature, max_count);
    original_execute_indirect(list, signature, max_count, arguments, arguments_offset, count, count_offset);
}
void STDMETHODCALLTYPE copy_texture_region_hook(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dest, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
    if (!passthrough() && dest && source && dest->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && source->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        texture_copied(source->pResource, dest->pResource, dest->SubresourceIndex);
    original_copy_texture_region(list, dest, x, y, z, source, box);
}
void STDMETHODCALLTYPE set_pipeline_state_hook(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    original_set_pipeline_state(list, pipeline);
    if (!passthrough()) pipeline_bound(list, pipeline);
}
void STDMETHODCALLTYPE set_root_signature_hook(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    original_set_root_signature(list, root);
    if (!passthrough()) root_signature_bound(list, root);
}
void STDMETHODCALLTYPE set_root_table_hook(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    original_set_root_table(list, index, table);
    if (!passthrough()) table_bound(list, index, table);
}
void STDMETHODCALLTYPE set_render_targets_hook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    original_set_render_targets(list, count, targets, single_range, depth);
    if (!passthrough()) targets_bound(list, count, targets, single_range, depth);
}
void STDMETHODCALLTYPE set_scissor_rects_hook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RECT* rects) {
    original_set_scissor_rects(list, count, rects);
    if (!passthrough()) scissors_bound(list, count, rects);
}
void STDMETHODCALLTYPE begin_render_pass_hook(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    if (!passthrough()) pass_begun(base_list(list, hooked_list4_offset), count, targets);
    original_begin_render_pass(list, count, targets, depth, flags);
}
void STDMETHODCALLTYPE end_render_pass_hook(ID3D12GraphicsCommandList4* list) {
    if (!passthrough()) pass_ended(base_list(list, hooked_list4_offset));
    original_end_render_pass(list);
}

// Command list functions reached through another interface version's vtable. Some D3D12 runtimes (Windows 11 26H1)
// give a list's interfaces vtables of their own, with functions the hooks above never see: the sim's drawing then
// bypasses the fix completely. Those functions are hooked as well (up to 4 per function). Their `this` is that
// interface, moved back by its offset to the list's ID3D12GraphicsCommandList pointer for the fix's bookkeeping. The
// runtime's function runs as an internal call: if it goes on into one of the main hooks, that one passes it through.
constexpr int max_variants = 4;
template <class F> struct Variants {
    std::array<F, max_variants> original{};
    std::array<ptrdiff_t, max_variants> offset{};
    std::array<void*, max_variants> target{};
    int count = 0;
};
Variants<ListReset> reset_variants;
Variants<ListClearState> clear_state_variants;
Variants<ListDraw> draw_variants;
Variants<ListDrawIndexed> draw_indexed_variants;
Variants<ListCopyTextureRegion> copy_texture_region_variants;
Variants<ListSetPipelineState> set_pipeline_state_variants;
Variants<ListSetRootSignature> set_root_signature_variants;
Variants<ListSetRootTable> set_root_table_variants;
Variants<ListSetRenderTargets> set_render_targets_variants;
Variants<ListSetScissorRects> set_scissor_rects_variants;
Variants<ListExecuteIndirect> execute_indirect_variants;
Variants<ListBeginRenderPass> begin_render_pass_variants;
Variants<ListEndRenderPass> end_render_pass_variants;
template <int V> HRESULT STDMETHODCALLTYPE reset_variant(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) {
    HRESULT hr;
    { Internal internal; hr = reset_variants.original[V](self, allocator, initial); }
    if (SUCCEEDED(hr) && !passthrough()) {
        const auto list = base_list(self, reset_variants.offset[V]);
        list_vtable_seen(list, "Reset");
        list_reset(list, initial);
    }
    return hr;
}
template <int V> void STDMETHODCALLTYPE clear_state_variant(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pipeline) {
    { Internal internal; clear_state_variants.original[V](self, pipeline); }
    if (!passthrough()) pipeline_bound(base_list(self, clear_state_variants.offset[V]), pipeline);
}
template <int V> void STDMETHODCALLTYPE draw_variant(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT first, UINT first_instance) {
    if (!passthrough() && draw_recorded(base_list(self, draw_variants.offset[V]), {false, count, instances, first, first_instance, 0})) return;
    { Internal internal; draw_variants.original[V](self, count, instances, first, first_instance); }
}
template <int V> void STDMETHODCALLTYPE draw_indexed_variant(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT first, INT vertex, UINT first_instance) {
    if (!passthrough() && draw_recorded(base_list(self, draw_indexed_variants.offset[V]), {true, count, instances, first, first_instance, vertex})) return;
    { Internal internal; draw_indexed_variants.original[V](self, count, instances, first, vertex, first_instance); }
}
template <int V> void STDMETHODCALLTYPE execute_indirect_variant(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature, UINT max_count,
    ID3D12Resource* arguments, UINT64 arguments_offset, ID3D12Resource* count, UINT64 count_offset) {
    if (!passthrough()) indirect_recorded(base_list(self, execute_indirect_variants.offset[V]), signature, max_count);
    { Internal internal; execute_indirect_variants.original[V](self, signature, max_count, arguments, arguments_offset, count, count_offset); }
}
template <int V> void STDMETHODCALLTYPE copy_texture_region_variant(ID3D12GraphicsCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dest, UINT x, UINT y,
    UINT z, const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
    if (!passthrough() && dest && source && dest->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && source->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        texture_copied(source->pResource, dest->pResource, dest->SubresourceIndex);
    { Internal internal; copy_texture_region_variants.original[V](self, dest, x, y, z, source, box); }
}
template <int V> void STDMETHODCALLTYPE set_pipeline_state_variant(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pipeline) {
    { Internal internal; set_pipeline_state_variants.original[V](self, pipeline); }
    if (!passthrough()) pipeline_bound(base_list(self, set_pipeline_state_variants.offset[V]), pipeline);
}
template <int V> void STDMETHODCALLTYPE set_root_signature_variant(ID3D12GraphicsCommandList* self, ID3D12RootSignature* root) {
    { Internal internal; set_root_signature_variants.original[V](self, root); }
    if (!passthrough()) root_signature_bound(base_list(self, set_root_signature_variants.offset[V]), root);
}
template <int V> void STDMETHODCALLTYPE set_root_table_variant(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    { Internal internal; set_root_table_variants.original[V](self, index, table); }
    if (!passthrough()) table_bound(base_list(self, set_root_table_variants.offset[V]), index, table);
}
template <int V> void STDMETHODCALLTYPE set_render_targets_variant(ID3D12GraphicsCommandList* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
    BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    { Internal internal; set_render_targets_variants.original[V](self, count, targets, single_range, depth); }
    if (!passthrough()) targets_bound(base_list(self, set_render_targets_variants.offset[V]), count, targets, single_range, depth);
}
template <int V> void STDMETHODCALLTYPE set_scissor_rects_variant(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rects) {
    { Internal internal; set_scissor_rects_variants.original[V](self, count, rects); }
    if (!passthrough()) scissors_bound(base_list(self, set_scissor_rects_variants.offset[V]), count, rects);
}
template <int V> void STDMETHODCALLTYPE begin_render_pass_variant(ID3D12GraphicsCommandList4* self, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    if (!passthrough()) pass_begun(base_list(self, begin_render_pass_variants.offset[V]), count, targets);
    { Internal internal; begin_render_pass_variants.original[V](self, count, targets, depth, flags); }
}
template <int V> void STDMETHODCALLTYPE end_render_pass_variant(ID3D12GraphicsCommandList4* self) {
    if (!passthrough()) pass_ended(base_list(self, end_render_pass_variants.offset[V]));
    { Internal internal; end_render_pass_variants.original[V](self); }
}
#define VARIANT_DETOURS(name) std::array<void*, max_variants>{reinterpret_cast<void*>(&name<0>), reinterpret_cast<void*>(&name<1>), \
    reinterpret_cast<void*>(&name<2>), reinterpret_cast<void*>(&name<3>)}

std::mutex variants_mutex;
std::atomic<int> variant_hooks{0};
template <class F> void hook_variant(const char* name, Variants<F>& variants, const std::array<void*, max_variants>& detours, void* hooked, void* target,
    ptrdiff_t offset) {
    if (!target || target == hooked) return;
    for (int i = 0; i < variants.count; ++i)
        if (variants.target[i] == target) {
            if (variants.offset[i] != offset) log("WARNING command list variant %s at %p seen with offsets %+td and %+td", name, target, variants.offset[i], offset);
            return;
        }
    if (variants.count == max_variants) { log("WARNING command list variant %s: more than %d, %s not hooked", name, max_variants, describe_code(target).c_str()); return; }
    const int i = variants.count;
    variants.offset[i] = offset;
    variants.target[i] = target;
    void* trampoline = nullptr;
    MH_STATUS status = MH_CreateHook(target, detours[i], &trampoline);
    if (status == MH_OK) {
        variants.original[i] = reinterpret_cast<F>(trampoline);
        status = MH_EnableHook(target);
    }
    if (status == MH_OK) { ++variants.count; ++variant_hooks; }
    log("command list variant %s #%d offset=%+td %s hooked=%d (status %d)", name, i, offset, describe_code(target).c_str(), int(status == MH_OK), int(status));
}
// Hooks the functions of every interface version of a direct command list that differ from the hooked ones.
void install_list_variants(ID3D12GraphicsCommandList* list) noexcept try {
    if (!hooked_list_vtable || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    static const IID* const versions[]{&__uuidof(ID3D12GraphicsCommandList), &__uuidof(ID3D12GraphicsCommandList1), &__uuidof(ID3D12GraphicsCommandList2),
        &__uuidof(ID3D12GraphicsCommandList3), &__uuidof(ID3D12GraphicsCommandList4), &__uuidof(ID3D12GraphicsCommandList5), &__uuidof(ID3D12GraphicsCommandList6),
        &__uuidof(ID3D12GraphicsCommandList7), &__uuidof(ID3D12GraphicsCommandList8), &__uuidof(ID3D12GraphicsCommandList9), &__uuidof(ID3D12GraphicsCommandList10)};
    std::lock_guard lock(variants_mutex);
    for (int v = 0; v < 11; ++v) {
        ComPtr<IUnknown> version;
        if (FAILED(list->QueryInterface(*versions[v], reinterpret_cast<void**>(version.GetAddressOf())))) continue;
        void** const vt = *reinterpret_cast<void***>(version.Get());
        const ptrdiff_t offset = reinterpret_cast<char*>(version.Get()) - reinterpret_cast<char*>(list);
        const auto entry = [&](size_t slot) { return vt[slot]; };
        const auto hooked = [&](size_t slot) { return hooked_list_vtable[slot]; };
        hook_variant("reset", reset_variants, VARIANT_DETOURS(reset_variant), hooked(list_reset_slot()), entry(list_reset_slot()), offset);
        hook_variant("clear_state", clear_state_variants, VARIANT_DETOURS(clear_state_variant), hooked(list_clear_state_slot()), entry(list_clear_state_slot()), offset);
        hook_variant("draw", draw_variants, VARIANT_DETOURS(draw_variant), hooked(list_draw_slot()), entry(list_draw_slot()), offset);
        hook_variant("draw_indexed", draw_indexed_variants, VARIANT_DETOURS(draw_indexed_variant), hooked(list_draw_indexed_slot()), entry(list_draw_indexed_slot()), offset);
        hook_variant("copy_texture_region", copy_texture_region_variants, VARIANT_DETOURS(copy_texture_region_variant), hooked(list_copy_texture_region_slot()),
            entry(list_copy_texture_region_slot()), offset);
        hook_variant("set_pipeline_state", set_pipeline_state_variants, VARIANT_DETOURS(set_pipeline_state_variant), hooked(list_set_pipeline_state_slot()),
            entry(list_set_pipeline_state_slot()), offset);
        hook_variant("set_root_signature", set_root_signature_variants, VARIANT_DETOURS(set_root_signature_variant), hooked(list_set_graphics_root_signature_slot()),
            entry(list_set_graphics_root_signature_slot()), offset);
        hook_variant("set_root_table", set_root_table_variants, VARIANT_DETOURS(set_root_table_variant), hooked(list_set_graphics_root_table_slot()),
            entry(list_set_graphics_root_table_slot()), offset);
        hook_variant("set_render_targets", set_render_targets_variants, VARIANT_DETOURS(set_render_targets_variant), hooked(list_set_render_targets_slot()),
            entry(list_set_render_targets_slot()), offset);
        if (original_set_scissor_rects)
            hook_variant("set_scissor_rects", set_scissor_rects_variants, VARIANT_DETOURS(set_scissor_rects_variant), hooked(list_set_scissor_rects_slot()),
                entry(list_set_scissor_rects_slot()), offset);
        if (original_execute_indirect)
            hook_variant("execute_indirect", execute_indirect_variants, VARIANT_DETOURS(execute_indirect_variant), hooked(list_execute_indirect_slot()),
                entry(list_execute_indirect_slot()), offset);
        if (v >= 4 && hooked_list4_vtable && original_begin_render_pass) {
            hook_variant("begin_render_pass", begin_render_pass_variants, VARIANT_DETOURS(begin_render_pass_variant), hooked_list4_vtable[list_begin_render_pass_slot()],
                entry(list_begin_render_pass_slot()), offset);
            hook_variant("end_render_pass", end_render_pass_variants, VARIANT_DETOURS(end_render_pass_variant), hooked_list4_vtable[list_end_render_pass_slot()],
                entry(list_end_render_pass_slot()), offset);
        }
    }
} catch (...) {}

// A runtime may also switch a list's vtable after creating it (e.g. to one for recording, at Reset): every list vtable
// met at creation, Reset and submission is checked once, and the functions of a new one are hooked as well.
std::array<std::atomic<void**>, 32> known_list_vtables{};
std::atomic<int> known_list_vtable_count{0};
void list_vtable_seen(ID3D12GraphicsCommandList* list, const char* where) noexcept try {
    void** const vtable = *reinterpret_cast<void***>(list);
    const int count = known_list_vtable_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) if (known_list_vtables[i].load(std::memory_order_relaxed) == vtable) return;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const int now = known_list_vtable_count.load(std::memory_order_relaxed);
    for (int i = 0; i < now; ++i) if (known_list_vtables[i].load(std::memory_order_relaxed) == vtable) return;
    if (now == int(known_list_vtables.size())) return;
    known_list_vtables[now].store(vtable, std::memory_order_relaxed);
    known_list_vtable_count.store(now + 1, std::memory_order_release);
    const int before = variant_hooks.load();
    log("command_list vtable=%p first seen at %s type=%d (hooked vtable %p) %s", vtable, where, int(list->GetType()), hooked_list_vtable, describe_code(vtable).c_str());
    install_list_variants(list);
    if (variant_hooks.load() != before) log("command list variants now %d", variant_hooks.load());
} catch (...) {}

using QueueExecute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
QueueExecute original_queue_execute = nullptr;
void STDMETHODCALLTYPE queue_execute_hook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!passthrough() && lists)
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            void** const vtable = *reinterpret_cast<void***>(lists[i]);
            bool known = false;
            const int n = known_list_vtable_count.load(std::memory_order_acquire);
            for (int k = 0; k < n && !known; ++k) known = known_list_vtables[k].load(std::memory_order_relaxed) == vtable;
            if (known) continue;
            ComPtr<ID3D12GraphicsCommandList> list;
            if (SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&list)))) list_vtable_seen(list.Get(), "ExecuteCommandLists");
        }
    original_queue_execute(queue, count, lists);
}

// The runtime's functions are shared by all its devices and command lists, so they are hooked once, at the first device.
void install_d3d12_hooks(ID3D12Device* device) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    bool pipelines = hook_slot(device, graphics_pipeline_slot(), reinterpret_cast<void*>(&graphics_hook), original_graphics);
    ComPtr<ID3D12Device2> device2;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2))))
        pipelines &= hook_slot(device2.Get(), pipeline_stream_slot(), reinterpret_cast<void*>(&stream_hook), original_stream);
    bool devices = hook_slot(device, create_descriptor_heap_slot(), reinterpret_cast<void*>(&create_heap_hook), original_create_heap) &&
        hook_slot(device, copy_descriptors_slot(), reinterpret_cast<void*>(&copy_hook), original_copy) &&
        hook_slot(device, copy_descriptors_simple_slot(), reinterpret_cast<void*>(&copy_simple_hook), original_copy_simple) &&
        hook_slot(device, create_root_signature_slot(), reinterpret_cast<void*>(&create_root_signature_hook), original_create_root_signature) &&
        hook_slot(device, create_srv_slot(), reinterpret_cast<void*>(&create_srv_hook), original_create_srv) &&
        hook_slot(device, create_uav_slot(), reinterpret_cast<void*>(&create_uav_hook), original_create_uav) &&
        hook_slot(device, create_rtv_slot(), reinterpret_cast<void*>(&create_rtv_hook), original_create_rtv) &&
        hook_slot(device, create_command_list_slot(), reinterpret_cast<void*>(&create_command_list_hook), original_create_command_list);
    ComPtr<ID3D12Device4> device4;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))))
        devices &= hook_slot(device4.Get(), create_command_list1_slot(), reinterpret_cast<void*>(&create_command_list1_hook), original_create_command_list1);
    // Only the pixel probe needs these (indirect draws listed, the game's scissor rectangle restored).
    bool probes = hook_slot(device, create_command_signature_slot(), reinterpret_cast<void*>(&create_command_signature_hook), original_create_command_signature);
    // The command list functions, found through a list of our own.
    bool lists = false, passes = false;
    {
        Internal internal;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
            list->Close();
            hooked_list_vtable = *reinterpret_cast<void***>(list.Get());
            lists = hook_slot(list.Get(), list_reset_slot(), reinterpret_cast<void*>(&reset_hook), original_reset) &&
                hook_slot(list.Get(), list_clear_state_slot(), reinterpret_cast<void*>(&clear_state_hook), original_clear_state) &&
                hook_slot(list.Get(), list_draw_slot(), reinterpret_cast<void*>(&draw_hook), original_draw) &&
                hook_slot(list.Get(), list_draw_indexed_slot(), reinterpret_cast<void*>(&draw_indexed_hook), original_draw_indexed) &&
                hook_slot(list.Get(), list_copy_texture_region_slot(), reinterpret_cast<void*>(&copy_texture_region_hook), original_copy_texture_region) &&
                hook_slot(list.Get(), list_set_pipeline_state_slot(), reinterpret_cast<void*>(&set_pipeline_state_hook), original_set_pipeline_state) &&
                hook_slot(list.Get(), list_set_graphics_root_signature_slot(), reinterpret_cast<void*>(&set_root_signature_hook), original_set_root_signature) &&
                hook_slot(list.Get(), list_set_graphics_root_table_slot(), reinterpret_cast<void*>(&set_root_table_hook), original_set_root_table) &&
                hook_slot(list.Get(), list_set_render_targets_slot(), reinterpret_cast<void*>(&set_render_targets_hook), original_set_render_targets);
            probes = probes && hook_slot(list.Get(), list_set_scissor_rects_slot(), reinterpret_cast<void*>(&set_scissor_rects_hook), original_set_scissor_rects) &&
                hook_slot(list.Get(), list_execute_indirect_slot(), reinterpret_cast<void*>(&execute_indirect_hook), original_execute_indirect);
            ComPtr<ID3D12GraphicsCommandList4> list4;
            if (SUCCEEDED(list.As(&list4))) {
                hooked_list4_vtable = *reinterpret_cast<void***>(list4.Get());
                hooked_list4_offset = reinterpret_cast<char*>(list4.Get()) - reinterpret_cast<char*>(list.Get());
            }
            if (list4)
                passes = hook_slot(list4.Get(), list_begin_render_pass_slot(), reinterpret_cast<void*>(&begin_render_pass_hook), original_begin_render_pass) &&
                    hook_slot(list4.Get(), list_end_render_pass_slot(), reinterpret_cast<void*>(&end_render_pass_hook), original_end_render_pass);
            if (lists) list_vtable_seen(list.Get(), "install");
        }
    }
    // The queue's submissions, to meet list vtables the sim switches to after creating its lists.
    bool queues = false;
    {
        Internal internal;
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))))
            queues = hook_slot(queue.Get(), queue_execute_slot(), reinterpret_cast<void*>(&queue_execute_hook), original_queue_execute);
    }
    log("d3d12 hooks pipelines=%d device=%d command_lists=%d render_passes=%d pixel_probe=%d queue=%d variants=%d%s", pipelines, devices, lists, passes, probes, int(queues), variant_hooks.load(),
        pipelines && devices && lists ? "" : " - ERROR the fix cannot follow the sim's drawing without these; send the log");
}

// DXGI: swapchain creation (to know the sim's swapchain and queue) and presents (the frame clock).
using FactoryCreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using FactoryCreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using FactoryCreateSwapChainForCoreWindow = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
    IDXGISwapChain1**);
using FactoryCreateSwapChainForComposition = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using SwapChainPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SwapChainPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
FactoryCreateSwapChain original_create_swapchain = nullptr;
FactoryCreateSwapChainForHwnd original_create_swapchain_hwnd = nullptr;
FactoryCreateSwapChainForCoreWindow original_create_swapchain_core_window = nullptr;
FactoryCreateSwapChainForComposition original_create_swapchain_composition = nullptr;
SwapChainPresent original_present = nullptr;
SwapChainPresent1 original_present1 = nullptr;
HRESULT STDMETHODCALLTYPE create_swapchain_hook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const HRESULT hr = original_create_swapchain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) swapchain_created(device, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_swapchain_hwnd_hook(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = original_create_swapchain_hwnd(factory, device, window, desc, fullscreen, output, out);
    if (SUCCEEDED(hr) && out && *out) swapchain_created(device, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_swapchain_core_window_hook(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = original_create_swapchain_core_window(factory, device, window, desc, output, out);
    if (SUCCEEDED(hr) && out && *out) swapchain_created(device, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_swapchain_composition_hook(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
    IDXGISwapChain1** out) {
    const HRESULT hr = original_create_swapchain_composition(factory, device, desc, output, out);
    if (SUCCEEDED(hr) && out && *out) swapchain_created(device, *out);
    return hr;
}
// One tick per present, also when Present calls Present1 inside (or the other way round).
thread_local bool in_present = false;
HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    const bool outer = !in_present;
    if (outer && !(flags & DXGI_PRESENT_TEST) && !passthrough()) presented(swapchain);
    in_present = true;
    const HRESULT hr = original_present(swapchain, interval, flags);
    if (outer) in_present = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* swapchain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    const bool outer = !in_present;
    if (outer && !(flags & DXGI_PRESENT_TEST) && !passthrough()) presented(swapchain);
    in_present = true;
    const HRESULT hr = original_present1(swapchain, interval, flags, parameters);
    if (outer) in_present = false;
    return hr;
}
std::mutex dxgi_hooks_mutex;
std::atomic<bool> dxgi_hooks_installed{false};
void install_dxgi_hooks(IUnknown* factory) {
    std::lock_guard lock(dxgi_hooks_mutex);
    ComPtr<IDXGIFactory2> factory2;
    if (dxgi_hooks_installed || !factory || FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) return;
    dxgi_hooks_installed = true;
    const bool hooked = hook_slot(factory2.Get(), factory_create_swapchain_slot(), reinterpret_cast<void*>(&create_swapchain_hook), original_create_swapchain) &&
        hook_slot(factory2.Get(), factory_create_swapchain_hwnd_slot(), reinterpret_cast<void*>(&create_swapchain_hwnd_hook), original_create_swapchain_hwnd) &&
        hook_slot(factory2.Get(), factory_create_swapchain_core_window_slot(), reinterpret_cast<void*>(&create_swapchain_core_window_hook), original_create_swapchain_core_window) &&
        hook_slot(factory2.Get(), factory_create_swapchain_composition_slot(), reinterpret_cast<void*>(&create_swapchain_composition_hook), original_create_swapchain_composition);
    log("dxgi swapchain hooks=%d", hooked);
}
// Standalone installed as version.dll or winmm.dll: the sim's DXGI factories aren't made through this DLL's exports, so
// swapchain creation is hooked on a factory made here once the sim has its device (the hooks sit in the factory's
// functions, which every factory shares). As dxgi.dll the sim's first factory has done that already.
void hook_swapchain_creation() {
    if (dxgi_hooks_installed.load()) return;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) install_dxgi_hooks(factory.Get());
}
void install_present_hooks(IDXGISwapChain* swapchain) {
    std::lock_guard lock(dxgi_hooks_mutex);
    static bool installed = false;
    ComPtr<IDXGISwapChain1> swapchain1;
    if (installed || FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1)))) return;
    installed = true;
    const bool hooked = hook_slot(swapchain1.Get(), swapchain_present_slot(), reinterpret_cast<void*>(&present_hook), original_present) &&
        hook_slot(swapchain1.Get(), swapchain_present1_slot(), reinterpret_cast<void*>(&present1_hook), original_present1);
    log("dxgi present hooks=%d%s", hooked, hooked ? "" : " (the frame clock runs on a timer instead; captures leave out the presented frames)");
}

// ---------------------------------------------------------------------------------------------------------------
// Start (host.hpp), when the sim loads this DLL: as its dxgi.dll (or through another loader), or as ReShade's add-on.
// In any other process (the launcher, crash reporter...) the fix doesn't start, and the standalone DLL only passes
// dxgi.dll's exports through.
bool active = false;
}

namespace glass {
bool start(void* self_module, void* reshade_module) noexcept try {
    const auto self = static_cast<HMODULE>(self_module);
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"FlightSimulator2024.exe") != 0) return false;
    // The standalone installed twice (e.g. as dxgi.dll and as winmm.dll): only the copy loaded first runs, or the screens
    // would be fixed twice and both copies would write the same log. The other one only passes its exports through.
    if (!reshade_module) {
        wchar_t marker_name[64]{};
        swprintf(marker_name, 64, L"Local\\GlassCockpitDlssFixStandalone-%lu", GetCurrentProcessId());
        const HANDLE marker = CreateEventW(nullptr, TRUE, FALSE, marker_name); // kept open for the process's life
        if (marker && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(marker); return false; }
    }
    if (const auto minhook = MH_Initialize(); minhook != MH_OK && minhook != MH_ERROR_ALREADY_INITIALIZED) return false;
    active = true;
    reshade_addon = reshade_module != nullptr;
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&start), &pinned);
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(self, module, MAX_PATH);
    addon_directory = std::filesystem::path(module).parent_path();
    // Offline test host: the stand-in display shader sits next to the fake FlightSimulator2024.exe.
    const auto fixture_directory = std::filesystem::path(executable).parent_path();
    std::error_code error;
    fixture_host = std::filesystem::exists(fixture_directory / L"fixture_ps.cso", error);
    force_tracking = fixture_host && wcsstr(GetCommandLineW(), L"--track");
    if (fixture_host) {
        auto hash_file = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
            return shader_hash(bytes.data(), bytes.size());
        };
        display_ps = hash_file(fixture_directory / L"fixture_ps.cso");
        display_vs = hash_file(fixture_directory / L"fixture_vs.cso");
        vr_display_ps = hash_file(fixture_directory / L"fixture_vr_ps.cso");
        display_ps_t0 = hash_file(fixture_directory / L"fixture_t0_ps.cso");
        vr_display_ps_t0 = hash_file(fixture_directory / L"fixture_vr_t0_ps.cso");
        vr_display_vs = hash_file(fixture_directory / L"fixture_vr_vs.cso");
    }
    // Keep the previous session's log: after a crash people usually relaunch before sending it.
    const auto log_path = addon_directory / L"glass-cockpit-dlss-fix.log";
    std::filesystem::rename(log_path, addon_directory / L"glass-cockpit-dlss-fix.old.log", error);
    // Not inherited (N): the sim starts helper processes (e.g. FenixBootstrapper) that would keep the log open after it
    // quits, and the next session could not open it.
    logfile = _wfsopen(log_path.c_str(), L"wN", _SH_DENYWR);
    log_file_path = log_path;
    if (!logfile) {
        // Something still holds the log (e.g. the previous session's process hasn't quite exited): log under another
        // name rather than not at all.
        log_file_path = addon_directory / (L"glass-cockpit-dlss-fix-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        logfile = _wfsopen(log_file_path.c_str(), L"wN", _SH_DENYWR);
        if (logfile) log("the usual log file could not be opened (another process holds it): logging here instead");
    }
    // The offline test runs with default settings unless asked to read the .ini (GLASS_FIXTURE_SETTINGS=1).
    wchar_t fixture_settings[8]{};
    if (!fixture_host || (GetEnvironmentVariableW(L"GLASS_FIXTURE_SETTINGS", fixture_settings, 8) && fixture_settings[0] == L'1')) {
        std::lock_guard lock(settings_mutex);
        load_settings();
        write_settings_file();
        settings_changed(); // remember the file's time: only later changes are reloads
    }
    init_tick = GetTickCount64();
    OSVERSIONINFOEXW os{sizeof(os)};
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&os);
    log("%s (%s) %s (built " __DATE__ " " __TIME__ ")", GLASS_NAME, edition(), overlay_version);
    log("windows %lu.%lu.%lu  game=%ls", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, executable);
    if (reshade_addon) {
        wchar_t reshade_path[MAX_PATH]{};
        GetModuleFileNameW(static_cast<HMODULE>(reshade_module), reshade_path, MAX_PATH);
        log("reshade %s path=%ls", file_version(reshade_path).c_str(), reshade_path);
    }
    log("module path=%ls fixture=%d %s", module, fixture_host, settings_text().c_str());
    log("hotkeys: %sCtrl+Shift+F9 overlay on/off, Ctrl+Shift+F11 green tint; settings in %ls",
        reshade_addon ? "" : (glass::hotkey_name(settings_window_key) + " settings window, ").c_str(), settings_path().c_str());
    if (!reshade_addon && _wcsicmp(std::filesystem::path(module).filename().c_str(), L"dxgi.dll") != 0)
        log("note loaded as %ls rather than as dxgi.dll", std::filesystem::path(module).filename().c_str());
    std::thread(worker).detach();
    std::thread(housekeeping).detach();
    return true;
} catch (...) { return false; }

void stop() { shutting_down = true; }

// Standalone: the sim creates its device after loading this DLL. d3d12.dll is loaded already (this DLL imports it); if
// its device exists already, this DLL came too late to see it made.
void hook_device_creation() noexcept try {
    if (!active) return;
    loaded_late = GetModuleHandleW(L"d3d12core.dll") != nullptr;
    const auto d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto create = d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    const bool hooked = create && hook(reinterpret_cast<void*>(create), reinterpret_cast<void*>(&create_device_hook), original_create_device);
    log("device creation hook=%d%s", hooked, loaded_late ? " (a device existed already: loaded late)" : "");
} catch (...) {}

void dxgi_factory_created(IUnknown* factory) {
    if (active) install_dxgi_hooks(factory);
}
void device_created(ID3D12Device* device) {
    if (active && device) ::device_created(device);
}
void swapchain_created(IUnknown* queue, IUnknown* swapchain) {
    if (active) ::swapchain_created(queue, swapchain);
}
void write_log(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}

Status status() {
    Status s;
    {
        std::lock_guard lock(info_mutex);
        s.gpu = gpu_name; s.driver = gpu_driver; s.dlss_version = dlss_version; s.fsr_version = fsr_version;
    }
    s.addon = reshade_addon;
    s.settings_window_key = settings_window_key;
    s.gpu_known = gpu_known; s.nvidia = gpu_nvidia; s.dlss_loaded = ngx_hooked || ngx_proxy_hooked; s.fsr_loaded = ffx_hooked; s.shader_found = stats.display_pipelines > 0;
    s.enabled = enabled; s.tint = tint; s.experimental = {detail_clamp_percent, edge_fade_percent, angle_soften_percent, edge_stretch_percent, dark_screen_fix, seam_fix, temporal_aa, soft_smoothing, stable_edges, int(mip0_only.load()), calm_glass, edge_trim_percent, occlusion_check}; s.jitter_positive = jitter_positive; s.line_smoothing = smoothing_percent; s.extra_smoothing = extra_smoothing; s.glass_strength = glass_percent;
    s.dlss_calls = stats.evaluations; s.dlss_upscales = stats.dlss_upscales; s.fsr_calls = stats.fsr_dispatches; s.upscaler = last_upscaler; s.screen_draws = stats.display_draws; s.recorded = stats.replays;
    s.overlay_frames = stats.composites; s.skipped = stats.composite_skipped;
    const auto now = present_count.load(std::memory_order_relaxed);
    s.active = s.enabled && s.overlay_frames && now <= last_composite_present + 30;
    for (unsigned i = 0; i < slot_count; ++i) if (slot_resources[i].load() && now <= slot_last_used[i] + 30) ++s.screens;
    s.render_w = last_render_w; s.render_h = last_render_h; s.out_w = last_out_w; s.out_h = last_out_h;
    s.problem = problem;
    s.log_path = (log_file_path.empty() ? addon_directory / L"glass-cockpit-dlss-fix.log" : log_file_path).wstring();
    { std::lock_guard lock(capture_mutex); s.capture = capture_message; }
    return s;
}
void set_enabled(bool on) {
    if (on == enabled) return;
    if (on) ++generation;
    change_settings([&] { enabled = on; });
    log("setting overlay=%d", int(on));
}
void set_tint(bool on) { tint = on; log("setting tint=%d", int(on)); }
void request_screen_probe() {
    screen_probe_until = present_count.load() + 600;
    log("screen_probe started: looking for draws that read cockpit-screen-like textures with other shaders");
}
void request_pixel_probe() {
    if (!main_state || pixel_probe_frame.load() != UINT64_MAX) return;
    { std::lock_guard lock(pixel_probe_pending_mutex); pixel_probe_pending.clear(); }
    if (!main_state->probe_queries) {
        D3D12_QUERY_HEAP_DESC heap{D3D12_QUERY_HEAP_TYPE_OCCLUSION, pixel_probe_max, 0};
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, uint64_t(pixel_probe_max) * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12QueryHeap> queries; ComPtr<ID3D12Resource> results;
        if (FAILED(main_state->native->CreateQueryHeap(&heap, IID_PPV_ARGS(&queries))) ||
            FAILED(main_state->native->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&results)))) {
            log("pixel_probe: could not create its queries"); return;
        }
        main_state->probe_readback = results; main_state->probe_queries = queries;
    }
    { std::lock_guard lock(pixel_probe_mutex); pixel_probe_draws.clear(); }
    pixel_probe_used = 0;
    pixel_probe_frame = present_count.load() + 1;
    log("pixel_probe requested: the draws of the next frame that reach the middle of the view");
}
void request_capture() {
    std::lock_guard lock(capture_mutex);
    if (capture_requested.load() || capture_ready_at || capture_presents_left.load()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"glass-capture-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    capture_folder = addon_directory / name;
    capture_frames.clear();
    capture_message = "Capturing... (fly with the screens in view)";
    capture_countdown = 0;
    capture_wait_until = present_count.load() + capture_wait_limit; // until the draw-time colour shaders are ready
    capture_presents_left = capture_present_count;
    capture_requested = capture_frame_count;
    log("capture requested: %ls", capture_folder.c_str());
}
// The add-on's overlay tab (status_panel.cpp).
void set_line_smoothing(int percent) {
    change_settings([&] { smoothing_percent = std::clamp(percent, 0, smoothing_limit()); });
    log("setting line_smoothing=%d", smoothing_percent.load());
}
void set_extra_smoothing(bool on) {
    change_settings([&] { extra_smoothing = on; smoothing_percent = std::min(smoothing_percent.load(), smoothing_limit()); });
    log("setting extra_smoothing=%d line_smoothing=%d", int(on), smoothing_percent.load());
}
void set_jitter_positive(bool on) {
    change_settings([&] { jitter_positive = on; });
    log("setting jitter_positive=%d", int(on));
}
void set_experimental(const Experimental& e, const Experimental* edited_from) {
    change_settings([&] {
        // Only the fixes changed here: the others keep what the file says now (another settings app may have changed
        // them since the tab or window last showed them). No snapshot (Reset): all of them.
        const auto changed = [&](int Experimental::* field) { return !edited_from || e.*field != edited_from->*field; };
        if (changed(&Experimental::detail_clamp)) detail_clamp_percent = std::clamp(e.detail_clamp, 0, 100);
        if (changed(&Experimental::edge_fade)) edge_fade_percent = std::clamp(e.edge_fade, 0, max_edge_fade);
        if (changed(&Experimental::angle_softening)) angle_soften_percent = std::clamp(e.angle_softening, 0, 100);
        if (changed(&Experimental::edge_stretch)) edge_stretch_percent = std::clamp(e.edge_stretch, 0, max_edge_stretch);
        if (changed(&Experimental::edge_trim)) edge_trim_percent = std::clamp(e.edge_trim, 0, max_edge_trim);
        if (changed(&Experimental::dark_screens)) dark_screen_fix = e.dark_screens ? 1 : 0;
        if (changed(&Experimental::seam_fix)) seam_fix = e.seam_fix ? 1 : 0;
        if (changed(&Experimental::temporal_aa)) temporal_aa = e.temporal_aa ? 1 : 0;
        if (changed(&Experimental::soft_smoothing)) soft_smoothing = e.soft_smoothing ? 1 : 0;
        if (temporal_aa && soft_smoothing) { if (changed(&Experimental::temporal_aa)) soft_smoothing = 0; else temporal_aa = 0; } // one or the other
        if (changed(&Experimental::stable_edges)) stable_edges = e.stable_edges ? 1 : 0;
        if (changed(&Experimental::full_size)) mip0_only = e.full_size != 0;
        if (changed(&Experimental::calm_glass)) calm_glass = e.calm_glass ? 1 : 0;
        if (changed(&Experimental::occlusion_check)) occlusion_check = e.occlusion_check ? 1 : 0;
    });
    log("setting seam_fix=%d full_size_sampling=%d stable_edges=%d dark_screen_fix=%d detail_clamp=%d edge_fade=%d angle_softening=%d edge_stretch=%d edge_trim=%d temporal_aa=%d soft_smoothing=%d calm_glass=%d occlusion_check=%d", seam_fix.load(), int(mip0_only.load()), stable_edges.load(),
        dark_screen_fix.load(), detail_clamp_percent.load(), edge_fade_percent.load(), angle_soften_percent.load(), edge_stretch_percent.load(), edge_trim_percent.load(), temporal_aa.load(), soft_smoothing.load(), calm_glass.load(), occlusion_check.load());
}
void set_settings_window_key(uint32_t key) {
    if (!key || hotkey_problem(key)) return;
    change_settings([&] { settings_window_key = key; });
    log("setting settings_window hotkey=%s", hotkey_name(key).c_str());
}
void set_glass_strength(int percent) {
    change_settings([&] { glass_percent = std::clamp(percent, 0, 100); });
    log("setting glass_strength=%d", glass_percent.load());
}
}

// Offline test host.
extern "C" __declspec(dllexport) void GlassFixJitterPositive(bool positive) { jitter_positive = positive; }
extern "C" __declspec(dllexport) void GlassFixLineSmoothing(int percent) { smoothing_percent = percent; }
extern "C" __declspec(dllexport) void GlassFixTemporal(int on) { temporal_aa = on ? 1 : 0; }
extern "C" __declspec(dllexport) void GlassFixGaussian(int on) { soft_smoothing = on ? 1 : 0; }
extern "C" __declspec(dllexport) void GlassFixGlassStrength(int percent) { glass_percent = percent; }
extern "C" __declspec(dllexport) void GlassFixStableEdges(int on) { stable_edges = on ? 1 : 0; }
extern "C" __declspec(dllexport) void GlassFixCalmGlass(int on) { calm_glass = on ? 1 : 0; }
extern "C" __declspec(dllexport) void GlassFixEdgeTrim(int hundredths) { edge_trim_percent = std::clamp(hundredths, 0, max_edge_trim); }
extern "C" __declspec(dllexport) void GlassFixOcclusionCheck(int on) { occlusion_check = on ? 1 : 0; }
extern "C" __declspec(dllexport) void GlassFixFullSize(int on) { mip0_only = on != 0; }
extern "C" __declspec(dllexport) void GlassFixDebugFlags(uint32_t bits) { debug_flags = bits; }
// One call per frame stands in for the present (cost window of 4 frames, reported 2 later).
extern "C" __declspec(dllexport) void GlassFixCostTick(ID3D12CommandQueue* queue) {
    static uint64_t frame = 0;
    cost_window = 4; cost_delay = 2; cost_spacing_ms = 0;
    measure_cost(queue, main_state, ++frame);
}
// No swapchain: 1 requests a screen capture (without presented frames), 0 stands in for a present.
extern "C" __declspec(dllexport) void GlassFixCaptureTest(int request) {
    if (request) { glass::request_capture(); capture_presents_left = 0; return; }
    capture_flush(++present_count);
    pixel_probe_flush(present_count.load());
}
// 1 requests a pixel probe; 0 returns the draws that reached the pixel in the last one (high 16 bits: screen draws), -1
// while none has finished.
extern "C" __declspec(dllexport) int GlassFixPixelProbe(int request) {
    if (request) { glass::request_pixel_probe(); pixel_probe_hits = -1; return 0; }
    const int hits = pixel_probe_hits.load();
    return hits < 0 ? -1 : (hits & 0xffff) | (pixel_probe_display_hits.load() << 16);
}

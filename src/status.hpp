// State shared between the add-on and its ReShade status panel (status_panel.cpp).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace glass {
// Experimental fixes (panel section of the same name).
struct Experimental {
    int detail_clamp = 0, edge_fade = 0, angle_softening = 0, edge_stretch = 0; // percent / hundredths of a render pixel
    int dark_screens = 0; // dark screen fix on/off
    int seam_fix = 1; // UV seam fix on/off (on by default)
    int temporal_aa = 0; // temporal anti-aliasing on/off
    int soft_smoothing = 0; // soft-edged (Gaussian) line smoothing on/off
    int stable_edges = 1; // stable screen edges on/off (on by default)
    int full_size = 1; // full-size sampling on/off (on by default; off = the game's mip levels)
    int calm_glass = 0; // calm glass at symbols on/off (off by default; no glass detail from the upscaler next to symbols)
    int edge_trim = 0; // hundredths of a render pixel of each screen's edge left to the upscaler (the reverse of edge_stretch)
    int occlusion_check = 1; // occlusion check on/off (on by default; screen pixels with something in front of them left to the upscaler)
    int painted_screens = 1; // painted screens on/off (on by default; virtual paintings drawn by translucent materials, e.g. Fenix A320 readouts "New")
    bool operator==(const Experimental&) const = default;
};

// Hotkeys: a virtual-key code plus modifier bits, written in the .ini ([hotkeys]) as e.g. "Ctrl+Shift+F9", or "None" for
// no hotkey. Changeable in the panel.
constexpr uint32_t key_ctrl = 0x100, key_shift = 0x200, key_alt = 0x400;
enum Hotkey : int { hotkey_fix, hotkey_tint, hotkey_count };
constexpr uint32_t default_hotkeys[hotkey_count]{key_ctrl | key_shift | 0x78, key_ctrl | key_shift | 0x7A}; // F9, F11

struct Status {
    std::string gpu, driver, dlss_version, fsr_version;
    bool gpu_known = false, nvidia = false, dlss_loaded = false, fsr_loaded = false, shader_found = false;
    bool enabled = false, tint = false, jitter_positive = false, active = false, extra_smoothing = false;
    Experimental experimental;
    int line_smoothing = 0, glass_strength = 100;
    std::array<uint32_t, hotkey_count> hotkeys{}; // by Hotkey (hotkey_name); 0: none
    uint64_t dlss_calls = 0, dlss_upscales = 0, fsr_calls = 0, screen_draws = 0, recorded = 0, overlay_frames = 0, skipped = 0;
    unsigned screens = 0, fixed_screens = 0, render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    const char* upscaler = "DLSS"; // the one the fix was last drawn after
    const char* problem = nullptr; // plain-language description of the current problem, if any
    std::wstring log_path;
    std::string capture; // screen capture progress / result
};
Status status();
void set_enabled(bool on);
void set_tint(bool on);
void request_capture();
void request_pixel_probe();
void request_screen_probe();
void set_jitter_positive(bool on);
void set_line_smoothing(int percent);
void set_extra_smoothing(bool on);
void set_glass_strength(int percent);
void set_experimental(const Experimental& e);

std::string hotkey_name(uint32_t key); // "None" for 0
uint32_t parse_hotkey(const std::string& text); // 0: not a key this understands
// Why a hotkey can't be used (closes the sim, needs a modifier), or null if it can.
const char* hotkey_problem(uint32_t key);
const char* hotkey_action(Hotkey which); // "fix on/off", "green tint"
// Sets a hotkey (0: none). Returns why it wasn't changed (a hotkey_problem, or it is already the other hotkey), or an
// empty string.
std::string set_hotkey(Hotkey which, uint32_t key);
// Set while the panel records a new hotkey and until its keys are let go: the fix's hotkeys don't fire meanwhile.
extern std::atomic<bool> hotkeys_paused;

// The panel only needs ReShade's Dear ImGui; if this ReShade build has no compatible ImGui, the fix still runs.
bool init_status_panel(void* reshade_module);
void uninit_status_panel();
}

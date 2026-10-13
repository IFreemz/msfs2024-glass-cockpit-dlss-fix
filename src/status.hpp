// State shared between the add-on and its ReShade status panel (status_panel.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace glass {
struct Status {
    std::string gpu, driver, dlss_version, fsr_version;
    bool gpu_known = false, nvidia = false, dlss_loaded = false, fsr_loaded = false, shader_found = false;
    bool enabled = false, tint = false, jitter_positive = false, active = false;
    int edge_smoothing = 0, gap_closer = 0, edge_stretch = 0, edge_fade = 0;
    int line_smoothing = 0, glass_strength = 100;
    uint64_t dlss_calls = 0, dlss_upscales = 0, fsr_calls = 0, screen_draws = 0, recorded = 0, overlay_frames = 0, skipped = 0;
    unsigned screens = 0, render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    const char* upscaler = "DLSS"; // the one the fix was last drawn after
    const char* problem = nullptr; // plain-language description of the current problem, if any
    std::wstring log_path;
};
Status status();
void set_enabled(bool on);
void set_tint(bool on);
void set_jitter_positive(bool on);
void set_line_smoothing(int percent);
void set_glass_strength(int percent);
void set_edge_test(int smoothing, int gap, int stretch, int fade);

// The panel only needs ReShade's Dear ImGui; if this ReShade build has no compatible ImGui, the fix still runs.
bool init_status_panel(void* reshade_module);
void uninit_status_panel();
}

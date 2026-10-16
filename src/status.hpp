// State shared between the fix, its status file (status_report.cpp) and the add-on's ReShade overlay tab (status_panel.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace glass {
// Experimental fixes ([experimental] in the settings file, a section of the overlay tab).
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
    bool operator==(const Experimental&) const = default;
};

struct Status {
    bool addon = false; // the ReShade add-on (settings also in its overlay tab), not the standalone dxgi.dll
    std::string gpu, driver, dlss_version, fsr_version;
    bool gpu_known = false, nvidia = false, dlss_loaded = false, fsr_loaded = false, shader_found = false;
    bool enabled = false, tint = false, jitter_positive = false, active = false, extra_smoothing = false;
    Experimental experimental;
    int line_smoothing = 0, glass_strength = 100;
    uint64_t dlss_calls = 0, dlss_upscales = 0, fsr_calls = 0, screen_draws = 0, recorded = 0, overlay_frames = 0, skipped = 0;
    unsigned screens = 0, render_w = 0, render_h = 0, out_w = 0, out_h = 0;
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
// Changed in the overlay tab; saved to the settings file.
void set_jitter_positive(bool on);
void set_line_smoothing(int percent);
void set_extra_smoothing(bool on);
void set_glass_strength(int percent);
void set_experimental(const Experimental& e);

// One line on what the fix is doing ("Working: fixing 4 screens ..."), and the report to paste into bug reports.
std::string status_summary(const Status& s);
std::string status_report(const Status& s);
// The text of glass-cockpit-dlss-fix-status.txt: summary, checks, settings and the report.
std::string status_text(const Status& s);

// The overlay tab only needs ReShade's Dear ImGui; if this ReShade build has no compatible ImGui, the fix still runs.
bool init_status_panel(void* reshade_module);
void uninit_status_panel();
}

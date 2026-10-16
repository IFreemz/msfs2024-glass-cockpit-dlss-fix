// glass-cockpit-dlss-fix-status.txt next to the log: what the fix is doing (what the add-on's overlay tab shows), the
// settings in effect and a report to paste into bug reports. Rewritten by the fix every 10 s while it changes.
#include "status.hpp"
#include "version.h"
#include <cstdarg>
#include <cstdio>
#include <string>

namespace {
std::string format(const char* text, ...) {
    va_list args; va_start(args, text);
    va_list measure; va_copy(measure, args);
    const int length = vsnprintf(nullptr, 0, text, measure);
    va_end(measure);
    std::string result(length > 0 ? size_t(length) : 0, '\0');
    if (length > 0) vsnprintf(result.data(), result.size() + 1, text, args);
    va_end(args);
    return result;
}

const char* edition(const glass::Status& s) { return s.addon ? "ReShade add-on" : "standalone"; }

enum class Check { ok, waiting };
std::string check_row(Check state, const char* label, const std::string& detail) {
    return format("  [%s] %-26s %s\n", state == Check::ok ? "OK" : "..", label, detail.c_str());
}
}

namespace glass {
std::string status_summary(const Status& s) {
    if (s.active) return format("Working: fixing %u screen%s (%s %ux%u -> %ux%u)", s.screens, s.screens == 1 ? "" : "s", s.upscaler, s.render_w, s.render_h, s.out_w, s.out_h);
    if (!s.enabled) return "Off (Ctrl+Shift+F9 to turn on)";
    if (s.problem) return std::string("Not working: ") + s.problem;
    return "Waiting for a glass cockpit with DLSS or FSR selected";
}

std::string status_report(const Status& s) {
    return format("Glass Cockpit DLSS Fix (%s) %s\nstatus: %s\ngpu: %s (driver %s)\ndlss: %s, calls=%llu upscales=%llu\nfsr: %s, calls=%llu\nshader_found=%d screen_draws=%llu recorded=%llu overlay_frames=%llu skipped=%llu\n"
        "enabled=%d jitter_positive=%d tint=%d line_smoothing=%d extra_smoothing=%d glass_strength=%d\n"
        "detail_clamp=%d edge_fade=%d angle_softening=%d edge_stretch=%d edge_trim=%d dark_screen_fix=%d seam_fix=%d full_size_sampling=%d stable_edges=%d temporal_aa=%d soft_smoothing=%d calm_glass=%d occlusion_check=%d\n",
        edition(s), GLASS_VERSION_STRING, status_summary(s).c_str(), s.gpu.empty() ? "unknown" : s.gpu.c_str(), s.driver.c_str(),
        s.dlss_loaded ? (s.dlss_version.empty() ? "loaded" : s.dlss_version.c_str()) : "not loaded", s.dlss_calls, s.dlss_upscales,
        s.fsr_loaded ? (s.fsr_version.empty() ? "loaded" : s.fsr_version.c_str()) : "not loaded", s.fsr_calls, s.shader_found, s.screen_draws,
        s.recorded, s.overlay_frames, s.skipped, s.enabled, s.jitter_positive, s.tint, s.line_smoothing, s.extra_smoothing, s.glass_strength,
        s.experimental.detail_clamp, s.experimental.edge_fade, s.experimental.angle_softening, s.experimental.edge_stretch, s.experimental.edge_trim, s.experimental.dark_screens, s.experimental.seam_fix, s.experimental.full_size, s.experimental.stable_edges, s.experimental.temporal_aa, s.experimental.soft_smoothing, s.experimental.calm_glass, s.experimental.occlusion_check);
}

std::string status_text(const Status& s) {
    std::string text = format("%s (%s) %s - updated every 10 seconds while the sim runs\n\n", GLASS_NAME, edition(s), GLASS_VERSION_STRING);
    text += status_summary(s) + "\n\nChecks\n";
    text += check_row(s.gpu_known ? Check::ok : Check::waiting, "GPU",
        s.gpu_known ? s.gpu + (s.driver.empty() ? "" : "  (driver " + s.driver + ")") + (s.nvidia ? "" : "  - FSR only") : "");
    std::string upscaler;
    if (s.dlss_upscales) upscaler = "DLSS " + s.dlss_version;
    if (s.fsr_calls) upscaler += (upscaler.empty() ? "FSR " : ", FSR ") + s.fsr_version;
    text += check_row(s.dlss_upscales || s.fsr_calls ? Check::ok : Check::waiting, "Upscaler running",
        !upscaler.empty() ? upscaler
        : s.dlss_calls ? "only DLSS Frame Generation (set Anti-Aliasing to DLSS or FSR in the sim's graphics settings)"
        : s.dlss_loaded || s.fsr_loaded ? "loaded, not called yet (select DLSS or FSR in the sim's graphics settings)" : "not loaded yet");
    text += check_row(s.shader_found ? Check::ok : Check::waiting, "Cockpit display shader", s.shader_found ? "found" : "not seen yet (load a flight with a glass cockpit)");
    text += check_row(s.recorded ? Check::ok : Check::waiting, "Cockpit screens", s.recorded ? format("%u visible now", s.screens) : "none recorded yet");
    text += check_row(s.active ? Check::ok : Check::waiting, "Fix drawn after upscaling", s.overlay_frames ? format("%llu frames", s.overlay_frames) : "not yet");
    if (s.problem) text += std::string("\nProblem: ") + s.problem + "\n";
    if (!(s.experimental == Experimental{})) text += "\nExperimental fixes changed in glass-cockpit-dlss-fix.ini ([experimental]).\n";
    if (!s.jitter_positive || s.glass_strength != 100) text += "\nAdvanced settings changed in glass-cockpit-dlss-fix.ini (jitter_positive, glass_strength).\n";
    text += "\nHotkeys in the sim: Ctrl+Shift+F9 fix on/off, Ctrl+Shift+F11 green tint on every fixed screen, Ctrl+Shift+F8 capture the screens,\n"
            "Ctrl+Shift+F7 full-size sampling on/off, Ctrl+Shift+F12 look for screens that stay unfixed and Ctrl+Shift+F10 list the draws\n"
            "that reach the middle of the view (both write to the log). Settings: glass-cockpit-dlss-fix.ini in this folder";
    text += s.addon ? ", or the Glass Cockpit DLSS Fix tab in the ReShade overlay.\n" : ".\n";
    if (!s.capture.empty()) text += "\nCapture: " + s.capture + "\n";
    text += "\nFor bug reports, attach glass-cockpit-dlss-fix.log and glass-cockpit-dlss-fix.old.log from this folder, and the report below.\n\n";
    text += status_report(s);
    // About (GPL-3.0 section 7(b) notice, see NOTICE.md).
    text += format("\n%s %s by %s - GPL-3.0 (see NOTICE.md) - %s\nOfficial downloads: GitHub releases only.\n", GLASS_NAME, GLASS_VERSION_STRING, GLASS_AUTHOR,
        GLASS_WEBSITE);
    return text;
}
}

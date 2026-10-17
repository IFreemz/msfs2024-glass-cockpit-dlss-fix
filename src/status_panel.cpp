// "Glass Cockpit DLSS Fix" tab in the ReShade overlay: status, settings and a copyable report for bug reports.
// Kept in its own file: only this file uses ReShade's Dear ImGui, so a ReShade build with another ImGui version
// just loses the panel (init_status_panel returns false) instead of refusing to load the whole add-on.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "status.hpp"
#include "version.h"
#include <cstdarg>
#include <cstdio>
#include <string>

namespace {
const ImVec4 green{0.35f, 0.85f, 0.35f, 1}, yellow{0.95f, 0.8f, 0.25f, 1}, red{0.95f, 0.35f, 0.3f, 1}, grey{0.6f, 0.6f, 0.6f, 1};

enum class Check { ok, waiting, failed };
void check_row(Check state, const char* label, const std::string& detail) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (state == Check::ok) ImGui::TextColored(green, "OK");
    else if (state == Check::waiting) ImGui::TextColored(grey, "...");
    else ImGui::TextColored(red, "!!");
    ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(detail.c_str());
}

std::string format(const char* text, ...) {
    char buffer[512];
    va_list args; va_start(args, text); vsnprintf(buffer, sizeof(buffer), text, args); va_end(args);
    return buffer;
}

std::string summary(const glass::Status& s) {
    if (s.active) return format("Working: fixing %u screen%s (%s %ux%u -> %ux%u)", s.screens, s.screens == 1 ? "" : "s", s.upscaler, s.render_w, s.render_h, s.out_w, s.out_h);
    if (!s.enabled) return "Off (Ctrl+Shift+F9 to turn on)";
    if (s.problem) return std::string("Not working: ") + s.problem;
    return "Waiting for a glass cockpit with DLSS or FSR selected";
}

std::string report(const glass::Status& s) {
    return format("Glass Cockpit DLSS Fix %s\nstatus: %s\ngpu: %s (driver %s)\ndlss: %s, calls=%llu upscales=%llu\nfsr: %s, calls=%llu\nshader_found=%d screen_draws=%llu recorded=%llu overlay_frames=%llu skipped=%llu\n"
        "enabled=%d jitter_positive=%d tint=%d line_smoothing=%d extra_smoothing=%d glass_strength=%d\n"
        "detail_clamp=%d edge_fade=%d angle_softening=%d edge_stretch=%d edge_trim=%d dark_screen_fix=%d seam_fix=%d full_size_sampling=%d stable_edges=%d temporal_aa=%d soft_smoothing=%d calm_glass=%d occlusion_check=%d\n",
        GLASS_VERSION_STRING, summary(s).c_str(), s.gpu.empty() ? "unknown" : s.gpu.c_str(), s.driver.c_str(),
        s.dlss_loaded ? (s.dlss_version.empty() ? "loaded" : s.dlss_version.c_str()) : "not loaded", s.dlss_calls, s.dlss_upscales,
        s.fsr_loaded ? (s.fsr_version.empty() ? "loaded" : s.fsr_version.c_str()) : "not loaded", s.fsr_calls, s.shader_found, s.screen_draws,
        s.recorded, s.overlay_frames, s.skipped, s.enabled, s.jitter_positive, s.tint, s.line_smoothing, s.extra_smoothing, s.glass_strength,
        s.experimental.detail_clamp, s.experimental.edge_fade, s.experimental.angle_softening, s.experimental.edge_stretch, s.experimental.edge_trim, s.experimental.dark_screens, s.experimental.seam_fix, s.experimental.full_size, s.experimental.stable_edges, s.experimental.temporal_aa, s.experimental.soft_smoothing, s.experimental.calm_glass, s.experimental.occlusion_check);
}

void draw(reshade::api::effect_runtime*) {
    const auto s = glass::status();
    ImGui::Text("%s %s", GLASS_NAME, GLASS_VERSION_STRING);
    ImGui::SameLine(); ImGui::TextColored(grey, "by %s", GLASS_AUTHOR);
    ImGui::TextLinkOpenURL("GitHub page", GLASS_WEBSITE);
    ImGui::SameLine(); ImGui::TextLinkOpenURL("Report a problem", GLASS_ISSUES);
    ImGui::Spacing();
    ImGui::TextColored(s.active ? green : (!s.enabled || !s.problem) ? yellow : red, "%s", summary(s).c_str());

    ImGui::SeparatorText("Settings");
    bool enabled = s.enabled, tint = s.tint;
    int smoothing = s.line_smoothing;
    if (ImGui::Checkbox("Fix enabled  (Ctrl+Shift+F9)", &enabled)) glass::set_enabled(enabled);
    bool extra = s.extra_smoothing;
    if (ImGui::SliderInt("Line smoothing", &smoothing, 0, extra ? 100 : 50, smoothing ? "%d%%" : "off (sharpest)", ImGuiSliderFlags_AlwaysClamp))
        glass::set_line_smoothing(smoothing);
    ImGui::SetItemTooltip("Smooths stair steps and shimmer on thin lines, like DLSS did. 0 = sharpest, 35 = default, 50 = like DLSS.\n"
        "Ctrl+click to type a value.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Extra", &extra)) glass::set_extra_smoothing(extra);
    ImGui::SetItemTooltip("Lets Line smoothing go up to 100 %% for stubborn shimmer (e.g. in VR).\n"
        "Above 50 the whole screen picture gets softer than with DLSS alone.");
    if (ImGui::Checkbox("Green tint on fixed screens  (Ctrl+Shift+F11)", &tint)) glass::set_tint(tint);
    if (!(s.experimental == glass::Experimental{})) ImGui::TextColored(yellow, "Experimental fixes changed (see Troubleshooting below)");
    if (!s.jitter_positive || s.glass_strength != 100) ImGui::TextColored(yellow, "Advanced settings changed (see Troubleshooting below)");

    ImGui::SeparatorText("Checks");
    if (ImGui::BeginTable("checks", 3, ImGuiTableFlags_SizingFixedFit)) {
        check_row(s.gpu_known ? Check::ok : Check::waiting, "GPU",
            s.gpu_known ? s.gpu + (s.driver.empty() ? "" : "  (driver " + s.driver + ")") + (s.nvidia ? "" : "  - FSR only") : "");
        std::string upscaler;
        if (s.dlss_upscales) upscaler = "DLSS " + s.dlss_version;
        if (s.fsr_calls) upscaler += (upscaler.empty() ? "FSR " : ", FSR ") + s.fsr_version;
        check_row(s.dlss_upscales || s.fsr_calls ? Check::ok : Check::waiting, "Upscaler running",
            !upscaler.empty() ? upscaler
            : s.dlss_calls ? "only DLSS Frame Generation (set Anti-Aliasing to DLSS or FSR in the sim's graphics settings)"
            : s.dlss_loaded || s.fsr_loaded ? "loaded, not called yet (select DLSS or FSR in the sim's graphics settings)" : "not loaded yet");
        check_row(s.shader_found ? Check::ok : Check::waiting, "Cockpit display shader", s.shader_found ? "found" : "not seen yet (load a flight with a glass cockpit)");
        check_row(s.recorded ? Check::ok : Check::waiting, "Cockpit screens", s.recorded ? format("%u visible now", s.screens) : "none recorded yet");
        check_row(s.active ? Check::ok : Check::waiting, "Fix drawn after upscaling",
            s.overlay_frames ? format("%llu frames", s.overlay_frames) : "not yet");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Troubleshooting");
    if (s.problem) ImGui::TextWrapped("%s", s.problem);
    if (ImGui::Button("Open log folder")) {
        const std::wstring arguments = L"/select,\"" + s.log_path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy report")) ImGui::SetClipboardText(report(s).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Capture screens  (Ctrl+Shift+F8)")) glass::request_capture();
    ImGui::SetItemTooltip("Saves 8 frames as sent to the monitor and 4 frames over about a third of a second as pictures next to the log: the sim's own frame, the upscaled frame before and\n"
        "after the fix, and the screen map. Use it with the problem visible on screen. Also saves one .zip with the pictures and both logs: attach that to a bug report.");
    if (!s.capture.empty()) ImGui::TextColored(grey, "%s", s.capture.c_str());
    if (ImGui::Button("Pixel probe  (Ctrl+Shift+F10)")) glass::request_pixel_probe();
    ImGui::SetItemTooltip("Logs every draw that reaches the pixel in the middle of the view (marked with a small cross while this section is open),\n"
        "with its shaders and textures: shows how something on a screen is drawn. Put it in the middle of the view first.");
    {
        // The middle of the view, for aiming the pixel probe.
        const ImVec2 size = ImGui::GetIO().DisplaySize, c{size.x * 0.5f, size.y * 0.5f};
        auto* draw = ImGui::GetForegroundDrawList();
        draw->AddLine({c.x - 14, c.y}, {c.x - 4, c.y}, IM_COL32(255, 60, 60, 255), 2); draw->AddLine({c.x + 4, c.y}, {c.x + 14, c.y}, IM_COL32(255, 60, 60, 255), 2);
        draw->AddLine({c.x, c.y - 14}, {c.x, c.y - 4}, IM_COL32(255, 60, 60, 255), 2); draw->AddLine({c.x, c.y + 4}, {c.x, c.y + 14}, IM_COL32(255, 60, 60, 255), 2);
    }
    ImGui::TextColored(grey, "For bug reports, attach glass-cockpit-dlss-fix.log and .old.log from the log folder.");

    // Fixes for problems only some aircraft have; collapsed, and each can be undone with Reset to defaults.
    if (ImGui::TreeNode("Experimental fixes (for problems on some aircraft)")) {
        glass::Experimental e = s.experimental;
        bool seam = e.seam_fix != 0;
        bool changed = ImGui::Checkbox("UV seam fix (on by default)", &seam);
        ImGui::SetItemTooltip("Removes thin lines where a screen's picture meets a part of the same screen that points elsewhere\n"
            "in its texture (e.g. the bezel band on the FSS E-Jets).");
        e.seam_fix = seam ? 1 : 0;
        bool full_size = e.full_size != 0;
        changed |= ImGui::Checkbox("Full-size sampling (on by default)  (Ctrl+Shift+F7)", &full_size);
        ImGui::SetItemTooltip("Reads the screens only from their full-size picture, with more samples where a screen is small on your monitor.\n"
            "Off: also from the smaller copies the GPU keeps for distant screens, which can shimmer when you zoom out.");
        e.full_size = full_size ? 1 : 0;
        bool stable = e.stable_edges != 0;
        changed |= ImGui::Checkbox("Stable screen edges (on by default)", &stable);
        ImGui::SetItemTooltip("Keeps the pixels along each screen's edge from shimmering: where a screen ends is learnt over a few frames\n"
            "instead of judged anew every frame. Turn off if a screen's edge looks cut off or too wide.");
        e.stable_edges = stable ? 1 : 0;
        bool occlusion = e.occlusion_check != 0;
        changed |= ImGui::Checkbox("Occlusion check (on by default)", &occlusion);
        ImGui::SetItemTooltip("Leaves the sim's own picture wherever something sits in front of a screen (a sidestick, a hand, the yoke),\n"
            "using the sim's depth buffer. Off: a screen can be drawn over things in front of it, e.g. the A380 EFB over the sidestick.");
        e.occlusion_check = occlusion ? 1 : 0;
        bool calm_glass = e.calm_glass != 0;
        changed |= ImGui::Checkbox("Calm glass at symbols", &calm_glass);
        ImGui::SetItemTooltip("Right next to symbols and lines, the glass effects (dirt, glare, reflections) are drawn without their finest\n"
            "detail, which flickers there (e.g. along the E-Jet's altitude and vertical speed tapes). Away from symbols nothing changes.\n"
            "Turn off if something drawn over a screen (a highlight, a cursor) looks blurred.");
        e.calm_glass = calm_glass ? 1 : 0;
        bool dark = e.dark_screens != 0;
        changed |= ImGui::Checkbox("Dark screen fix", &dark);
        ImGui::SetItemTooltip("Keeps mostly black screens with thin symbols (e.g. the F/A-18E's) fixed in strong sun glare.\n"
            "Can also let the fix draw over a screen whose picture it can't redraw.");
        e.dark_screens = dark ? 1 : 0;
        // One or the other: ticking one unticks the other.
        bool soft = e.soft_smoothing != 0, temporal = e.temporal_aa != 0;
        if (ImGui::Checkbox("Soft-edged line smoothing", &soft)) { changed = true; if (soft) temporal = false; }
        ImGui::SetItemTooltip("Line smoothing weighted towards each pixel's centre over a wider area: smoother screens with less shimmer\n"
            "while the view moves. Plain Line smoothing works well on most aircraft; try this if screens still shimmer\n"
            "while you move your head. A matter of taste. Needs Line smoothing above 0.");
        if (ImGui::Checkbox("Temporal anti-aliasing", &temporal)) { changed = true; if (temporal) soft = false; }
        ImGui::SetItemTooltip("Smooths screen text and lines over several frames, like the sim's own TAA, without the trails:\n"
            "the history is dropped wherever a screen's picture changes. Plain Line smoothing works well on most aircraft;\n"
            "try this if you want sharper text that still doesn't stair-step, with Line smoothing at 0-20.\n"
            "Uses some extra video memory (about 100 MB at 4K).");
        e.soft_smoothing = soft ? 1 : 0;
        e.temporal_aa = temporal ? 1 : 0;
        changed |= ImGui::SliderInt("Edge trim", &e.edge_trim, 0, 400, e.edge_trim ? "%d" : "off", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Leaves the outer edge of every screen to the sim's own picture, in hundredths of a render pixel\n"
            "(100 = 1 pixel). The reverse of the edge stretcher: for screens whose fixed edge shows a sliver of something\n"
            "that isn't there, or jagged edges. 50 calms most flickering or shimmering screen edges; up to 150 for aircraft\n"
            "with stronger edge problems. Within that band the upscaler's trails come back.");
        changed |= ImGui::SliderInt("Detail clamp", &e.detail_clamp, 0, 100, e.detail_clamp ? "%d%%" : "off", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Keeps the fixed screens within the brightness the sim itself draws around each pixel.\n"
            "Hides texture detail too thin for the sim to show, e.g. a dashed line where a screen's picture meets its bezel.\n"
            "Try 100 if you see such lines. Lower it if small text looks duller than with the fix off.");
        changed |= ImGui::SliderInt("Edge fade", &e.edge_fade, 0, 250, e.edge_fade ? "%d" : "off", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Fades screen edges into the sim's own picture, in hundredths of a render pixel (100 = 1 pixel).\n"
            "Can calm jaggy or crawling screen edges. Within that band the upscaler's trails come back a little; higher values can flicker.");
        changed |= ImGui::SliderInt("Angle softening", &e.angle_softening, 0, 100, e.angle_softening ? "%d%%" : "off", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Draws screens seen at a steep angle softer, like the sim does. Screens seen straight on are unchanged.");
        changed |= ImGui::SliderInt("Edge stretcher", &e.edge_stretch, 0, 400, e.edge_stretch ? "%d" : "off", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Extends screens outwards over their frame, in hundredths of a render pixel (100 = 1 pixel),\n"
            "continuing their picture. For screens that don't quite reach their frame.");
        if (ImGui::Button("Reset experimental fixes")) { e = {}; changed = true; }
        if (changed) glass::set_experimental(e);
        ImGui::TreePop();
    }

    // Settings that only help in specific cases; collapsed so they aren't mistaken for quality settings.
    if (ImGui::TreeNode("Advanced (only change these if asked to in a bug report)")) {
        bool flipped = !s.jitter_positive;
        int glass_strength = s.glass_strength;
        if (ImGui::Checkbox("Flip jitter direction", &flipped)) glass::set_jitter_positive(!flipped);
        ImGui::SetItemTooltip("Only needed if the screen text shimmers while the camera is still.");
        if (ImGui::SliderInt("Glass layer", &glass_strength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) glass::set_glass_strength(glass_strength);
        ImGui::SetItemTooltip("How much of the sim's glass effects (dirt, glare, reflections, shadows) is kept on fixed screens.\n"
            "100 = all (default). Lower values can help if quickly changing numbers leave a milky trail.");
        if (ImGui::Button("Reset to defaults")) { glass::set_jitter_positive(true); glass::set_glass_strength(100); }
        ImGui::TreePop();
    }
    // About (GPL-3.0 section 7(b) notice, see NOTICE.md).
    ImGui::Separator();
    ImGui::TextColored(grey, "%s %s by %s - GPL-3.0 (see NOTICE.md) - github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix", GLASS_NAME, GLASS_VERSION_STRING, GLASS_AUTHOR);
    ImGui::TextColored(grey, "Official downloads: GitHub releases only.");
}
}

namespace glass {
bool init_status_panel(void* reshade_module) {
    const auto get_table = reinterpret_cast<const imgui_function_table* (*)(uint32_t)>(
        GetProcAddress(static_cast<HMODULE>(reshade_module), "ReShadeGetImGuiFunctionTable"));
    if (!get_table || !(imgui_function_table_instance() = get_table(IMGUI_VERSION_NUM))) return false;
    reshade::register_overlay(GLASS_NAME, draw);
    return true;
}
void uninit_status_panel() {
    if (imgui_function_table_instance()) reshade::unregister_overlay(GLASS_NAME, draw);
}
}

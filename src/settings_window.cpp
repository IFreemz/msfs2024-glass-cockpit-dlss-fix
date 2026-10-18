// Settings window of the standalone fix (dxgi.dll), opened and closed with a hotkey (Ctrl+Shift+F6 by default,
// changeable in the window): the settings and status of the add-on's ReShade tab (status_panel.cpp) in a small window of
// its own on the desktop. Deliberately not a menu drawn into the sim's picture: ReShade, OptiScaler and OptiShade draw
// their menus there and take the sim's keyboard and mouse while they are open, and a second one would compete with them.
// The window is drawn on the CPU with GDI+ on its own thread: no Direct3D, no rendering or input hooks, nothing for other
// mods to attach to, and no work at all while it's closed. It repaints only when something changes.
// It never takes the keyboard focus from the sim (WS_EX_NOACTIVATE): clicks still work, but the sim stays the active
// window. When another window of its own process took the focus, the sim stopped taking camera and mouse input, also
// after getting the focus back.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "host.hpp"
#include "status.hpp"
#include "version.h"

namespace {
HMODULE self = nullptr;
std::atomic<bool> thread_started{false};
std::atomic<HWND> window{nullptr};
constexpr UINT toggle_message = WM_APP + 1;
constexpr UINT_PTR refresh_timer = 1, capture_release_timer = 2, capture_poll_timer = 3;
constexpr int client_w = 800, client_h = 768; // DIPs

// Colours (dark theme).
constexpr COLORREF background = RGB(23, 24, 27), card = RGB(34, 35, 39), card_border = RGB(46, 48, 54), row_hover = RGB(40, 42, 47);
constexpr COLORREF text_colour = RGB(232, 233, 236), muted = RGB(155, 160, 168), accent = RGB(52, 211, 153);
constexpr COLORREF control_off = RGB(62, 65, 72), button_fill = RGB(48, 51, 57), button_hover = RGB(60, 64, 71), button_down = RGB(72, 76, 84);
constexpr COLORREF good = RGB(52, 211, 153), warn = RGB(251, 191, 36), bad = RGB(248, 113, 113);
Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(size_t(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length);
    return result;
}
std::wstring format(const wchar_t* text, ...) {
    wchar_t buffer[256];
    va_list args; va_start(args, text); _vsnwprintf_s(buffer, _TRUNCATE, text, args); va_end(args);
    return buffer;
}

UINT dpi = 96;
int scale(int v) { return MulDiv(v, int(dpi), 96); }
HFONT title_font = nullptr, body_font = nullptr, small_font = nullptr, section_font = nullptr;
void create_fonts() {
    for (HFONT f : {title_font, body_font, small_font, section_font}) if (f) DeleteObject(f);
    auto make = [](int px, int weight) {
        return CreateFontW(-scale(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    title_font = make(19, FW_SEMIBOLD); body_font = make(14, FW_NORMAL); small_font = make(12, FW_NORMAL); section_font = make(12, FW_BOLD);
}

// What is shown, refreshed twice a second while the window is open.
glass::Status status;
void read_status() { status = glass::status(); }

enum Id : int {
    id_none = 0, id_enabled, id_smoothing, id_extra, id_tint, id_key, id_advanced, id_jitter, id_glass, id_reset_advanced,
    id_occlusion, id_stable, id_full_size, id_seam, id_calm, id_dark, id_soft, id_temporal,
    id_edge_trim, id_detail_clamp, id_edge_fade, id_angle, id_edge_stretch, id_reset_experimental,
    id_capture, id_log_folder, id_copy_report, id_settings_file, id_website, id_jason,
};
enum class Kind { toggle, slider, button, header, link };
struct Widget { Id id; Kind kind; RECT rect; RECT track{}; int max = 0; const wchar_t* tip = nullptr; };
std::vector<Widget> widgets; // where each control is (pixels), rebuilt on every paint
const Widget* widget_at(POINT p) {
    for (const auto& w : widgets) if (PtInRect(&w.rect, p)) return &w;
    return nullptr;
}
const Widget* widget(Id id) {
    for (const auto& w : widgets) if (w.id == id) return &w;
    return nullptr;
}

Id hover = id_none, pressed = id_none, dragging = id_none;
int drag_value = 0;
bool advanced_open = false, capturing = false;
std::wstring help_note; // a message about the hotkey, shown in the help panel until the mouse moves on

const wchar_t* const default_help = L"Hover over an option to see what it does. Changes apply instantly and are saved to glass-cockpit-dlss-fix.ini.";
const wchar_t* const tip_enabled = L"Turns the fix on and off, e.g. to compare with the sim's own picture (also Ctrl+Shift+F9).";
const wchar_t* const tip_smoothing = L"Smooths stair steps and shimmer on thin lines, like DLSS did. 0 = sharpest, 35 = default, 50 = like DLSS.";
const wchar_t* const tip_extra = L"Lets Line smoothing go up to 100 % for stubborn shimmer (e.g. in VR). Above 50 the whole screen picture gets softer than with DLSS alone.";
const wchar_t* const tip_tint = L"Tints every fixed screen green: shows which screens the fix covers (also Ctrl+Shift+F11).";
const wchar_t* const tip_key = L"The key combination that opens and closes this window. Click, then press the new combination (click again to cancel). The sim sees those keys too. "
    L"Change it if it clashes with another mod's hotkey.";
const wchar_t* const tip_advanced = L"Settings only needed for troubleshooting: only change these if asked to in a bug report.";
const wchar_t* const tip_jitter = L"Only needed if the screen text shimmers while the camera is still.";
const wchar_t* const tip_glass = L"How much of the sim's glass effects (dirt, glare, reflections, shadows) is kept on fixed screens. 100 = all (default). "
    L"Lower values can help if quickly changing numbers leave a milky trail.";
const wchar_t* const tip_seam = L"Removes thin lines where a screen's picture meets a part of the same screen that points elsewhere in its texture "
    L"(e.g. the bezel band on the FSS E-Jets).";
const wchar_t* const tip_full_size = L"Reads the screens only from their full-size picture, with more samples where a screen is small on your monitor "
    L"(also Ctrl+Shift+F7). Off: also from the smaller copies the GPU keeps for distant screens, which can shimmer when you zoom out.";
const wchar_t* const tip_stable = L"Keeps the pixels along each screen's edge from shimmering: where a screen ends is learnt over a few frames. "
    L"Turn off if a screen's edge looks cut off or too wide.";
const wchar_t* const tip_occlusion = L"Leaves the sim's own picture wherever something sits in front of a screen (a sidestick, a hand, the yoke). "
    L"Off: a screen can be drawn over things in front of it.";
const wchar_t* const tip_calm = L"Next to symbols and lines, the glass effects (dirt, glare, reflections) are drawn without their finest detail, which "
    L"flickers there. Turn off if something drawn over a screen (a highlight, a cursor) looks blurred.";
const wchar_t* const tip_dark = L"Keeps mostly black screens with thin symbols (e.g. the F/A-18E's) fixed in strong sun glare. Can also let the fix draw "
    L"over a screen whose picture it can't redraw.";
const wchar_t* const tip_soft = L"Line smoothing weighted towards each pixel's centre over a wider area: less shimmer while the view moves. Plain Line "
    L"smoothing works well on most aircraft; try this if screens still shimmer while you move your head.";
const wchar_t* const tip_temporal = L"Smooths screen text and lines over several frames, like the sim's TAA, without the trails. Try it with Line "
    L"smoothing at 0-20 for sharper text that still doesn't stair-step. Uses some extra video memory.";
const wchar_t* const tip_edge_trim = L"Leaves the outer edge of every screen to the sim's own picture, in hundredths of a render pixel. 50 calms most "
    L"flickering or shimmering screen edges; up to 150 for aircraft with stronger edge problems.";
const wchar_t* const tip_detail_clamp = L"Keeps the fixed screens within the brightness the sim itself draws around each pixel. Hides detail too thin for the "
    L"sim to show, e.g. a dashed line where a screen meets its bezel. Try 100 if you see such lines.";
const wchar_t* const tip_edge_fade = L"Fades screen edges into the sim's own picture, in hundredths of a render pixel. Can calm jaggy or crawling edges.";
const wchar_t* const tip_angle = L"Draws screens seen at a steep angle softer, like the sim does. Screens seen straight on are unchanged.";
const wchar_t* const tip_edge_stretch = L"Extends screens outwards over their frame, in hundredths of a render pixel. For screens that don't quite reach their frame.";
const wchar_t* const tip_capture = L"Saves pictures of the screens and one .zip with the pictures and both logs, next to the log: attach that to a bug "
    L"report. Use it with the problem visible on screen (also Ctrl+Shift+F8).";
const wchar_t* const tip_log = L"Opens the folder with glass-cockpit-dlss-fix.log, to attach to a bug report.";
const wchar_t* const tip_report = L"Copies a short report of what the fix is doing, to paste into a bug report.";
const wchar_t* const tip_settings = L"Opens glass-cockpit-dlss-fix.ini in Notepad, where all these settings are saved.";
const wchar_t* const tip_website = L"Opens the project's GitHub page: instructions, compatibility list and bug reports.";
const wchar_t* const tip_jason = L"Opens Jason Tsorlinis' GitHub page.";

int setting_value(Id id) {
    const auto& e = status.experimental;
    switch (id) {
    case id_smoothing: return status.line_smoothing;
    case id_glass: return status.glass_strength;
    case id_edge_trim: return e.edge_trim;
    case id_detail_clamp: return e.detail_clamp;
    case id_edge_fade: return e.edge_fade;
    case id_angle: return e.angle_softening;
    case id_edge_stretch: return e.edge_stretch;
    case id_enabled: return status.enabled;
    case id_extra: return status.extra_smoothing;
    case id_tint: return status.tint;
    case id_jitter: return !status.jitter_positive;
    case id_occlusion: return e.occlusion_check;
    case id_stable: return e.stable_edges;
    case id_full_size: return e.full_size;
    case id_seam: return e.seam_fix;
    case id_calm: return e.calm_glass;
    case id_dark: return e.dark_screens;
    case id_soft: return e.soft_smoothing;
    case id_temporal: return e.temporal_aa;
    default: return 0;
    }
}
void set_value(Id id, int v) {
    auto e = status.experimental;
    switch (id) {
    case id_enabled: glass::set_enabled(v != 0); break;
    case id_extra: glass::set_extra_smoothing(v != 0); break;
    case id_tint: glass::set_tint(v != 0); break;
    case id_jitter: glass::set_jitter_positive(v == 0); break;
    case id_smoothing: glass::set_line_smoothing(v); break;
    case id_glass: glass::set_glass_strength(v); break;
    default:
        switch (id) {
        case id_occlusion: e.occlusion_check = v; break;
        case id_stable: e.stable_edges = v; break;
        case id_full_size: e.full_size = v; break;
        case id_seam: e.seam_fix = v; break;
        case id_calm: e.calm_glass = v; break;
        case id_dark: e.dark_screens = v; break;
        case id_soft: e.soft_smoothing = v; if (v) e.temporal_aa = 0; break; // one or the other
        case id_temporal: e.temporal_aa = v; if (v) e.soft_smoothing = 0; break;
        case id_edge_trim: e.edge_trim = v; break;
        case id_detail_clamp: e.detail_clamp = v; break;
        case id_edge_fade: e.edge_fade = v; break;
        case id_angle: e.angle_softening = v; break;
        case id_edge_stretch: e.edge_stretch = v; break;
        default: return;
        }
        glass::set_experimental(e);
    }
    read_status();
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing. Shapes with GDI+ (anti-aliased), text with GDI (ClearType) afterwards.
struct Text { RECT rect; std::wstring text; HFONT font; COLORREF colour; UINT flags; };
std::vector<Text> texts;
void text(int x, int y, int w, int h, const std::wstring& s, HFONT font, COLORREF colour, UINT flags = DT_LEFT) {
    texts.push_back({{x, y, x + w, y + h}, s, font, colour, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS});
}
void rounded(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, float x, float y, float w, float h, float r, const Gdiplus::Pen* pen = nullptr) {
    Gdiplus::GraphicsPath path;
    r = std::min({r, w / 2, h / 2});
    path.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    path.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    path.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
    if (pen) g.DrawPath(pen, &path);
}

struct Painter {
    Gdiplus::Graphics& g;
    int card_x = 0, card_w = 0, y = 0;

    void card_begin(int x, int top, int w, int h, const wchar_t* title) {
        const Gdiplus::SolidBrush fill(gp(card));
        const Gdiplus::Pen border(gp(card_border), float(scale(1)));
        rounded(g, fill, float(scale(x)), float(scale(top)), float(scale(w)), float(scale(h)), float(scale(10)), &border);
        text(scale(x + 16), scale(top + 8), scale(w - 32), scale(20), title, section_font, muted);
        card_x = x; card_w = w; y = top + 32;
    }
    void hover_background(int top, int h) {
        const Gdiplus::SolidBrush fill(gp(row_hover));
        rounded(g, fill, float(scale(card_x + 6)), float(scale(top)), float(scale(card_w - 12)), float(scale(h)), float(scale(6)));
    }
    void toggle(Id id, const wchar_t* label, const wchar_t* hint, const wchar_t* tip) {
        const int h = 32, x = card_x, w = card_w;
        const RECT r{scale(x + 6), scale(y), scale(x + w - 6), scale(y + h)};
        widgets.push_back({id, Kind::toggle, r, {}, 0, tip});
        if (hover == id) hover_background(y, h);
        const bool on = setting_value(id) != 0;
        // Switch: a pill with a round knob.
        const float sw = float(scale(36)), sh = float(scale(20)), sx = float(scale(x + w - 16)) - sw, sy = float(scale(y + (h - 20) / 2));
        const Gdiplus::SolidBrush track(gp(on ? accent : control_off)), knob(gp(RGB(255, 255, 255)));
        rounded(g, track, sx, sy, sw, sh, sh / 2);
        const float k = sh - float(scale(6));
        g.FillEllipse(&knob, on ? sx + sw - k - float(scale(3)) : sx + float(scale(3)), sy + float(scale(3)), k, k);
        text(scale(x + 16), scale(y), scale(w - (hint ? 200 : 80)), scale(h), label, body_font, text_colour);
        if (hint) text(scale(x + w - 210), scale(y), scale(150), scale(h), hint, small_font, muted, DT_RIGHT);
        y += h;
    }
    void slider(Id id, const wchar_t* label, int max, const wchar_t* unit, const wchar_t* tip) {
        const int h = 32, x = card_x, w = card_w, label_w = 118, value_w = 64;
        const RECT r{scale(x + 6), scale(y), scale(x + w - 6), scale(y + h)};
        const RECT track{scale(x + 16 + label_w), scale(y), scale(x + w - 16 - value_w), scale(y + h)};
        widgets.push_back({id, Kind::slider, r, track, max, tip});
        if (hover == id || dragging == id) hover_background(y, h);
        const int value = dragging == id ? drag_value : std::clamp(setting_value(id), 0, max);
        const float tx = float(track.left), tw = float(track.right - track.left), ty = float(scale(y + h / 2)), th = float(scale(4));
        const float fraction = max ? float(value) / float(max) : 0;
        const Gdiplus::SolidBrush rail(gp(control_off)), fill(gp(accent)), knob(gp(RGB(255, 255, 255)));
        rounded(g, rail, tx, ty - th / 2, tw, th, th / 2);
        if (value) rounded(g, fill, tx, ty - th / 2, tw * fraction, th, th / 2);
        const float k = float(scale(14));
        g.FillEllipse(&knob, tx + tw * fraction - k / 2, ty - k / 2, k, k);
        text(scale(x + 16), scale(y), scale(label_w), scale(h), label, body_font, text_colour);
        const std::wstring shown = value == 0 && id != id_glass ? (id == id_smoothing ? L"sharpest" : L"off") : *unit ? format(L"%d %s", value, unit) : format(L"%d", value);
        text(scale(x + w - 16 - value_w), scale(y), scale(value_w), scale(h), shown, body_font, value ? text_colour : muted, DT_RIGHT);
        y += h;
    }
    void button(Id id, const wchar_t* label, int x, int top, int w, int h, const wchar_t* tip, bool highlight = false) {
        const RECT r{scale(x), scale(top), scale(x + w), scale(top + h)};
        widgets.push_back({id, Kind::button, r, {}, 0, tip});
        const COLORREF fill = pressed == id && hover == id ? button_down : hover == id ? button_hover : button_fill;
        const Gdiplus::SolidBrush brush(gp(fill));
        const Gdiplus::Pen outline(gp(highlight ? accent : card_border), float(scale(1)));
        rounded(g, brush, float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top), float(scale(6)), &outline);
        text(r.left, r.top, r.right - r.left, r.bottom - r.top, label, body_font, highlight ? accent : text_colour, DT_CENTER);
    }
};

void paint(HWND hwnd, HDC target) {
    RECT client; GetClientRect(hwnd, &client);
    const int width = client.right, height = client.bottom;
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    widgets.clear(); texts.clear();
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const Gdiplus::SolidBrush back(gp(background));
        g.FillRectangle(&back, 0, 0, width, height);
        Painter p{g};

        // Header: name, version, and the status line.
        text(scale(20), scale(12), scale(500), scale(28), wide(GLASS_NAME), title_font, text_colour);
        text(scale(20), scale(38), scale(500), scale(18), wide(std::string(GLASS_VERSION_STRING " - standalone")), small_font, muted);
        const COLORREF state = status.active ? good : (!status.enabled || !status.problem) ? warn : bad;
        {
            const Gdiplus::SolidBrush fill(gp(card));
            const Gdiplus::Pen border(gp(card_border), float(scale(1)));
            rounded(g, fill, float(scale(16)), float(scale(66)), float(scale(client_w - 32)), float(scale(38)), float(scale(10)), &border);
            const Gdiplus::SolidBrush dot(gp(state));
            g.FillEllipse(&dot, float(scale(30)), float(scale(80)), float(scale(10)), float(scale(10)));
            text(scale(50), scale(66), scale(client_w - 80), scale(38), wide(glass::status_summary(status)), body_font, text_colour);
        }

        // Left column.
        const int lx = 16, lw = 376, rx = 408, rw = 376, top = 116;
        p.card_begin(lx, top, lw, 32 + 4 * 32 + 8, L"DISPLAY");
        p.toggle(id_enabled, L"Fix enabled", L"Ctrl+Shift+F9", tip_enabled);
        p.slider(id_smoothing, L"Line smoothing", status.extra_smoothing ? 100 : 50, L"%", tip_smoothing);
        p.toggle(id_extra, L"Extra smoothing (up to 100 %)", nullptr, tip_extra);
        p.toggle(id_tint, L"Green tint on fixed screens", L"Ctrl+Shift+F11", tip_tint);

        int y = top + 32 + 4 * 32 + 8 + 8;
        p.card_begin(lx, y, lw, 32 + 36 + 6, L"WINDOW HOTKEY");
        text(scale(lx + 16), scale(y + 32), scale(170), scale(30), L"Open / close this window", body_font, text_colour);
        p.button(id_key, capturing ? L"Press keys... (Esc cancels)" : wide(glass::hotkey_name(status.settings_window_key)).c_str(), lx + 186, y + 32, lw - 202, 30, tip_key, capturing);
        y += 32 + 36 + 6 + 8;

        p.card_begin(lx, y, lw, 32 + 2 * 36 + 24, L"BUG REPORTS");
        const int bw = (lw - 32 - 8) / 2;
        p.button(id_capture, L"Capture screens", lx + 16, y + 32, bw, 30, tip_capture);
        p.button(id_log_folder, L"Open log folder", lx + 16 + bw + 8, y + 32, bw, 30, tip_log);
        p.button(id_copy_report, L"Copy report", lx + 16, y + 68, bw, 30, tip_report);
        p.button(id_settings_file, L"Settings file", lx + 16 + bw + 8, y + 68, bw, 30, tip_settings);
        text(scale(lx + 16), scale(y + 102), scale(lw - 32), scale(18), status.capture.empty() ? L"Ctrl+Shift+F8 captures too." : wide(status.capture),
            small_font, muted);
        y += 32 + 2 * 36 + 24 + 8;

        // Advanced: collapsed until clicked.
        const int advanced_h = advanced_open ? 32 + 3 * 32 + 10 : 36;
        {
            const Gdiplus::SolidBrush fill(gp(card));
            const Gdiplus::Pen border(gp(card_border), float(scale(1)));
            rounded(g, fill, float(scale(lx)), float(scale(y)), float(scale(lw)), float(scale(advanced_h)), float(scale(10)), &border);
            const RECT header{scale(lx), scale(y), scale(lx + lw), scale(y + 36)};
            widgets.push_back({id_advanced, Kind::header, header, {}, 0, tip_advanced});
            if (hover == id_advanced) {
                const Gdiplus::SolidBrush highlight(gp(row_hover));
                rounded(g, highlight, float(scale(lx + 6)), float(scale(y + 4)), float(scale(lw - 12)), float(scale(28)), float(scale(6)));
            }
            // Chevron: right when closed, down when open.
            const Gdiplus::Pen chevron(gp(muted), float(scale(2)));
            const float cx = float(scale(lx + lw - 28)), cy = float(scale(y + 18)), c = float(scale(4));
            if (advanced_open) { g.DrawLine(&chevron, cx - c, cy - c / 2, cx, cy + c / 2); g.DrawLine(&chevron, cx, cy + c / 2, cx + c, cy - c / 2); }
            else { g.DrawLine(&chevron, cx - c / 2, cy - c, cx + c / 2, cy); g.DrawLine(&chevron, cx + c / 2, cy, cx - c / 2, cy + c); }
            text(scale(lx + 16), scale(y), scale(lw - 60), scale(36), L"ADVANCED (TROUBLESHOOTING)", section_font, muted);
            if (advanced_open) {
                p.card_x = lx; p.card_w = lw; p.y = y + 32;
                p.toggle(id_jitter, L"Flip jitter direction", nullptr, tip_jitter);
                p.slider(id_glass, L"Glass layer", 100, L"%", tip_glass);
                p.button(id_reset_advanced, L"Reset to defaults", lx + 16, p.y + 2, 150, 28, L"Puts the advanced settings back to their defaults.");
            }
        }

        // Right column: experimental fixes.
        const int experimental_h = 32 + 8 * 32 + 6 + 5 * 32 + 44;
        p.card_begin(rx, top, rw, experimental_h, L"EXPERIMENTAL FIXES");
        p.toggle(id_occlusion, L"Occlusion check", L"on by default", tip_occlusion);
        p.toggle(id_stable, L"Stable screen edges", L"on by default", tip_stable);
        p.toggle(id_full_size, L"Full-size sampling", L"on by default", tip_full_size);
        p.toggle(id_seam, L"UV seam fix", L"on by default", tip_seam);
        p.toggle(id_calm, L"Calm glass at symbols", nullptr, tip_calm);
        p.toggle(id_dark, L"Dark screen fix", nullptr, tip_dark);
        p.toggle(id_soft, L"Soft-edged line smoothing", nullptr, tip_soft);
        p.toggle(id_temporal, L"Temporal anti-aliasing", nullptr, tip_temporal);
        p.y += 6;
        p.slider(id_edge_trim, L"Edge trim", 400, L"", tip_edge_trim);
        p.slider(id_detail_clamp, L"Detail clamp", 100, L"%", tip_detail_clamp);
        p.slider(id_edge_fade, L"Edge fade", 250, L"", tip_edge_fade);
        p.slider(id_angle, L"Angle softening", 100, L"%", tip_angle);
        p.slider(id_edge_stretch, L"Edge stretcher", 400, L"", tip_edge_stretch);
        p.button(id_reset_experimental, L"Reset experimental fixes", rx + 16, p.y + 6, 200, 28, L"Puts all experimental fixes back to their defaults.");

        // Help panel: what the option under the mouse does. Below both columns with Advanced open, so nothing moves when
        // it opens or closes.
        const int help_y = std::max(top + experimental_h, y + 32 + 3 * 32 + 10) + 10;
        {
            const Gdiplus::SolidBrush fill(gp(card));
            rounded(g, fill, float(scale(16)), float(scale(help_y)), float(scale(client_w - 32)), float(scale(58)), float(scale(10)));
            const Widget* w = widget(hover);
            const std::wstring help = !help_note.empty() ? help_note : w && w->tip ? w->tip : default_help;
            texts.push_back({{scale(32), scale(help_y + 6), scale(client_w - 32), scale(help_y + 52)}, help, body_font,
                help_note.empty() ? muted : warn, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS});
        }

        // About (GPL-3.0 section 7(b) notice, see NOTICE.md), and thanks to the standalone version's author.
        const int footer_y = help_y + 66;
        const std::wstring about = wide(std::string(GLASS_NAME " " GLASS_VERSION_STRING " by " GLASS_AUTHOR
            " - GPL-3.0 - github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix"));
        widgets.push_back({id_website, Kind::link, {scale(16), scale(footer_y), scale(client_w - 16), scale(footer_y + 18)}, {}, 0, tip_website});
        text(scale(20), scale(footer_y), scale(client_w - 40), scale(18), about, small_font, hover == id_website ? text_colour : muted);
        text(scale(client_w - 300), scale(footer_y + 18), scale(280), scale(18), L"Official downloads: GitHub and flightsim.to", small_font, muted, DT_RIGHT);
        widgets.push_back({id_jason, Kind::link, {scale(16), scale(footer_y + 18), scale(client_w - 320), scale(footer_y + 36)}, {}, 0, tip_jason});
        text(scale(20), scale(footer_y + 18), scale(client_w - 340), scale(18), L"Standalone version by Jason Tsorlinis (github.com/jtsorlinis). Thanks, Jason!",
            small_font, hover == id_jason ? text_colour : muted);
    }
    SetBkMode(dc, TRANSPARENT);
    for (auto& t : texts) {
        SelectObject(dc, t.font);
        SetTextColor(dc, t.colour);
        DrawTextW(dc, t.text.c_str(), int(t.text.size()), &t.rect, t.flags);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------------------------------------------
// The window's icon: a dark rounded square with a cockpit screen and its horizon line, drawn at the size asked for.
HICON make_icon(int size) {
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bitmap);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const float s = float(size);
        const Gdiplus::SolidBrush back(gp(RGB(30, 32, 36))), screen(gp(RGB(14, 15, 17)));
        rounded(g, back, 0, 0, s, s, s * 0.22f);
        const Gdiplus::Pen frame(gp(accent), std::max(1.0f, s / 14));
        rounded(g, screen, s * 0.18f, s * 0.2f, s * 0.64f, s * 0.6f, s * 0.08f, &frame);
        const Gdiplus::Pen horizon(gp(accent), std::max(1.0f, s / 16));
        g.DrawLine(&horizon, s * 0.28f, s * 0.56f, s * 0.72f, s * 0.44f);
        const Gdiplus::SolidBrush mark(gp(RGB(255, 255, 255)));
        g.FillRectangle(&mark, s * 0.44f, s * 0.48f, s * 0.12f, std::max(1.0f, s / 16));
    }
    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

// Dark title bar (Windows 10 2004+ / 11), in the window's background colour where supported (Windows 11).
void dark_title_bar(HWND hwnd) {
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    const auto set = dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    if (!set) return;
    const BOOL dark = TRUE;
    set(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    const COLORREF caption = background;
    set(hwnd, 35, &caption, sizeof(caption)); // DWMWA_CAPTION_COLOR
}

// ---------------------------------------------------------------------------------------------------------------
// Input.
std::wstring folder_of(const std::wstring& path) { const auto slash = path.find_last_of(L"\\/"); return slash == std::wstring::npos ? path : path.substr(0, slash); }
void copy_to_clipboard(HWND owner, const std::string& text) {
    const std::wstring w = wide(text);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (void* data = GlobalLock(memory)) {
            memcpy(data, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard();
}
void stop_capturing(HWND hwnd, bool wait_for_release) {
    capturing = false;
    KillTimer(hwnd, capture_poll_timer);
    // The new combination is still held: the hotkey stays paused until it's let go, or it would close the window at once.
    if (wait_for_release) SetTimer(hwnd, capture_release_timer, 50, nullptr);
    else glass::settings_window_capturing = false;
}
void click(HWND hwnd, Id id) {
    switch (id) {
    case id_advanced: advanced_open = !advanced_open; break;
    case id_key:
        if (capturing) { stop_capturing(hwnd, false); break; }
        capturing = true;
        glass::settings_window_capturing = true;
        help_note.clear();
        SetTimer(hwnd, capture_poll_timer, 15, nullptr); // the keys go to the sim: they're read from the keyboard's state
        break;
    case id_reset_advanced: glass::set_jitter_positive(true); glass::set_glass_strength(100); break;
    case id_reset_experimental: glass::set_experimental({}); break;
    case id_capture: glass::request_capture(); break;
    case id_log_folder: {
        const std::wstring arguments = L"/select,\"" + status.log_path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case id_copy_report: copy_to_clipboard(hwnd, glass::status_report(status)); help_note = L"Report copied: paste it into your bug report."; break;
    case id_settings_file: {
        const std::wstring path = L"\"" + folder_of(status.log_path) + L"\\glass-cockpit-dlss-fix.ini\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case id_website: ShellExecuteW(nullptr, L"open", wide(GLASS_WEBSITE).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case id_jason: ShellExecuteW(nullptr, L"open", L"https://github.com/jtsorlinis", nullptr, nullptr, SW_SHOWNORMAL); break;
    default: return;
    }
    read_status();
}
int slider_value(const Widget& w, int x) {
    const int left = w.track.left, right = w.track.right;
    if (right <= left) return 0;
    return int(std::lround(double(std::clamp(x, left, right) - left) / double(right - left) * w.max));
}
void key_pressed(HWND hwnd, UINT vk) {
    if (vk == VK_ESCAPE) { stop_capturing(hwnd, false); return; }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) return; // waiting for the key itself
    uint32_t key = vk & 0xFF;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) key |= glass::key_ctrl;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) key |= glass::key_shift;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) key |= glass::key_alt;
    const std::string name = glass::hotkey_name(key);
    if (name.find('?') != std::string::npos || glass::parse_hotkey(name) != key) {
        help_note = L"That key can't be used for the hotkey: try an F key, a letter or a number with Ctrl, Shift or Alt.";
    } else if (const char* problem = glass::hotkey_problem(key)) {
        help_note = wide(name) + L" can't be used: " + wide(problem) + L".";
    } else {
        glass::set_settings_window_key(key);
        help_note = L"This window now opens and closes with " + wide(name) + L".";
        stop_capturing(hwnd, true);
        read_status();
    }
}

void show(HWND hwnd, bool visible) {
    if (visible) {
        read_status();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE); // the sim keeps the focus
        SetTimer(hwnd, refresh_timer, 500, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        if (capturing) stop_capturing(hwnd, false);
        KillTimer(hwnd, refresh_timer);
        dragging = pressed = hover = id_none;
        help_note.clear();
        ShowWindow(hwnd, SW_HIDE);
    }
}

// What the window shows, to repaint only when it changes.
std::string shown_state() {
    char buffer[512];
    const auto& e = status.experimental;
    snprintf(buffer, sizeof(buffer), "%d%d%d%d%d %d %d %u|%d%d%d%d%d%d%d%d %d %d %d %d %d|", status.active, status.enabled, status.tint, status.extra_smoothing,
        status.jitter_positive, status.line_smoothing, status.glass_strength, status.settings_window_key, e.occlusion_check, e.stable_edges, e.full_size,
        e.seam_fix, e.calm_glass, e.dark_screens, e.soft_smoothing, e.temporal_aa, e.edge_trim, e.detail_clamp, e.edge_fade, e.angle_softening, e.edge_stretch);
    return buffer + glass::status_summary(status) + "|" + status.capture;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case toggle_message: show(hwnd, !IsWindowVisible(hwnd)); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // clicks never take the focus from the sim
    case WM_TIMER:
        if (w == refresh_timer) {
            const auto before = shown_state();
            read_status();
            if (shown_state() != before) InvalidateRect(hwnd, nullptr, FALSE);
        } else if (w == capture_poll_timer) {
            // Recording a new hotkey: the first key pressed since the last check (besides the modifiers and the mouse).
            static std::array<bool, 256> was_down{};
            static bool primed = false;
            if (!capturing) { primed = false; return 0; }
            for (int vk = 0x08; vk < 0xFF; ++vk) {
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                const bool fresh = primed && down && !was_down[size_t(vk)];
                was_down[size_t(vk)] = down;
                if (fresh) { key_pressed(hwnd, UINT(vk)); InvalidateRect(hwnd, nullptr, FALSE); if (!capturing) break; }
            }
            primed = capturing; // the keys held when recording started don't count
            if (!capturing) primed = false;
        } else if (w == capture_release_timer) {
            const uint32_t key = status.settings_window_key;
            if (!(GetAsyncKeyState(int(key & 0xFF)) & 0x8000)) { KillTimer(hwnd, capture_release_timer); glass::settings_window_capturing = false; }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(hwnd, &ps);
        paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1; // painted in full by WM_PAINT
    case WM_MOUSEMOVE: {
        const POINT p{short(LOWORD(l)), short(HIWORD(l))};
        if (dragging != id_none) {
            if (const Widget* s = widget(dragging)) { drag_value = slider_value(*s, p.x); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        const Widget* at = widget_at(p);
        const Id now = at ? at->id : id_none;
        if (now != hover) {
            hover = now;
            if (!capturing) help_note.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (dragging == id_none && hover != id_none) { hover = id_none; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT p{short(LOWORD(l)), short(HIWORD(l))};
        const Widget* at = widget_at(p);
        if (!at) return 0;
        SetCapture(hwnd);
        if (at->kind == Kind::slider) { dragging = at->id; drag_value = slider_value(*at, p.x); }
        else pressed = at->id;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const POINT p{short(LOWORD(l)), short(HIWORD(l))};
        if (dragging != id_none) {
            // The setting changes when the slider is let go, so a drag doesn't write the settings file dozens of times.
            const Id id = dragging;
            dragging = id_none;
            if (drag_value != setting_value(id)) set_value(id, drag_value);
        } else if (pressed != id_none) {
            const Widget* at = widget_at(p);
            const Id id = pressed;
            pressed = id_none;
            if (at && at->id == id) {
                if (at->kind == Kind::toggle) set_value(id, setting_value(id) ? 0 : 1);
                else click(hwnd, id);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{short(LOWORD(l)), short(HIWORD(l))};
        ScreenToClient(hwnd, &p);
        const Widget* at = widget_at(p);
        if (at && at->kind == Kind::slider && dragging == id_none) {
            const int step = at->max >= 200 ? 5 : 1, notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
            const int value = std::clamp(setting_value(at->id) + notches * step, 0, at->max);
            if (value != setting_value(at->id)) { set_value(at->id, value); InvalidateRect(hwnd, nullptr, FALSE); }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(widget_at(p) ? 32649 : 32512))); // IDC_HAND : IDC_ARROW
            return TRUE;
        }
        break;
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        create_fonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(l);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE: show(hwnd, false); return 0; // only hidden: the hotkey opens it again
    default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

DWORD WINAPI window_thread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ULONG_PTR gdiplus = 0;
    const Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&gdiplus, &startup, nullptr) != Gdiplus::Ok) { thread_started = false; return 0; }

    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = self;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    type.lpszClassName = L"GlassCockpitDlssFixSettings";
    RegisterClassExW(&type);

    // Centred on the monitor the sim is on, above the sim (which may cover the whole monitor).
    const HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT monitor_dpi_x = 96, monitor_dpi_y = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (const HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        if (const auto get = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) get(monitor, 0, &monitor_dpi_x, &monitor_dpi_y);
    dpi = monitor_dpi_x;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, ex_style = WS_EX_TOPMOST | WS_EX_NOACTIVATE; // fixed size; never takes the focus
    RECT frame{0, 0, scale(client_w), scale(client_h)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, dpi);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    const RECT& area = info.rcWork;
    const HWND hwnd = CreateWindowExW(ex_style, type.lpszClassName, L"Glass Cockpit DLSS Fix", style,
        area.left + std::max(0L, (area.right - area.left - w) / 2), area.top + std::max(0L, (area.bottom - area.top - h) / 2), w, h, nullptr, nullptr, self, nullptr);
    if (!hwnd) { Gdiplus::GdiplusShutdown(gdiplus); thread_started = false; return 0; }
    dpi = GetDpiForWindow(hwnd);
    create_fonts();
    dark_title_bar(hwnd);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(make_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi))));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(make_icon(GetSystemMetricsForDpi(SM_CXICON, dpi))));
    window = hwnd;
    glass::write_log("settings window opened");
    show(hwnd, true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void toggle() {
    // A second press within 300 ms is taken as a double press, not as "close it again".
    static std::atomic<uint64_t> last{0};
    const uint64_t now = GetTickCount64();
    if (now - last.exchange(now) < 300) return;
    if (const HWND hwnd = window.load()) { PostMessageW(hwnd, toggle_message, 0, 0); return; }
    if (thread_started.exchange(true)) return; // being created
    if (const HANDLE thread = CreateThread(nullptr, 0, window_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    else thread_started = false;
}
}

namespace glass {
void init_settings_window(void* module) {
    self = static_cast<HMODULE>(module);
    settings_window_hotkey = toggle;
}
}

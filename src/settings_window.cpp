// Settings window of the standalone fix (dxgi.dll), opened and closed with Ctrl+Shift+F6: the settings and status of
// the add-on's ReShade tab (status_panel.cpp) in a small window of its own on the desktop. Deliberately not a menu drawn
// into the sim's picture: ReShade, OptiScaler and OptiShade draw their menus there and take the sim's keyboard and
// mouse while they are open, and a second one would compete with them. This window needs no rendering hooks, no
// input hooks and no ImGui, so it works next to any of them (and in VR, on the desktop).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
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
HWND previous_foreground = nullptr; // given the focus back when the window closes
constexpr UINT toggle_message = WM_APP + 1;
constexpr UINT_PTR refresh_timer = 1;

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

// Controls, laid out in DIPs (96 DPI) and scaled to the window's DPI.
enum Id : int {
    id_none = 0, id_enabled = 100, id_smoothing, id_extra, id_tint, id_jitter, id_glass, id_reset_advanced,
    id_seam, id_full_size, id_stable, id_occlusion, id_calm, id_dark, id_soft, id_temporal,
    id_edge_trim, id_detail_clamp, id_edge_fade, id_angle, id_edge_stretch, id_reset_experimental,
    id_capture, id_log_folder, id_copy_report, id_settings_file,
};
struct Control { HWND hwnd = nullptr; RECT dip{}; bool bold = false; };
std::vector<Control> controls;
HWND status_text = nullptr, capture_text = nullptr, tooltip = nullptr;
HFONT font = nullptr, bold_font = nullptr;
UINT dpi = 96;
int scale(int v) { return MulDiv(v, int(dpi), 96); }

// Sliders: the setting each one changes, and how its value reads.
struct Slider { int id; HWND bar = nullptr, value = nullptr; int max; const wchar_t* unit; };
std::vector<Slider> sliders;
Slider* slider(int id) { for (auto& s : sliders) if (s.id == id) return &s; return nullptr; }

HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, const wchar_t* tip = nullptr, bool bold = false) {
    const HWND parent = window.load();
    const HWND hwnd = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(w), scale(h), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self, nullptr);
    controls.push_back({hwnd, {x, y, x + w, y + h}, bold});
    if (tip && tooltip) {
        TTTOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = parent;
        info.uId = reinterpret_cast<UINT_PTR>(hwnd);
        info.lpszText = const_cast<wchar_t*>(tip);
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    return hwnd;
}
HWND group(const wchar_t* text, int x, int y, int w, int h) { return add(L"BUTTON", text, BS_GROUPBOX, id_none, x, y, w, h); }
HWND checkbox(int id, const wchar_t* text, int x, int y, int w, const wchar_t* tip) {
    return add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, w, 20, tip);
}
HWND button(int id, const wchar_t* text, int x, int y, int w, const wchar_t* tip = nullptr) {
    return add(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, x, y, w, 26, tip);
}
void add_slider(int id, const wchar_t* label, int x, int y, int max, const wchar_t* unit, const wchar_t* tip, int label_w = 110) {
    add(L"STATIC", label, SS_NOTIFY | SS_CENTERIMAGE, id_none, x, y, label_w, 24, tip);
    Slider s{id, nullptr, nullptr, max, unit};
    s.bar = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id, x + label_w, y, 160, 24, tip);
    SendMessageW(s.bar, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(s.bar, TBM_SETRANGEMAX, TRUE, max);
    SendMessageW(s.bar, TBM_SETPAGESIZE, 0, std::max(max / 20, 1));
    s.value = add(L"STATIC", L"", SS_CENTERIMAGE, id_none, x + label_w + 164, y, 90, 24);
    sliders.push_back(s);
}

const wchar_t* const tip_enabled = L"Turns the fix on and off (Ctrl+Shift+F9), e.g. to compare with the sim's own picture.";
const wchar_t* const tip_smoothing = L"Smooths stair steps and shimmer on thin lines, like DLSS did. 0 = sharpest, 35 = default, 50 = like DLSS.";
const wchar_t* const tip_extra = L"Lets Line smoothing go up to 100 % for stubborn shimmer (e.g. in VR). Above 50 the whole screen picture gets softer than with DLSS alone.";
const wchar_t* const tip_tint = L"Tints every fixed screen green (Ctrl+Shift+F11): shows which screens the fix covers.";
const wchar_t* const tip_jitter = L"Only needed if the screen text shimmers while the camera is still.";
const wchar_t* const tip_glass = L"How much of the sim's glass effects (dirt, glare, reflections, shadows) is kept on fixed screens. 100 = all (default). "
    L"Lower values can help if quickly changing numbers leave a milky trail.";
const wchar_t* const tip_seam = L"Removes thin lines where a screen's picture meets a part of the same screen that points elsewhere in its texture "
    L"(e.g. the bezel band on the FSS E-Jets).";
const wchar_t* const tip_full_size = L"Reads the screens only from their full-size picture, with more samples where a screen is small on your monitor (Ctrl+Shift+F7). "
    L"Off: also from the smaller copies the GPU keeps for distant screens, which can shimmer when you zoom out.";
const wchar_t* const tip_stable = L"Keeps the pixels along each screen's edge from shimmering: where a screen ends is learnt over a few frames instead of judged anew "
    L"every frame. Turn off if a screen's edge looks cut off or too wide.";
const wchar_t* const tip_occlusion = L"Leaves the sim's own picture wherever something sits in front of a screen (a sidestick, a hand, the yoke), using the sim's "
    L"depth buffer. Off: a screen can be drawn over things in front of it.";
const wchar_t* const tip_calm = L"Right next to symbols and lines, the glass effects (dirt, glare, reflections) are drawn without their finest detail, which "
    L"flickers there. Away from symbols nothing changes. Turn off if something drawn over a screen (a highlight, a cursor) looks blurred.";
const wchar_t* const tip_dark = L"Keeps mostly black screens with thin symbols (e.g. the F/A-18E's) fixed in strong sun glare. Can also let the fix draw over a "
    L"screen whose picture it can't redraw.";
const wchar_t* const tip_soft = L"Line smoothing weighted towards each pixel's centre over a wider area: smoother screens with less shimmer while the view moves. "
    L"Plain Line smoothing works well on most aircraft; try this if screens still shimmer while you move your head. Needs Line smoothing above 0.";
const wchar_t* const tip_temporal = L"Smooths screen text and lines over several frames, like the sim's own TAA, without the trails. Plain Line smoothing works well "
    L"on most aircraft; try this if you want sharper text that still doesn't stair-step, with Line smoothing at 0-20. Uses some extra video memory.";
const wchar_t* const tip_edge_trim = L"Leaves the outer edge of every screen to the sim's own picture, in hundredths of a render pixel (100 = 1 pixel). 50 calms most "
    L"flickering or shimmering screen edges; up to 150 for aircraft with stronger edge problems. Within that band the upscaler's trails come back.";
const wchar_t* const tip_detail_clamp = L"Keeps the fixed screens within the brightness the sim itself draws around each pixel. Hides texture detail too thin for the "
    L"sim to show, e.g. a dashed line where a screen's picture meets its bezel. Try 100 if you see such lines.";
const wchar_t* const tip_edge_fade = L"Fades screen edges into the sim's own picture, in hundredths of a render pixel (100 = 1 pixel). Can calm jaggy or crawling "
    L"screen edges. Within that band the upscaler's trails come back a little.";
const wchar_t* const tip_angle = L"Draws screens seen at a steep angle softer, like the sim does. Screens seen straight on are unchanged.";
const wchar_t* const tip_edge_stretch = L"Extends screens outwards over their frame, in hundredths of a render pixel (100 = 1 pixel), continuing their picture. "
    L"For screens that don't quite reach their frame.";
const wchar_t* const tip_capture = L"Saves pictures of the screens next to the log and one .zip with the pictures and both logs: attach that to a bug report. "
    L"Use it with the problem visible on screen.";

void create_controls() {
    controls.clear(); sliders.clear();
    // Left column: status, main settings, advanced, bug reports.
    add(L"STATIC", wide(std::string(GLASS_NAME " " GLASS_VERSION_STRING " (standalone)")).c_str(), 0, id_none, 12, 10, 340, 22, nullptr, true);
    status_text = add(L"STATIC", L"", 0, id_none, 12, 34, 340, 38);
    group(L"Settings", 12, 76, 340, 128);
    checkbox(id_enabled, L"Fix enabled  (Ctrl+Shift+F9)", 24, 96, 310, tip_enabled);
    add_slider(id_smoothing, L"Line smoothing", 24, 120, 50, L"%", tip_smoothing, 96);
    checkbox(id_extra, L"Extra (lets Line smoothing go up to 100 %)", 24, 148, 310, tip_extra);
    checkbox(id_tint, L"Green tint on fixed screens  (Ctrl+Shift+F11)", 24, 172, 310, tip_tint);
    group(L"Advanced (only change if asked to in a bug report)", 12, 212, 340, 112);
    checkbox(id_jitter, L"Flip jitter direction", 24, 232, 310, tip_jitter);
    add_slider(id_glass, L"Glass layer", 24, 256, 100, L"%", tip_glass, 96);
    button(id_reset_advanced, L"Reset to defaults", 24, 288, 130);
    group(L"Bug reports", 12, 332, 340, 108);
    button(id_capture, L"Capture screens  (Ctrl+Shift+F8)", 24, 352, 220, tip_capture);
    button(id_log_folder, L"Open log folder", 24, 384, 100);
    button(id_copy_report, L"Copy report", 130, 384, 100, L"Copies a short report of what the fix is doing, to paste into a bug report.");
    button(id_settings_file, L"Settings file", 236, 384, 104, L"Opens glass-cockpit-dlss-fix.ini, where all these settings are saved.");
    capture_text = add(L"STATIC", L"", 0, id_none, 24, 414, 320, 20);

    // Right column: experimental fixes.
    group(L"Experimental fixes (for problems on some aircraft)", 364, 10, 384, 430);
    int y = 30;
    checkbox(id_occlusion, L"Occlusion check (on by default)", 376, y, 360, tip_occlusion); y += 24;
    checkbox(id_stable, L"Stable screen edges (on by default)", 376, y, 360, tip_stable); y += 24;
    checkbox(id_full_size, L"Full-size sampling (on by default)  (Ctrl+Shift+F7)", 376, y, 360, tip_full_size); y += 24;
    checkbox(id_seam, L"UV seam fix (on by default)", 376, y, 360, tip_seam); y += 24;
    checkbox(id_calm, L"Calm glass at symbols", 376, y, 360, tip_calm); y += 24;
    checkbox(id_dark, L"Dark screen fix", 376, y, 360, tip_dark); y += 24;
    checkbox(id_soft, L"Soft-edged line smoothing", 376, y, 360, tip_soft); y += 24;
    checkbox(id_temporal, L"Temporal anti-aliasing", 376, y, 360, tip_temporal); y += 32;
    add_slider(id_edge_trim, L"Edge trim", 376, y, 400, L"", tip_edge_trim); y += 32;
    add_slider(id_detail_clamp, L"Detail clamp", 376, y, 100, L"%", tip_detail_clamp); y += 32;
    add_slider(id_edge_fade, L"Edge fade", 376, y, 250, L"", tip_edge_fade); y += 32;
    add_slider(id_angle, L"Angle softening", 376, y, 100, L"%", tip_angle); y += 32;
    add_slider(id_edge_stretch, L"Edge stretcher", 376, y, 400, L"", tip_edge_stretch); y += 36;
    button(id_reset_experimental, L"Reset experimental fixes", 376, y, 180);

    add(L"STATIC", L"Hover over an option to see what it does. Changes apply instantly and are saved to glass-cockpit-dlss-fix.ini. "
        L"Ctrl+Shift+F6 or Esc closes this window.", 0, id_none, 12, 448, 736, 34);
    // About (GPL-3.0 section 7(b) notice, see NOTICE.md), and thanks to the standalone version's author.
    add(L"STATIC", wide(std::string(GLASS_NAME " " GLASS_VERSION_STRING " by " GLASS_AUTHOR " - GPL-3.0 (see NOTICE.md) - "
        "github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix - official downloads: GitHub releases only")).c_str(), 0, id_none, 12, 484, 736, 20);
    add(L"STATIC", L"Standalone version by Jason Tsorlinis (github.com/jtsorlinis). Thanks, Jason!", 0, id_none, 12, 504, 736, 20);
}
constexpr int client_w = 760, client_h = 532;

void apply_fonts() {
    if (font) DeleteObject(font);
    if (bold_font) DeleteObject(bold_font);
    font = CreateFontW(-MulDiv(9, int(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    bold_font = CreateFontW(-MulDiv(10, int(dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto& c : controls) SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(c.bold ? bold_font : font), TRUE);
    if (tooltip) {
        SendMessageW(tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, scale(420));
    }
}
void layout() {
    for (const auto& c : controls)
        SetWindowPos(c.hwnd, nullptr, scale(c.dip.left), scale(c.dip.top), scale(c.dip.right - c.dip.left), scale(c.dip.bottom - c.dip.top), SWP_NOZORDER | SWP_NOACTIVATE);
    apply_fonts();
}

// Controls follow the settings (also when changed by a hotkey or in the .ini file), except one being dragged.
void set_check(int id, bool on) {
    const HWND hwnd = GetDlgItem(window.load(), id);
    if (hwnd && (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED) != on) SendMessageW(hwnd, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
bool checked(int id) { return SendMessageW(GetDlgItem(window.load(), id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void show_value(Slider& s, int value) {
    const std::wstring text = value == 0 ? (s.id == id_smoothing ? L"off (sharpest)" : s.id == id_glass ? L"0 %" : L"off")
        : *s.unit ? format(L"%d %s", value, s.unit) : format(L"%d", value);
    wchar_t current[64]{};
    GetWindowTextW(s.value, current, 64);
    if (text != current) SetWindowTextW(s.value, text.c_str());
}
void set_slider(int id, int value, int max = -1) {
    Slider* s = slider(id);
    if (!s) return;
    if (max >= 0 && max != s->max) { s->max = max; SendMessageW(s->bar, TBM_SETRANGEMAX, TRUE, max); }
    if (GetCapture() != s->bar && SendMessageW(s->bar, TBM_GETPOS, 0, 0) != value) SendMessageW(s->bar, TBM_SETPOS, TRUE, value);
    show_value(*s, value);
}
COLORREF status_colour = RGB(0, 0, 0);
void refresh() {
    const auto s = glass::status();
    const std::wstring summary = wide(glass::status_summary(s));
    wchar_t current[512]{};
    GetWindowTextW(status_text, current, 512);
    const COLORREF colour = s.active ? RGB(20, 130, 40) : (!s.enabled || !s.problem) ? RGB(170, 110, 0) : RGB(190, 30, 30);
    if (summary != current || colour != status_colour) { status_colour = colour; SetWindowTextW(status_text, summary.c_str()); InvalidateRect(status_text, nullptr, TRUE); }
    GetWindowTextW(capture_text, current, 512);
    if (wide(s.capture) != current) SetWindowTextW(capture_text, wide(s.capture).c_str());
    set_check(id_enabled, s.enabled);
    set_check(id_extra, s.extra_smoothing);
    set_check(id_tint, s.tint);
    set_check(id_jitter, !s.jitter_positive);
    set_slider(id_smoothing, s.line_smoothing, s.extra_smoothing ? 100 : 50);
    set_slider(id_glass, s.glass_strength);
    const auto& e = s.experimental;
    set_check(id_seam, e.seam_fix != 0); set_check(id_full_size, e.full_size != 0); set_check(id_stable, e.stable_edges != 0);
    set_check(id_occlusion, e.occlusion_check != 0); set_check(id_calm, e.calm_glass != 0); set_check(id_dark, e.dark_screens != 0);
    set_check(id_soft, e.soft_smoothing != 0); set_check(id_temporal, e.temporal_aa != 0);
    set_slider(id_edge_trim, e.edge_trim); set_slider(id_detail_clamp, e.detail_clamp); set_slider(id_edge_fade, e.edge_fade);
    set_slider(id_angle, e.angle_softening); set_slider(id_edge_stretch, e.edge_stretch);
}

// The experimental fixes as the controls show them.
glass::Experimental experimental_from_controls() {
    glass::Experimental e = glass::status().experimental;
    e.seam_fix = checked(id_seam); e.full_size = checked(id_full_size); e.stable_edges = checked(id_stable);
    e.occlusion_check = checked(id_occlusion); e.calm_glass = checked(id_calm); e.dark_screens = checked(id_dark);
    e.soft_smoothing = checked(id_soft); e.temporal_aa = checked(id_temporal);
    auto pos = [](int id) { return int(SendMessageW(slider(id)->bar, TBM_GETPOS, 0, 0)); };
    e.edge_trim = pos(id_edge_trim); e.detail_clamp = pos(id_detail_clamp); e.edge_fade = pos(id_edge_fade);
    e.angle_softening = pos(id_angle); e.edge_stretch = pos(id_edge_stretch);
    return e;
}
void slider_changed(int id, int value) {
    if (id == id_smoothing) glass::set_line_smoothing(value);
    else if (id == id_glass) glass::set_glass_strength(value);
    else glass::set_experimental(experimental_from_controls());
}

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
std::wstring folder_of(const std::wstring& path) { const auto slash = path.find_last_of(L"\\/"); return slash == std::wstring::npos ? path : path.substr(0, slash); }

void command(HWND hwnd, int id) {
    switch (id) {
    case id_enabled: glass::set_enabled(checked(id_enabled)); break;
    case id_extra: glass::set_extra_smoothing(checked(id_extra)); break;
    case id_tint: glass::set_tint(checked(id_tint)); break;
    case id_jitter: glass::set_jitter_positive(!checked(id_jitter)); break;
    case id_reset_advanced: glass::set_jitter_positive(true); glass::set_glass_strength(100); break;
    case id_soft: if (checked(id_soft)) set_check(id_temporal, false); glass::set_experimental(experimental_from_controls()); break; // one or the other
    case id_temporal: if (checked(id_temporal)) set_check(id_soft, false); glass::set_experimental(experimental_from_controls()); break;
    case id_seam: case id_full_size: case id_stable: case id_occlusion: case id_calm: case id_dark:
        glass::set_experimental(experimental_from_controls()); break;
    case id_reset_experimental: glass::set_experimental({}); break;
    case id_capture: glass::request_capture(); break;
    case id_log_folder: {
        const std::wstring arguments = L"/select,\"" + glass::status().log_path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case id_copy_report: copy_to_clipboard(hwnd, glass::status_report(glass::status())); break;
    case id_settings_file: {
        const std::wstring path = folder_of(glass::status().log_path) + L"\\glass-cockpit-dlss-fix.ini";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    default: return;
    }
    refresh();
}

void show(HWND hwnd, bool visible) {
    if (visible) {
        const HWND foreground = GetForegroundWindow();
        if (foreground != hwnd) previous_foreground = foreground;
        refresh();
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        SetTimer(hwnd, refresh_timer, 500, nullptr);
    } else {
        KillTimer(hwnd, refresh_timer);
        ShowWindow(hwnd, SW_HIDE);
        if (previous_foreground && IsWindow(previous_foreground)) SetForegroundWindow(previous_foreground); // back to the sim
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case toggle_message: show(hwnd, !IsWindowVisible(hwnd)); return 0;
    case WM_TIMER: if (w == refresh_timer) refresh(); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL) { show(hwnd, false); return 0; }
        if (HIWORD(w) == BN_CLICKED) command(hwnd, LOWORD(w));
        return 0;
    case WM_HSCROLL:
        for (auto& s : sliders) {
            if (reinterpret_cast<HWND>(l) != s.bar) continue;
            const int value = int(SendMessageW(s.bar, TBM_GETPOS, 0, 0));
            show_value(s, value);
            // While the thumb is dragged only the number follows; the setting changes when it is let go (or on each
            // key press / click), so a drag doesn't write the settings file dozens of times.
            if (LOWORD(w) != TB_THUMBTRACK) slider_changed(s.id, value);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(l) == status_text) {
            SetTextColor(reinterpret_cast<HDC>(w), status_colour);
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        const RECT* suggested = reinterpret_cast<const RECT*>(l);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_CLOSE: show(hwnd, false); return 0; // only hidden: Ctrl+Shift+F6 opens it again
    default: break;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

// Visual styles (themed buttons, sliders) for this window only: the manifest resource in this DLL (version.rc) is
// activated on this window's thread, so the sim's own windows are unaffected.
void activate_visual_styles() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    ACTCTXW context{sizeof(context)};
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.lpSource = path;
    context.hModule = self;
    context.lpResourceName = MAKEINTRESOURCEW(3);
    const HANDLE handle = CreateActCtxW(&context);
    ULONG_PTR cookie = 0;
    if (handle != INVALID_HANDLE_VALUE) ActivateActCtx(handle, &cookie); // kept active for the thread's lifetime
}

DWORD WINAPI window_thread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    activate_visual_styles();
    if (const HMODULE comctl = LoadLibraryW(L"comctl32.dll")) {
        using Init = BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*);
        if (const auto init = reinterpret_cast<Init>(GetProcAddress(comctl, "InitCommonControlsEx"))) {
            const INITCOMMONCONTROLSEX classes{sizeof(classes), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
            init(&classes);
        }
    }
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = self;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
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
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, ex_style = WS_EX_TOPMOST | WS_EX_CONTROLPARENT;
    RECT frame{0, 0, scale(client_w), scale(client_h)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, dpi);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    const RECT& area = info.rcWork;
    const HWND hwnd = CreateWindowExW(ex_style, type.lpszClassName, L"Glass Cockpit DLSS Fix - settings", style,
        area.left + (area.right - area.left - w) / 2, area.top + (area.bottom - area.top - h) / 2, w, h, nullptr, nullptr, self, nullptr);
    if (!hwnd) { thread_started = false; return 0; }
    window = hwnd;
    dpi = GetDpiForWindow(hwnd);
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, self, nullptr);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    create_controls();
    layout();
    glass::write_log("settings window opened (Ctrl+Shift+F6)");
    show(hwnd, true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &message)) continue; // Tab between controls, Esc closes
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void toggle() {
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

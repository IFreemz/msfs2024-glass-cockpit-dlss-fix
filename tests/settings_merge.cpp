// Exercise the real settings implementation without starting hooks or background threads.
// GPL-3.0-only with the additional terms in NOTICE.md.
#include "../src/glass_cockpit_dlss_fix.cpp"
#include <stdexcept>

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) try {
    check(argc == 2, "Provide a disposable test directory");
    addon_directory = std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(addon_directory), "Test directory must not already exist");
    std::filesystem::create_directories(addon_directory);
    write_settings_file(); settings_changed();
    auto external = [](const wchar_t* section, const wchar_t* key, const wchar_t* value) {
        check(WritePrivateProfileStringW(section, key, value, settings_path().c_str()) != FALSE, "Write INI");
        // Force a distinct timestamp even on filesystems with coarse write-time resolution.
        const HANDLE file = CreateFileW(settings_path().c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        check(file != INVALID_HANDLE_VALUE, "Open timestamp");
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER time{}; time.LowPart = now.dwLowDateTime; time.HighPart = now.dwHighDateTime;
        time.QuadPart += 10000000;
        now = {time.LowPart, time.HighPart};
        const bool ok = SetFileTime(file, nullptr, nullptr, &now) != FALSE;
        CloseHandle(file); check(ok, "Set timestamp");
    };
    auto read = [](const wchar_t* section, const wchar_t* key) {
        return GetPrivateProfileIntW(section, key, 999, settings_path().c_str());
    };
    external(L"overlay", L"line_smoothing", L"17");
    glass::set_glass_strength(73); // before reload_settings's one-second poll
    check(smoothing_percent == 17 && read(L"overlay", L"line_smoothing") == 17, "Pending smoothing lost");
    check(glass_percent == 73 && read(L"advanced", L"glass_strength") == 73, "Local edit lost");
    external(L"overlay", L"extra_smoothing", L"1");
    glass::set_line_smoothing(85);
    check(smoothing_percent == 85, "Clamp used stale Extra limit");
    glass::set_enabled(false);
    const auto before = generation.load();
    external(L"overlay", L"enabled", L"1");
    glass::set_glass_strength(72);
    check(enabled && generation == before + 1, "External enable must invalidate recorded lists");
    external(L"overlay", L"enabled", L"0");
    glass::set_enabled(true); // previously returned early using stale enabled=true
    check(enabled && generation == before + 2 && read(L"overlay", L"enabled") == 1, "Local enable used stale state");

    const auto snapshot = glass::status().experimental;
    auto edited = snapshot; edited.dark_screens = 1;
    external(L"experimental", L"edge_trim", L"12");
    glass::set_experimental(edited, &snapshot);
    check(edge_trim_percent == 12 && dark_screen_fix == 1, "Experimental snapshot overwrote pending field");
    const auto temporal_snapshot = glass::status().experimental;
    auto temporal_edit = temporal_snapshot; temporal_edit.temporal_aa = 1;
    external(L"experimental", L"soft_smoothing", L"1");
    glass::set_experimental(temporal_edit, &temporal_snapshot);
    check(temporal_aa == 1 && soft_smoothing == 0, "Explicit temporal mode lost to pending soft mode");
    external(L"experimental", L"detail_clamp", L"80");
    glass::set_experimental({}); // explicit reset still replaces all fields
    check(glass::status().experimental == glass::Experimental{}, "Reset defaults changed");
    external(L"overlay", L"enabled", L"0"); reload_settings();
    check(!enabled, "Background reload failed");
    std::puts("Settings merge, enable generations, changed-field updates and reset: PASS");
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }

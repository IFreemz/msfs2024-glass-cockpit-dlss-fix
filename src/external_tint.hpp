// Glass Cockpit DLSS Fix by IFreemz — https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix
// GPL-3.0-only with the additional terms in NOTICE.md.
#pragma once
#include <Windows.h>
#include <string>

namespace glass::external_tint {
// Local-session, per-simulator-process protocol; see docs/EXTERNAL-CONTROLS.md.
inline std::wstring name(DWORD process, const wchar_t* suffix) {
    return L"Local\\GlassCockpitDlssFix.Tint.v1." + std::to_wstring(process) + L"." + suffix;
}
struct handle {
    HANDLE value = nullptr;
    explicit handle(HANDLE h = nullptr) : value(h) {}
    ~handle() { if (value) CloseHandle(value); }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
};

// Called under the engine's tint mutex, never on the render thread or under loader lock.
class server {
    DWORD process_;
    handle ready_;
    bool attached_ = false;

    bool owner_alive() const {
        handle owner(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name(process_, L"Owner").c_str()));
        if (!owner.value) return false;
        const DWORD wait = WaitForSingleObject(owner.value, 0);
        if (wait == WAIT_TIMEOUT) return true; // controller's UI thread owns it
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) ReleaseMutex(owner.value);
        return false;
    }
public:
    explicit server(DWORD process = GetCurrentProcessId()) : process_(process) {}
    void advertise() {
        if (!ready_.value) ready_.value = CreateEventW(nullptr, TRUE, TRUE, name(process_, L"Ready").c_str());
    }
    bool poll(bool local) {
        advertise();
        handle state(owner_alive() ? OpenEventW(SYNCHRONIZE, FALSE, name(process_, L"On").c_str()) : nullptr);
        if (state.value) {
            const DWORD wait = WaitForSingleObject(state.value, 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
                attached_ = true;
                return wait == WAIT_OBJECT_0;
            }
        }
        // Do not retain Owner/On handles: closing/crashing the controller must end its preview.
        const bool was_attached = attached_;
        attached_ = false;
        return was_attached ? false : local;
    }
    bool set(bool on) {
        handle state(owner_alive() ? OpenEventW(EVENT_MODIFY_STATE, FALSE, name(process_, L"On").c_str()) : nullptr);
        if (state.value) {
            if (on ? SetEvent(state.value) : ResetEvent(state.value)) attached_ = true;
        } else {
            // A native edit may arrive after owner exit, before poll notices it. This local intent
            // supersedes the old attachment; the next poll must not clear the new native value.
            attached_ = false;
        }
        return on;
    }
};
}

// Real cross-process owner lifetime, including abnormal termination. No simulator or GPU needed.
// GPL-3.0-only with the additional terms in NOTICE.md.
#define WIN32_LEAN_AND_MEAN
#include "../src/external_tint.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using glass::external_tint::handle;
using glass::external_tint::name;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct child {
    PROCESS_INFORMATION process{};
    HANDLE input = nullptr, output = nullptr;
    child(const wchar_t* executable, DWORD target) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE child_in = nullptr, child_out = nullptr;
        check(CreatePipe(&child_in, &input, &sa, 0) && CreatePipe(&output, &child_out, &sa, 0), "Pipes");
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_in; si.hStdOutput = child_out; si.hStdError = child_out;
        auto command = L"\"" + std::wstring(executable) + L"\" --owner " + std::to_wstring(target);
        const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &process) != FALSE;
        CloseHandle(child_in); CloseHandle(child_out); check(started, "Child start");
    }
    char reply() {
        for (unsigned i = 0; i < 500; ++i) {
            DWORD available = 0;
            check(PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) != FALSE, "Child disconnected");
            if (available) { char c = 0; DWORD read = 0; check(ReadFile(output, &c, 1, &read, nullptr) && read == 1, "Reply"); return c; }
            Sleep(10);
        }
        throw std::runtime_error("Timed out waiting for child");
    }
    void send(char c) { DWORD wrote = 0; check(WriteFile(input, &c, 1, &wrote, nullptr) && wrote == 1, "Send"); }
    void stop(bool crash) {
        if (crash) check(TerminateProcess(process.hProcess, 17) != FALSE, "Terminate test child");
        else send('q');
        check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "Child did not exit");
    }
    ~child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 18);
            CloseHandle(process.hProcess); CloseHandle(process.hThread);
        }
        if (input) CloseHandle(input); if (output) CloseHandle(output);
    }
};
int owner(DWORD target) {
    handle mutex(CreateMutexW(nullptr, FALSE, name(target, L"Owner").c_str()));
    if (!mutex.value) return 2;
    const DWORD wait = WaitForSingleObject(mutex.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { std::putchar('B'); std::fflush(stdout); return 0; }
    handle state(CreateEventW(nullptr, TRUE, FALSE, name(target, L"On").c_str()));
    if (!state.value) return 3;
    ResetEvent(state.value);
    std::putchar('R'); std::fflush(stdout);
    for (int c; (c = std::getchar()) != EOF && c != 'q';) {
        if (c == '1') SetEvent(state.value);
        if (c == '0') ResetEvent(state.value);
        std::putchar(WaitForSingleObject(state.value, 0) == WAIT_OBJECT_0 ? '1' : '0'); std::fflush(stdout);
    }
    ResetEvent(state.value); ReleaseMutex(mutex.value);
    return 0;
}
int wmain(int argc, wchar_t** argv) try {
    if (argc == 3 && std::wstring(argv[1]) == L"--owner") return owner(wcstoul(argv[2], nullptr, 10));
    const DWORD pid = GetCurrentProcessId();
    glass::external_tint::server server(pid);
    check(!server.poll(false), "Default tint");
    { handle ready(OpenEventW(SYNCHRONIZE, FALSE, name(pid, L"Ready").c_str())); check(ready.value != nullptr, "Capability missing"); }
    check(server.poll(server.set(true)), "Native toggle without controller");
    check(!server.poll(server.set(false)), "Native off");
    for (bool crash : {false, true}) {
        child controller(argv[0], pid); check(controller.reply() == 'R', "Controller attach");
        check(!server.poll(false), "Attach starts off");
        { child duplicate(argv[0], pid); check(duplicate.reply() == 'B', "Second owner accepted"); }
        controller.send('1'); check(controller.reply() == '1' && server.poll(false), "External on");
        glass::external_tint::server other(pid + 1); check(!other.poll(false), "Tint leaked into different target");
        check(!server.set(false), "Native off while attached");
        controller.send('?'); check(controller.reply() == '0' && !server.poll(true), "Native off not shared");
        check(server.set(true), "Native on while attached");
        controller.send('?'); check(controller.reply() == '1' && server.poll(false), "Native on not shared");
        controller.send('0'); check(controller.reply() == '0' && !server.poll(true), "External off");
        controller.send('1'); check(controller.reply() == '1' && server.poll(false), "External restart");
        controller.stop(crash);
        check(!server.poll(true), "Owner exit must clear tint");
        check(server.poll(server.set(true)), "Native toggle after detach");
        check(!server.poll(server.set(false)), "Native reset after detach");
    }
    std::puts("External tint: capability, bidirectional control, isolation, single owner, clean exit, crash and reconnect PASS");
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }

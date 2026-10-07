// RadTuneTask - windowless launcher for scheduled tasks.
//
// RadTune.exe is a console program, so when Task Scheduler starts it directly a
// console window flashes on screen at every logon/startup (issue #5). This tiny
// GUI-subsystem program creates no window: it starts RadTune.exe (found next to
// itself) with CREATE_NO_WINDOW, forwards its own arguments unchanged, waits, and
// exits with RadTune's exit code - so Task Scheduler's "Last Run Result" still
// shows real failures.
//
// The CLI stays a console program on purpose: making RadTune.exe itself a GUI
// app would hide the window too, but cmd/PowerShell would then stop waiting for
// it and lose its exit code, which scripts depend on.

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"" : dir.substr(0, slash + 1);

    const std::wstring target = dir + L"RadTune.exe";
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 2;   // RadTune.exe missing: shows up as Last Run Result 0x2

    // Forward everything after our own program name, verbatim.
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') { ++p; while (*p && *p != L'"') ++p; if (*p) ++p; }
    else            { while (*p && *p != L' ' && *p != L'\t') ++p; }
    while (*p == L' ' || *p == L'\t') ++p;

    std::wstring cmd = L"\"" + target + L"\" " + p;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return 3;   // could not start RadTune.exe

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

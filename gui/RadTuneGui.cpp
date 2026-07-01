// RadTune GUI - a thin native Win32 frontend over the RadTune CLI.
//
// It never talks to ADLX itself: it only builds command lines (-set / -load /
// -schedule), runs RadTune.exe (found next to this exe), and shows its output.
// The app requests elevation via its manifest, so the child RadTune.exe inherits
// admin rights (required for tuning and for creating a highest-privileges task).

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <initializer_list>

// ---------------------------------------------------------------------------
// Layout constants and control IDs
// ---------------------------------------------------------------------------
namespace {
constexpr int LEFT = 15, LBLW = 150, FLDX = 170, FLDW = 180, EDH = 24;

enum : int {
    IDC_SOURCE = 1001, IDC_GPU, IDC_CORE, IDC_COREMIN, IDC_VOLT, IDC_VRAM,
    IDC_POWER, IDC_ZERORPM, IDC_PROFILE, IDC_BROWSE, IDC_TRIGGER, IDC_TIME,
    IDC_APPLY, IDC_SCHEDULE, IDC_STATUS, IDC_REMOVE, IDC_OUTPUT
};

HFONT g_font = nullptr;
HWND g_source, g_gpu, g_core, g_coremin, g_volt, g_vram, g_power, g_zerorpm,
     g_profile, g_browse, g_trigger, g_time, g_output;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::wstring Trim(std::wstring s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s(n, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    return s;
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::string WideToAcp(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring AcpToWide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Full path to RadTune.exe sitting next to this GUI executable.
std::wstring RadTunePath() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"RadTune.exe";
}

// Strip ANSI/VT escape sequences and normalise newlines for a Win32 edit box.
std::string CleanOutput(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;  // skip to final byte
            continue;                                                       // for-loop skips it
        }
        if (in[i] == '\r') continue;
        if (in[i] == '\n') { out += "\r\n"; continue; }
        out += in[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Control factory helpers
// ---------------------------------------------------------------------------
HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, DWORD ex,
        int x, int y, int w, int h, HWND parent, int id) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

HWND MkLabel(HWND p, const wchar_t* t, int x, int y, int w) {
    return Mk(L"STATIC", t, SS_LEFT, 0, x, y, w, 20, p, -1);
}

HWND MkEdit(HWND p, int id, int x, int y, int w, DWORD extra = 0) {
    return Mk(L"EDIT", L"", WS_TABSTOP | extra, WS_EX_CLIENTEDGE, x, y, w, EDH, p, id);
}

HWND MkButton(HWND p, int id, const wchar_t* t, int x, int y, int w, int h = 30) {
    return Mk(L"BUTTON", t, WS_TABSTOP | BS_PUSHBUTTON, 0, x, y, w, h, p, id);
}

HWND MkCombo(HWND p, int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = Mk(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, x, y, w, 220, p, id);
    for (auto it : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)it);
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    return c;
}

HWND LabeledEdit(HWND p, const wchar_t* label, int id, int y, const wchar_t* def = L"", int w = FLDW) {
    MkLabel(p, label, LEFT, y + 4, LBLW);
    HWND e = MkEdit(p, id, FLDX, y, w);
    if (*def) SetWindowTextW(e, def);
    return e;
}

// ---------------------------------------------------------------------------
// Command building + execution
// ---------------------------------------------------------------------------
// Enable manual fields vs profile fields depending on the selected source.
void UpdateSourceState() {
    const bool profile = SendMessageW(g_source, CB_GETCURSEL, 0, 0) == 1;
    for (HWND h : { g_core, g_coremin, g_volt, g_vram, g_power, g_zerorpm })
        EnableWindow(h, !profile);
    EnableWindow(g_profile, profile);
    EnableWindow(g_browse, profile);
}

// Builds the "-set ..." or "-load ..." tail shared by Apply and Schedule.
// Returns empty and fills `err` on invalid input.
std::wstring BuildPayload(std::wstring& err) {
    const std::wstring gpu = Trim(GetText(g_gpu));
    const bool profile = SendMessageW(g_source, CB_GETCURSEL, 0, 0) == 1;

    if (profile) {
        const std::wstring path = Trim(GetText(g_profile));
        if (path.empty()) { err = L"Select a profile .xml file first."; return L""; }
        std::wstring s = L"-load " + Quote(path);
        if (!gpu.empty()) s += L" gpu=" + gpu;
        return s;
    }

    std::wstring tail;
    auto add = [&](HWND h, const wchar_t* key) {
        const std::wstring v = Trim(GetText(h));
        if (!v.empty()) tail += L" " + std::wstring(key) + L"=" + v;
    };
    add(g_core, L"core");
    add(g_coremin, L"coremin");
    add(g_volt, L"volt");
    add(g_vram, L"vram");
    add(g_power, L"power");
    const LRESULT zr = SendMessageW(g_zerorpm, CB_GETCURSEL, 0, 0);  // 0 leave, 1 enable, 2 disable
    if (zr == 1) tail += L" zerorpm=1";
    else if (zr == 2) tail += L" zerorpm=0";

    if (tail.empty()) { err = L"Enter at least one tuning value."; return L""; }
    std::wstring s = L"-set";
    if (!gpu.empty()) s += L" gpu=" + gpu;
    return s + tail;
}

// Runs a command line, capturing stdout+stderr. Returns raw bytes (may hold ANSI).
std::string RunAndCapture(const std::wstring& cmdLine) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "[GUI] CreatePipe failed.";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;  // CreateProcessW may write to the buffer
    const BOOL ok = CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);  // parent must drop its write end so ReadFile sees EOF
    if (!ok) {
        CloseHandle(rd);
        return "[GUI] Could not launch RadTune.exe.\r\nExpected it next to RadTuneGUI.exe:\r\n"
               + WideToAcp(RadTunePath());
    }

    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

void ShowOutput(const std::string& acp) {
    SetWindowTextW(g_output, AcpToWide(CleanOutput(acp)).c_str());
}

// Runs "RadTune.exe <args>", echoing the command then its output.
void RunRadTune(const std::wstring& args) {
    const std::wstring cmd = Quote(RadTunePath()) + L" " + args;
    const std::string header = "> " + WideToAcp(cmd) + "\r\n\r\n";
    ShowOutput(header + RunAndCapture(cmd));
}

void OnApply() {
    std::wstring err;
    const std::wstring payload = BuildPayload(err);
    if (payload.empty()) { ShowOutput("[GUI] " + WideToAcp(err)); return; }
    RunRadTune(payload);
}

void OnSchedule() {
    std::wstring err;
    const std::wstring payload = BuildPayload(err);
    if (payload.empty()) { ShowOutput("[GUI] " + WideToAcp(err)); return; }

    wchar_t trig[32] = { 0 };
    GetWindowTextW(g_trigger, trig, 32);            // "logon" | "startup" | "daily"
    std::wstring trigger = trig;
    if (trigger == L"daily") {
        const std::wstring t = Trim(GetText(g_time));
        if (t.size() != 5 || t[2] != L':') {
            ShowOutput("[GUI] For a daily schedule, enter the time as HH:MM (e.g. 09:00).");
            return;
        }
        trigger += L"=" + t;
    }
    RunRadTune(L"-schedule " + trigger + L" " + payload);
}

void OnBrowse() {
    wchar_t file[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetParent(g_profile);
    ofn.lpstrFilter = L"Adrenalin profile (*.xml)\0*.xml\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_profile, file);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
void BuildUi(HWND w) {
    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    int y = 14;
    MkLabel(w, L"Source", LEFT, y + 4, LBLW);
    g_source = MkCombo(w, IDC_SOURCE, FLDX, y, FLDW, { L"Manual tuning  (-set)", L"Load profile  (-load)" });
    y += 34;

    g_gpu     = LabeledEdit(w, L"GPU index",           IDC_GPU,     y, L"0", 60); y += 30;
    g_core    = LabeledEdit(w, L"Core max (MHz)",       IDC_CORE,    y);          y += 30;
    g_coremin = LabeledEdit(w, L"Core min (MHz)",       IDC_COREMIN, y);          y += 30;
    g_volt    = LabeledEdit(w, L"Voltage offset (mV)",  IDC_VOLT,    y);          y += 30;
    g_vram    = LabeledEdit(w, L"VRAM max (MHz)",       IDC_VRAM,    y);          y += 30;
    g_power   = LabeledEdit(w, L"Power limit (%)",      IDC_POWER,   y);          y += 30;

    MkLabel(w, L"Zero RPM fan", LEFT, y + 4, LBLW);
    g_zerorpm = MkCombo(w, IDC_ZERORPM, FLDX, y, FLDW, { L"Leave unchanged", L"Enable", L"Disable" });
    y += 34;

    MkLabel(w, L"Profile .xml", LEFT, y + 4, LBLW);
    g_profile = MkEdit(w, IDC_PROFILE, FLDX, y, 270);
    g_browse  = MkButton(w, IDC_BROWSE, L"Browse...", FLDX + 280, y - 2, 80, 26);
    y += 40;

    MkButton(w, IDC_APPLY, L"Apply now", LEFT, y, 175, 30);
    y += 42;

    MkLabel(w, L"Trigger", LEFT, y + 4, 55);
    g_trigger = MkCombo(w, IDC_TRIGGER, 75, y, 110, { L"logon", L"startup", L"daily" });
    MkLabel(w, L"Time (daily)", 200, y + 4, 85);
    g_time = MkEdit(w, IDC_TIME, 290, y, 70);
    SetWindowTextW(g_time, L"09:00");
    y += 36;

    MkButton(w, IDC_SCHEDULE, L"Create schedule", LEFT,       y, 175, 28);
    MkButton(w, IDC_STATUS,   L"Show status",     LEFT + 185, y, 150, 28);
    MkButton(w, IDC_REMOVE,   L"Remove schedule", LEFT + 345, y, 175, 28);
    y += 40;

    MkLabel(w, L"Output", LEFT, y, 100);
    y += 20;
    g_output = Mk(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                  WS_EX_CLIENTEDGE, LEFT, y, 520, 150, w, IDC_OUTPUT);

    UpdateSourceState();
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        BuildUi(w);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SOURCE:  if (HIWORD(wp) == CBN_SELCHANGE) UpdateSourceState(); return 0;
        case IDC_APPLY:    OnApply();    return 0;
        case IDC_SCHEDULE: OnSchedule(); return 0;
        case IDC_STATUS:   RunRadTune(L"-schedule status"); return 0;
        case IDC_REMOVE:   RunRadTune(L"-schedule remove"); return 0;
        case IDC_BROWSE:   OnBrowse();   return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd) {
    const INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RadTuneGuiWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RadTune GUI",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 700, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {   // Tab navigation between controls
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

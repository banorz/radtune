// RadTune GUI - a thin native Win32 frontend over the RadTune CLI.
//
// It never talks to ADLX itself: it only builds command lines (-set / -load /
// -get / -schedule), runs RadTune.exe (found next to this exe), and shows its
// output. The app requests elevation via its manifest, so the child RadTune.exe
// inherits admin rights (required for tuning and for scheduling).

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <utility>
#include <initializer_list>

#define IDI_APPICON 101

namespace {
// ---------------------------------------------------------------------------
// Layout constants and control IDs
// ---------------------------------------------------------------------------
constexpr int M = 16;          // outer margin
constexpr int HEADER = 58;     // header strip height
constexpr int TABH = 28;       // tab strip height (Tuning | Fan | Live)
constexpr int LBLX = 32, FLDX = 196, FLDW = 150, EDH = 24;
// Content area below the tab strip: the settings pages (Tuning / Fan), then the
// shared action bar. The window is sized FROM these with AdjustWindowRect, so the
// layout can't silently overflow or leave a gap after a change.
constexpr int CONTENT_TOP = HEADER + 4 + TABH;
constexpr int SETTINGS_H  = 352;
constexpr int ACTIONS_H   = 196;
constexpr int CLIENT_W    = 584;
constexpr int CLIENT_H    = CONTENT_TOP + SETTINGS_H + ACTIONS_H;

enum : int {
    IDC_SOURCE = 1001, IDC_GPU, IDC_READ, IDC_CORE, IDC_COREMIN, IDC_VOLT,
    IDC_VRAM, IDC_MEMTIMING, IDC_POWER, IDC_ZERORPM, IDC_PROFILE, IDC_BROWSE, IDC_APPLY,
    IDC_TRIGGER, IDC_TIME, IDC_SCHEDULE, IDC_STATUS, IDC_REMOVE, IDC_OUTPUT,
    IDC_TABS, IDC_REFRESH, IDC_LIVE,
    IDC_FANMIN, IDC_FANTARGET, IDC_ACOUSTIC,
    IDC_CURVE   // fan curve edits: IDC_CURVE + i (temperature), IDC_CURVE + 5 + i (speed)
};

constexpr int CURVE_POINTS = 5;   // ADLX manual fan curve: 5 (temperature, speed) points

HFONT g_font = nullptr, g_mono = nullptr, g_title = nullptr, g_sub = nullptr;
HWND g_source, g_gpu, g_core, g_coremin, g_volt, g_vram, g_memtiming, g_power, g_zerorpm,
     g_profile, g_browse, g_trigger, g_time, g_status;
// Tab strip and its two pages. Each page is a container child window holding
// that tab's controls; exactly one is visible at a time. Overlapping sibling
// windows (the first attempt) repaint over each other, so the Live page showed
// through the tuning form - containers avoid the problem entirely.
HWND g_main = nullptr;   // main window; owner of the result dialogs
// Pages: Tuning and Fan hold settings; the action bar below them (Apply,
// Automation, status) is shared, so Apply and a schedule always cover BOTH -
// a fan curve that isn't in the logon task wouldn't survive a reboot.
HWND g_tabs, g_pageTuning, g_pageFan, g_actions, g_pageLive, g_live;
HWND g_fanmin, g_fantarget, g_acoustic, g_curveT[CURVE_POINTS], g_curveS[CURVE_POINTS];
// Core max and voltage are offsets on RDNA4 but absolute values on RDNA2/3, so
// their labels are set from -get's coremode= / voltmode= rather than fixed.
HWND g_coreLabel, g_voltLabel;

// Settings the card may not have. -get prints a key only when the GPU supports
// it; a missing key clears and disables the field (an RX 9060/9070 XT has no
// core min, and RDNA4 exposes only the fan curve - issue #10).
bool g_hasCoreMin = true, g_hasFanMin = true, g_hasFanTarget = true,
     g_hasAcoustic = true, g_hasCurve = true;

// memtiming combo index -> RadTune token. Index 0 is always "Leave unchanged"
// (empty token). The rest is filled from the card's OWN supported list, which
// `-get` reports as memtimingsupported=: the ADLX enum is a superset across
// architectures, so a hardcoded list would offer presets the GPU refuses.
std::vector<std::wstring> g_memtimingTokens = { L"" };

const wchar_t* REG_KEY = L"Software\\RadTune";

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

int ComboSel(HWND h) { return (int)SendMessageW(h, CB_GETCURSEL, 0, 0); }
void SetCombo(HWND h, int i) { SendMessageW(h, CB_SETCURSEL, i, 0); }
int ComboCount(HWND h) { return (int)SendMessageW(h, CB_GETCOUNT, 0, 0); }
void ComboClear(HWND h) { SendMessageW(h, CB_RESETCONTENT, 0, 0); }
void ComboAdd(HWND h, const std::wstring& s) { SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }

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

std::wstring RadTunePath() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"RadTune.exe";
}

// Strip ANSI/VT escapes and normalise newlines for a Win32 edit box.
std::string CleanOutput(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;
            continue;
        }
        if (in[i] == '\r') continue;
        if (in[i] == '\n') { out += "\r\n"; continue; }
        out += in[i];
    }
    return out;
}

int PtToPx(int pt) {
    HDC hdc = GetDC(nullptr);
    const int px = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    return px;
}

HFONT MakeFont(int pt, int weight, const wchar_t* face) {
    return CreateFontW(PtToPx(pt), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

// ---------------------------------------------------------------------------
// Registry persistence
// ---------------------------------------------------------------------------
void RegWrite(const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

std::wstring RegRead(const wchar_t* name) {
    HKEY k;
    std::wstring out;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[512];
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
            out = buf;
        RegCloseKey(k);
    }
    return out;
}

void SaveSettings() {
    auto e = [](HWND h, const wchar_t* n) { RegWrite(n, GetText(h)); };
    auto c = [](HWND h, const wchar_t* n) { RegWrite(n, std::to_wstring(ComboSel(h))); };
    c(g_source, L"source"); c(g_gpu, L"gpu"); e(g_core, L"core"); e(g_coremin, L"coremin");
    e(g_volt, L"volt"); e(g_vram, L"vram"); c(g_memtiming, L"memtiming"); e(g_power, L"power");
    c(g_zerorpm, L"zerorpm");
    e(g_profile, L"profile"); c(g_trigger, L"trigger"); e(g_time, L"time");
}

void LoadSettings() {
    auto e = [](HWND h, const wchar_t* n) { std::wstring v = RegRead(n); if (!v.empty()) SetWindowTextW(h, v.c_str()); };
    auto c = [](HWND h, const wchar_t* n) { std::wstring v = RegRead(n); if (!v.empty()) SetCombo(h, _wtoi(v.c_str())); };
    c(g_source, L"source"); e(g_core, L"core"); e(g_coremin, L"coremin");
    e(g_volt, L"volt"); e(g_vram, L"vram"); c(g_memtiming, L"memtiming"); e(g_power, L"power");
    c(g_zerorpm, L"zerorpm");
    e(g_profile, L"profile"); c(g_trigger, L"trigger"); e(g_time, L"time");
}

// ---------------------------------------------------------------------------
// Control factory helpers
// ---------------------------------------------------------------------------
HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, DWORD ex,
        int x, int y, int w, int h, HWND parent, int id, HFONT font) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkLabel(HWND p, const wchar_t* t, int x, int y, int w) {
    return Mk(L"STATIC", t, SS_LEFT, 0, x, y, w, 20, p, -1, g_font);
}
HWND MkGroup(HWND p, const wchar_t* t, int x, int y, int w, int h) {
    return Mk(L"BUTTON", t, BS_GROUPBOX, 0, x, y, w, h, p, -1, g_font);
}
HWND MkEdit(HWND p, int id, int x, int y, int w, DWORD extra = 0) {
    return Mk(L"EDIT", L"", WS_TABSTOP | extra, WS_EX_CLIENTEDGE, x, y, w, EDH, p, id, g_font);
}
HWND MkButton(HWND p, int id, const wchar_t* t, int x, int y, int w, int h = 28) {
    // Owner-drawn so text contrast never depends on the system button theme
    // (Win11 dark mode can render stock buttons grey-on-grey). See DrawButton().
    return Mk(L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, 0, x, y, w, h, p, id, g_font);
}
HWND MkCombo(HWND p, int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = Mk(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, x, y, w, 240, p, id, g_font);
    for (auto it : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)it);
    SetCombo(c, 0);
    return c;
}
HWND LabeledEdit(HWND p, const wchar_t* label, int id, int y, int w = FLDW) {
    MkLabel(p, label, LBLX, y + 4, FLDX - LBLX - 8);
    return MkEdit(p, id, FLDX, y, w);
}

// ---------------------------------------------------------------------------
// Command building + execution
// ---------------------------------------------------------------------------
void UpdateSourceState() {
    const bool profile = ComboSel(g_source) == 1;
    const bool manual = !profile;   // -load ignores the manual fields entirely
    for (HWND h : { g_core, g_volt, g_vram, g_memtiming, g_power, g_zerorpm })
        EnableWindow(h, manual);
    EnableWindow(g_coremin,   manual && g_hasCoreMin);
    EnableWindow(g_fanmin,    manual && g_hasFanMin);
    EnableWindow(g_fantarget, manual && g_hasFanTarget);
    EnableWindow(g_acoustic,  manual && g_hasAcoustic);
    for (int i = 0; i < CURVE_POINTS; ++i) {
        EnableWindow(g_curveT[i], manual && g_hasCurve);
        EnableWindow(g_curveS[i], manual && g_hasCurve);
    }
    EnableWindow(g_profile, profile);
    EnableWindow(g_browse, profile);
}

// The GPU dropdown lists device names; the CLI wants the index.
std::wstring SelectedGpu() {
    const int i = ComboSel(g_gpu);
    return std::to_wstring(i < 0 ? 0 : i);
}

std::wstring BuildPayload(std::wstring& err) {
    const std::wstring gpu = SelectedGpu();
    if (ComboSel(g_source) == 1) {  // Profile (-load)
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
    add(g_core, L"core"); add(g_coremin, L"coremin"); add(g_volt, L"volt");
    add(g_vram, L"vram"); add(g_power, L"power");
    const int mt = ComboSel(g_memtiming);  // 0 = leave unchanged, else preset
    if (mt > 0 && mt < (int)g_memtimingTokens.size())
        tail += L" memtiming=" + g_memtimingTokens[mt];
    const int zr = ComboSel(g_zerorpm);   // 0 leave, 1 enable, 2 disable
    if (zr == 1) tail += L" zerorpm=1";
    else if (zr == 2) tail += L" zerorpm=0";
    add(g_fanmin, L"fanmin"); add(g_fantarget, L"fantarget"); add(g_acoustic, L"acoustic");

    // Fan curve: all points or none. A half-filled curve has no sensible meaning.
    std::wstring curve;
    int filled = 0;
    for (int i = 0; i < CURVE_POINTS; ++i) {
        const std::wstring t = Trim(GetText(g_curveT[i])), s = Trim(GetText(g_curveS[i]));
        filled += !t.empty() + !s.empty();
        curve += (i ? L"," : L"") + t + L":" + s;
    }
    if (filled == 2 * CURVE_POINTS) {
        tail += L" fancurve=" + curve;
    } else if (filled > 0) {
        err = L"Fill in every fan curve point (temperature and speed), or clear them all to leave the curve unchanged.";
        return L"";
    }
    if (tail.empty()) { err = L"Enter at least one tuning value."; return L""; }
    std::wstring s = L"-set";
    if (!gpu.empty()) s += L" gpu=" + gpu;
    return s + tail;
}

std::string RunAndCapture(const std::wstring& cmdLine, DWORD* exitCode = nullptr) {
    if (exitCode) *exitCode = 1;   // assume failure until the child says otherwise
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "[GUI] CreatePipe failed.";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return "[GUI] Could not launch RadTune.exe.\r\nExpected next to RadTuneGUI.exe:\r\n" + WideToAcp(RadTunePath());
    }
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

// One-line progress/state text under the form. Operation results are reported
// in a dialog instead: a modal box cannot be missed the way a side panel can,
// and the CLI's exit code tells us whether it should be an error or a success.
void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

void ShowResultDialog(const std::string& acp, DWORD exitCode) {
    std::wstring body = Trim(AcpToWide(CleanOutput(acp)));
    if (body.empty()) body = (exitCode == 0) ? L"Done." : L"The command failed but produced no output.";
    MessageBoxW(g_main, body.c_str(), exitCode == 0 ? L"RadTune" : L"RadTune - failed",
                MB_OK | (exitCode == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
}

// --- Async command execution -------------------------------------------------
// RadTune.exe (ADLX init) can take ~1s; running it on the UI thread froze the
// window. Run it on a worker thread and post the captured output back so the UI
// stays responsive.
enum { RUN_SHOW = 1, RUN_READ = 2, RUN_GPUS = 3 };
constexpr UINT WM_APP_RESULT = WM_APP + 1;

bool g_busy = false;

struct RunCtx { int kind; std::wstring cmd; };
struct RunOut { int kind; std::string text; DWORD exitCode; };

DWORD WINAPI RunWorker(LPVOID p) {
    RunCtx* c = static_cast<RunCtx*>(p);
    DWORD rc = 1;
    std::string out = RunAndCapture(c->cmd, &rc);
    PostMessageW(g_main, WM_APP_RESULT, 0,
                 reinterpret_cast<LPARAM>(new RunOut{ c->kind, std::move(out), rc }));
    delete c;
    return 0;
}

void SetActionsEnabled(bool on) {
    EnableWindow(GetDlgItem(g_pageTuning, IDC_READ), on);
    for (int id : { IDC_APPLY, IDC_SCHEDULE, IDC_STATUS, IDC_REMOVE })
        EnableWindow(GetDlgItem(g_actions, id), on);
}

// Launches "RadTune.exe <args>" on a worker thread; the result comes back via
// WM_APP_RESULT and is handled by OnRunResult on the UI thread.
void StartRun(int kind, const std::wstring& args) {
    if (g_busy) return;
    const std::wstring cmd = Quote(RadTunePath()) + L" " + args;
    SetStatus(L"Running...");
    g_busy = true;
    SetActionsEnabled(false);
    RunCtx* c = new RunCtx{ kind, cmd };
    HANDLE h = CreateThread(nullptr, 0, RunWorker, c, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        delete c;
        g_busy = false;
        SetActionsEnabled(true);
        SetStatus(L"Could not start worker thread.");
    }
}

// Fills the manual fields from RadTune -get output. Returns true if any matched.
bool ParseGetOutput(const std::string& raw) {
    const std::string clean = CleanOutput(raw);
    SetCombo(g_source, 0);  // switch to Manual so fields are visible/editable
    UpdateSourceState();
    bool got = false;
    // memtiming needs both lines before we can act: the supported list defines
    // the dropdown, the current value picks the entry. They arrive in order but
    // we collect and apply them after the loop rather than rely on it.
    std::wstring mtCurrent, mtSupported, curve;
    bool seenCoreMin = false, seenFanMin = false, seenFanTarget = false, seenAcoustic = false;
    size_t pos = 0;
    while (pos < clean.size()) {
        size_t nl = clean.find("\r\n", pos);
        std::string line = clean.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? clean.size() : nl + 2;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::wstring v = Trim(AcpToWide(line.substr(eq + 1)));
        bool known = true;
        if (key == "core") SetWindowTextW(g_core, v.c_str());
        else if (key == "coremin")   { SetWindowTextW(g_coremin, v.c_str());   seenCoreMin = true; }
        else if (key == "volt") SetWindowTextW(g_volt, v.c_str());
        else if (key == "vram") SetWindowTextW(g_vram, v.c_str());
        else if (key == "power") SetWindowTextW(g_power, v.c_str());
        else if (key == "memtiming") mtCurrent = v;
        else if (key == "memtimingsupported") mtSupported = v;
        else if (key == "zerorpm") SetCombo(g_zerorpm, v == L"1" ? 1 : 2);
        else if (key == "fanmin")    { SetWindowTextW(g_fanmin, v.c_str());    seenFanMin = true; }
        else if (key == "fantarget") { SetWindowTextW(g_fantarget, v.c_str()); seenFanTarget = true; }
        else if (key == "acoustic")  { SetWindowTextW(g_acoustic, v.c_str());  seenAcoustic = true; }
        else if (key == "fancurve")  curve = v;
        else if (key == "coremode")
            SetWindowTextW(g_coreLabel, v == L"offset" ? L"Core max offset (MHz)" : L"Core max (MHz)");
        else if (key == "voltmode")
            SetWindowTextW(g_voltLabel, v == L"offset" ? L"Voltage offset (mV)" : L"Voltage (mV)");
        else known = false;
        if (known) got = true;
    }

    // The fan curve editor has CURVE_POINTS columns; a curve of any other shape
    // can't be shown faithfully, so it counts as unsupported here.
    std::vector<std::pair<std::wstring, std::wstring>> points;
    for (size_t p = 0; !curve.empty() && p <= curve.size();) {
        const size_t c = curve.find(L',', p);
        const std::wstring pt = curve.substr(p, c == std::wstring::npos ? std::wstring::npos : c - p);
        const size_t colon = pt.find(L':');
        if (colon != std::wstring::npos) points.emplace_back(pt.substr(0, colon), pt.substr(colon + 1));
        if (c == std::wstring::npos) break;
        p = c + 1;
    }
    const bool curveOk = points.size() == CURVE_POINTS;
    for (int i = 0; i < CURVE_POINTS; ++i) {
        SetWindowTextW(g_curveT[i], curveOk ? points[i].first.c_str()  : L"");
        SetWindowTextW(g_curveS[i], curveOk ? points[i].second.c_str() : L"");
    }

    // A key the card didn't report means the setting doesn't exist there: clear
    // the field so a stale value can't be sent, and let UpdateSourceState
    // disable it.
    if (got) {
        g_hasCoreMin = seenCoreMin;   g_hasFanMin = seenFanMin;  g_hasFanTarget = seenFanTarget;
        g_hasAcoustic = seenAcoustic; g_hasCurve = curveOk;
        if (!seenCoreMin)   SetWindowTextW(g_coremin, L"");
        if (!seenFanMin)    SetWindowTextW(g_fanmin, L"");
        if (!seenFanTarget) SetWindowTextW(g_fantarget, L"");
        if (!seenAcoustic)  SetWindowTextW(g_acoustic, L"");
        UpdateSourceState();
    }

    if (!mtSupported.empty()) {
        // Rebuild the dropdown to exactly what this card accepts.
        g_memtimingTokens.assign(1, L"");
        ComboClear(g_memtiming);
        ComboAdd(g_memtiming, L"Leave unchanged");
        size_t p = 0;
        while (p <= mtSupported.size()) {
            const size_t c = mtSupported.find(L',', p);
            std::wstring tok = Trim(mtSupported.substr(p, c == std::wstring::npos ? std::wstring::npos : c - p));
            if (!tok.empty()) { g_memtimingTokens.push_back(tok); ComboAdd(g_memtiming, tok); }
            if (c == std::wstring::npos) break;
            p = c + 1;
        }
    }
    if (!mtCurrent.empty()) {
        int idx = 0;   // 0 = "Leave unchanged" when the value isn't in the list
        for (size_t i = 1; i < g_memtimingTokens.size(); ++i)
            if (g_memtimingTokens[i] == mtCurrent) { idx = (int)i; break; }
        SetCombo(g_memtiming, idx);
    }
    return got;
}

// Fills the GPU dropdown from "-gpus" output (gpu0=NAME lines). Returns the
// number of GPUs found.
int ParseGpusOutput(const std::string& raw) {
    const std::string clean = CleanOutput(raw);
    std::vector<std::wstring> names;
    size_t pos = 0;
    while (pos < clean.size()) {
        size_t nl = clean.find("\r\n", pos);
        std::string line = clean.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? clean.size() : nl + 2;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.compare(0, 3, "gpu") != 0) continue;
        names.push_back(Trim(AcpToWide(line.substr(eq + 1))));
    }
    if (names.empty()) return 0;
    ComboClear(g_gpu);
    for (size_t i = 0; i < names.size(); ++i)
        ComboAdd(g_gpu, std::to_wstring(i) + L": " + names[i]);
    // Restore the remembered selection now that the list exists.
    const std::wstring saved = RegRead(L"gpu");
    int sel = saved.empty() ? 0 : _wtoi(saved.c_str());
    if (sel < 0 || sel >= (int)names.size()) sel = 0;
    SetCombo(g_gpu, sel);
    return (int)names.size();
}

// Turns "-monitor" key=value output into a human-readable readout for the Live
// tab. Only lines present in the output are shown (unsupported metrics skipped).
std::string FormatMonitor(const std::string& raw) {
    const std::string clean = CleanOutput(raw);
    // key -> value
    std::string keys[16]; std::string vals[16]; int n = 0;
    size_t pos = 0;
    while (pos < clean.size() && n < 16) {
        size_t nl = clean.find("\r\n", pos);
        std::string line = clean.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? clean.size() : nl + 2;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        keys[n] = line.substr(0, eq);
        vals[n] = line.substr(eq + 1);
        ++n;
    }
    auto val = [&](const char* k) -> std::string {
        for (int i = 0; i < n; ++i) if (keys[i] == k) return vals[i];
        return "";
    };
    struct Row { const char* key; const char* label; const char* unit; };
    static const Row rows[] = {
        {"gpuclock",  "GPU clock",   "MHz"}, {"vramclock", "VRAM clock",  "MHz"},
        {"temp",      "Temperature", "C"},   {"hotspot",   "Hotspot",     "C"},
        {"fan",       "Fan speed",   "RPM"}, {"power",     "GPU power",   "W"},
        {"boardpower","Board power", "W"},   {"voltage",   "Voltage",     "mV"},
        {"usage",     "GPU usage",   "%"},   {"vramused",  "VRAM used",   "MB"},
    };
    std::string out;
    for (const auto& r : rows) {
        const std::string v = val(r.key);
        if (v.empty()) continue;
        std::string label = r.label; label.resize(14, ' ');
        out += label + v + " " + r.unit + "\r\n";
    }
    if (out.empty()) out = "[GUI] No telemetry was returned by the GPU.";
    return out;
}

void OnReadGpu();   // defined below; the -gpus result chains straight into it

// Runs on the UI thread when a worker finishes (WM_APP_RESULT).
void OnRunResult(int kind, const std::string& raw, DWORD exitCode) {
    bool chainRead = false;
    // Reads are routine and their result is visible in the fields, so they only
    // interrupt with a dialog when they actually fail. Actions the user asked
    // for (apply / schedule) always report back.
    if (kind == RUN_READ) {
        const bool got = ParseGetOutput(raw);
        SetStatus(got ? L"Values read from the GPU." : L"No tuning values were read.");
        if (!got || exitCode != 0) ShowResultDialog(raw, exitCode);
    } else if (kind == RUN_GPUS) {
        // Startup: the device list arrived. If we found a GPU there's no reason
        // to make the user press a button to see its current tuning - read it.
        const int n = ParseGpusOutput(raw);
        if (n > 0) { chainRead = true; SetStatus(L""); }
        else {
            SetStatus(L"No AMD GPU found.");
            ShowResultDialog(raw, exitCode);
        }
    } else {
        SetStatus(exitCode == 0 ? L"Done." : L"Failed.");
        ShowResultDialog(raw, exitCode);
    }
    SaveSettings();
    g_busy = false;
    SetActionsEnabled(true);
    if (chainRead) OnReadGpu();
}

void OnApply() {
    std::wstring err;
    const std::wstring payload = BuildPayload(err);
    if (payload.empty()) { MessageBoxW(g_main, err.c_str(), L"RadTune", MB_OK | MB_ICONWARNING); return; }
    StartRun(RUN_SHOW, payload);
}

void OnSchedule() {
    std::wstring err;
    const std::wstring payload = BuildPayload(err);
    if (payload.empty()) { MessageBoxW(g_main, err.c_str(), L"RadTune", MB_OK | MB_ICONWARNING); return; }
    std::wstring trigger = Trim(GetText(g_trigger));  // logon | startup | daily
    if (trigger == L"daily") {
        const std::wstring t = Trim(GetText(g_time));
        if (t.size() != 5 || t[2] != L':') {
            MessageBoxW(g_main, L"For a daily schedule, enter the time as HH:MM (e.g. 09:00).", L"RadTune", MB_OK | MB_ICONWARNING);
            return;
        }
        trigger += L"=" + t;
    }
    StartRun(RUN_SHOW, L"-schedule " + trigger + L" " + payload);
}

void OnReadGpu() {
    StartRun(RUN_READ, L"-get gpu=" + SelectedGpu());
}

// --- Live telemetry stream --------------------------------------------------
// The Live tab keeps ONE "RadTune -monitor watch=..." process running and reads
// its samples as they arrive. Re-launching per sample would pay ~600 ms of ADLX
// init every time, so the reading would always be that stale and the machine
// would churn through processes. Started when the tab is shown, killed when it
// is hidden or the window closes.
constexpr UINT WM_APP_LIVE = WM_APP + 2;
constexpr int LIVE_INTERVAL_MS = 1000;

HANDLE g_liveProc = nullptr;   // child process (terminated to stop it)
HANDLE g_liveRead = nullptr;   // our end of its stdout pipe
volatile bool g_liveStop = false;

DWORD WINAPI LiveWorker(LPVOID) {
    std::string acc;
    char buf[2048];
    DWORD n = 0;
    while (!g_liveStop && ReadFile(g_liveRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
        // Drop CR while accumulating: the child's CRT writes text mode, so the
        // blank line separating samples arrives as "\r\n\r\n". Searching the raw
        // bytes for "\n\n" never matched and no sample was ever emitted.
        for (DWORD i = 0; i < n; ++i)
            if (buf[i] != '\r') acc += buf[i];
        // Each sample is terminated by a blank line, so a "\n\n" marks one
        // complete record (the startup banner rides along with the first one
        // and is ignored by FormatMonitor, which only reads key=value lines).
        size_t cut;
        while ((cut = acc.find("\n\n")) != std::string::npos) {
            auto* s = new std::string(acc.substr(0, cut));
            acc.erase(0, cut + 2);
            if (!PostMessageW(g_main, WM_APP_LIVE, 0, reinterpret_cast<LPARAM>(s))) delete s;
        }
    }
    return 0;
}

void StopLive() {
    g_liveStop = true;
    if (g_liveProc) { TerminateProcess(g_liveProc, 0); CloseHandle(g_liveProc); g_liveProc = nullptr; }
    if (g_liveRead) { CloseHandle(g_liveRead); g_liveRead = nullptr; }
}

void StartLive() {
    if (g_liveProc) return;   // already streaming
    SetWindowTextW(g_live, L"Starting telemetry...");

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;

    std::wstring cmd = Quote(RadTunePath()) + L" -monitor gpu=" + SelectedGpu() +
                       L" watch=" + std::to_wstring(LIVE_INTERVAL_MS);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        SetWindowTextW(g_live, L"[GUI] Could not start RadTune.exe for live telemetry.");
        return;
    }
    CloseHandle(pi.hThread);
    g_liveProc = pi.hProcess;
    g_liveRead = rd;
    g_liveStop = false;
    HANDLE h = CreateThread(nullptr, 0, LiveWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else   StopLive();
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
// UI construction
// ---------------------------------------------------------------------------
void BuildUi(HWND w) {
    RECT rc; GetClientRect(w, &rc);
    const int GW = rc.right - 2 * M;   // group width

    // ---- Tab strip: Tuning | Fan | Live ----
    g_tabs = Mk(L"SysTabControl32", L"", 0, 0, M, HEADER + 4, GW, TABH - 2, w, IDC_TABS, g_font);
    TCITEMW ti{}; ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Tuning"; SendMessageW(g_tabs, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"Fan";    SendMessageW(g_tabs, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"Live";   SendMessageW(g_tabs, TCM_INSERTITEMW, 2, (LPARAM)&ti);

    // ---- Containers. Tuning/Fan share the settings rect and sit above the
    // shared action bar; Live spans both, since it has no settings to apply. ----
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto page = [&](int top, int height, bool visible) {
        return CreateWindowExW(0, L"RadTunePage", L"", WS_CHILD | WS_CLIPSIBLINGS | (visible ? WS_VISIBLE : 0),
                               0, top, rc.right, height, w, nullptr, hInst, nullptr);
    };
    g_pageTuning = page(CONTENT_TOP, SETTINGS_H, true);
    g_pageFan    = page(CONTENT_TOP, SETTINGS_H, false);
    g_actions    = page(CONTENT_TOP + SETTINGS_H, ACTIONS_H, true);
    g_pageLive   = page(CONTENT_TOP, SETTINGS_H + ACTIONS_H, false);

    // Everything below is positioned relative to its container, not the window.
    HWND p = g_pageTuning;

    // ---- Tuning page ----
    int gy = 6;
    MkGroup(p, L" GPU tuning ", M, gy, GW, 338);
    int y = gy + 24;
    MkLabel(p, L"Source", LBLX, y + 4, FLDX - LBLX - 8);
    g_source = MkCombo(p, IDC_SOURCE, FLDX, y, 300, { L"Manual tuning  (-set)", L"Load profile  (-load)" });
    y += 32;

    // Right under Source: it is the input for the "Load profile" source.
    MkLabel(p, L"Profile .xml", LBLX, y + 4, FLDX - LBLX - 8);
    g_profile = MkEdit(p, IDC_PROFILE, FLDX, y, GW - (FLDX - M) - 120);
    g_browse = MkButton(p, IDC_BROWSE, L"Browse...", M + GW - 104, y - 1, 88, 26);
    y += 34;

    // Device list is filled at startup from "-gpus"; the CLI takes the index.
    MkLabel(p, L"GPU", LBLX, y + 4, FLDX - LBLX - 8);
    g_gpu = MkCombo(p, IDC_GPU, FLDX, y, 230, { });
    MkButton(p, IDC_READ, L"Read from GPU", FLDX + 238, y - 1, 120, 26);
    y += 32;

    // Neutral until -get says whether this card uses offsets (RDNA4) or absolutes.
    g_coreLabel = MkLabel(p, L"Core max (MHz)", LBLX, y + 4, FLDX - LBLX - 8);
    g_core      = MkEdit(p, IDC_CORE, FLDX, y, FLDW);                    y += 30;
    g_coremin   = LabeledEdit(p, L"Core min (MHz)", IDC_COREMIN, y);    y += 30;
    g_voltLabel = MkLabel(p, L"Voltage (mV)", LBLX, y + 4, FLDX - LBLX - 8);
    g_volt      = MkEdit(p, IDC_VOLT, FLDX, y, FLDW);                    y += 30;
    g_vram    = LabeledEdit(p, L"VRAM max (MHz)",        IDC_VRAM,    y); y += 30;

    MkLabel(p, L"VRAM mem timing", LBLX, y + 4, FLDX - LBLX - 8);
    // Only "Leave unchanged" up front - the real presets are added once we know
    // which ones this card supports (from -get's memtimingsupported=).
    g_memtiming = MkCombo(p, IDC_MEMTIMING, FLDX, y, 200, { L"Leave unchanged" });
    y += 32;

    g_power   = LabeledEdit(p, L"Power limit (%)", IDC_POWER, y);

    // ---- Fan page. Every field but Zero RPM is enabled only if -get reported
    // it: RDNA4 exposes just the curve; the SDK does not say which cards have the rest. ----
    HWND f = g_pageFan;
    MkGroup(f, L" Fan ", M, gy, GW, 256);
    y = gy + 24;
    MkLabel(f, L"Zero RPM", LBLX, y + 4, FLDX - LBLX - 8);
    g_zerorpm = MkCombo(f, IDC_ZERORPM, FLDX, y, 200, { L"Leave unchanged", L"Enable", L"Disable" });
    y += 32;
    g_fanmin    = LabeledEdit(f, L"Min fan speed (RPM)",    IDC_FANMIN,    y); y += 30;
    g_fantarget = LabeledEdit(f, L"Target fan speed (RPM)", IDC_FANTARGET, y); y += 30;
    g_acoustic  = LabeledEdit(f, L"Acoustic limit (MHz)",   IDC_ACOUSTIC,  y); y += 36;

    MkLabel(f, L"Curve temp (°C)", LBLX, y + 4, FLDX - LBLX - 8);
    for (int i = 0; i < CURVE_POINTS; ++i)
        g_curveT[i] = MkEdit(f, IDC_CURVE + i, FLDX + i * 60, y, 52);
    y += 30;
    MkLabel(f, L"Curve speed (%)", LBLX, y + 4, FLDX - LBLX - 8);
    for (int i = 0; i < CURVE_POINTS; ++i)
        g_curveS[i] = MkEdit(f, IDC_CURVE + CURVE_POINTS + i, FLDX + i * 60, y, 52);
    y += 32;
    Mk(L"STATIC", L"Five points, each a temperature and the fan speed to run at it. "
                  L"Clear all ten boxes to leave the curve as it is.",
       SS_LEFT, 0, LBLX, y, GW - 32, 32, f, -1, g_font);

    // ---- Shared action bar: applies / schedules Tuning AND Fan together ----
    HWND a = g_actions;
    MkButton(a, IDC_APPLY, L"Apply now", M, 0, 200, 32);
    const int ay = 44;
    MkGroup(a, L" Automation (Task Scheduler) ", M, ay, GW, 104);
    int ty = ay + 26;
    MkLabel(a, L"Trigger", LBLX, ty + 4, 55);
    g_trigger = MkCombo(a, IDC_TRIGGER, 92, ty, 110, { L"logon", L"startup", L"daily" });
    MkLabel(a, L"Time (daily)", 224, ty + 4, 80);
    g_time = MkEdit(a, IDC_TIME, 312, ty, 70);
    SetWindowTextW(g_time, L"09:00");
    ty += 36;
    MkButton(a, IDC_SCHEDULE, L"Create schedule", LBLX,       ty, 170, 28);
    MkButton(a, IDC_STATUS,   L"Show status",     LBLX + 182, ty, 140, 28);
    MkButton(a, IDC_REMOVE,   L"Remove schedule", LBLX + 330, ty, 170, 28);

    // Status line (results are reported in a dialog, not a panel).
    g_status = Mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, M, ay + 104 + 12, GW, 20, a, IDC_OUTPUT, g_font);

    // ---- Live page: streams while the tab is open, no button ----
    const int liveH = SETTINGS_H + ACTIONS_H;
    MkGroup(g_pageLive, L" Live readings ", M, gy, GW, liveH - gy - M);
    g_live = Mk(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                WS_EX_CLIENTEDGE, LBLX, gy + 26, GW - 32, liveH - gy - M - 36,
                g_pageLive, IDC_LIVE, g_mono);

    LoadSettings();
    UpdateSourceState();
}

void PaintHeader(HWND w, HDC hdc) {
    RECT rc; GetClientRect(w, &rc);
    RECT hd = { 0, 0, rc.right, HEADER - 2 };
    HBRUSH b = CreateSolidBrush(RGB(24, 25, 28));
    FillRect(hdc, &hd, b); DeleteObject(b);
    RECT accent = { 0, HEADER - 2, rc.right, HEADER };
    HBRUSH ab = CreateSolidBrush(RGB(237, 28, 36));
    FillRect(hdc, &accent, ab); DeleteObject(ab);

    HICON ic = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                 IMAGE_ICON, 38, 38, 0);
    if (ic) { DrawIconEx(hdc, 14, 9, ic, 38, 38, 0, nullptr, DI_NORMAL); DestroyIcon(ic); }

    SetBkMode(hdc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(hdc, g_title);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, 62, 8, L"RadTune", 7);
    SelectObject(hdc, g_sub);
    SetTextColor(hdc, RGB(168, 170, 176));
    const wchar_t* sub = L"GPU tuning, made persistent";
    TextOutW(hdc, 64, 34, sub, (int)wcslen(sub));
    SelectObject(hdc, old);
}

// Owner-draw for push buttons: explicit colours so the label is always legible.
void DrawButton(LPDRAWITEMSTRUCT dis) {
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool primary  = (int)dis->CtlID == IDC_APPLY;
    RECT r = dis->rcItem;

    COLORREF bg, fg, bd;
    if (primary) {
        bg = pressed ? RGB(196, 20, 28) : RGB(224, 32, 40);
        fg = disabled ? RGB(240, 200, 200) : RGB(255, 255, 255);
        bd = RGB(168, 18, 24);
    } else {
        bg = pressed ? RGB(224, 226, 230) : RGB(250, 250, 250);
        fg = disabled ? RGB(150, 152, 156) : RGB(28, 28, 30);
        bd = RGB(150, 154, 160);
    }

    HBRUSH fill = CreateSolidBrush(bg);
    FillRect(dis->hDC, &r, fill);
    DeleteObject(fill);

    HPEN pen = CreatePen(PS_SOLID, 1, bd);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBr = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBr);
    DeleteObject(pen);

    wchar_t txt[64] = { 0 };
    GetWindowTextW(dis->hwndItem, txt, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    HGDIOBJ oldFont = SelectObject(dis->hDC, g_font);
    DrawTextW(dis->hDC, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldFont);

    if (dis->itemState & ODS_FOCUS) {
        RECT fr = r; InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }
}

// Window procedure for the two tab pages. Controls now live on a page, so their
// notifications arrive here instead of at the main window - forward them up so
// the single handler in WndProc keeps owning all the behaviour.
LRESULT CALLBACK PageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
    case WM_DRAWITEM:
        return SendMessageW(GetParent(h), msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_main  = w;
        g_font  = MakeFont(9,  FW_NORMAL,   L"Segoe UI");
        g_mono  = MakeFont(9,  FW_NORMAL,   L"Consolas");
        g_title = MakeFont(15, FW_SEMIBOLD, L"Segoe UI");
        g_sub   = MakeFont(9,  FW_NORMAL,   L"Segoe UI");
        BuildUi(w);
        // Enumerate the GPUs straight away; the result chains into a -get so the
        // form shows the card's real values without the user pressing anything.
        StartRun(RUN_GPUS, L"-gpus");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(w, &ps);
        PaintHeader(w, hdc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        auto* dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType == ODT_BUTTON) { DrawButton(dis); return TRUE; }
        return DefWindowProcW(w, msg, wp, lp);
    }
    case WM_APP_RESULT: {
        auto* res = reinterpret_cast<RunOut*>(lp);
        OnRunResult(res->kind, res->text, res->exitCode);
        delete res;
        return 0;
    }
    case WM_APP_LIVE: {
        auto* s = reinterpret_cast<std::string*>(lp);
        SetWindowTextW(g_live, AcpToWide(FormatMonitor(*s)).c_str());
        delete s;
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) {
            const int sel = (int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0);   // 0 Tuning, 1 Fan, 2 Live
            ShowWindow(g_pageTuning, sel == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_pageFan,    sel == 1 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_actions,    sel != 2 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_pageLive,   sel == 2 ? SW_SHOW : SW_HIDE);
            // Only stream while the Live tab is actually on screen.
            if (sel == 2) StartLive(); else StopLive();
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SOURCE: if (HIWORD(wp) == CBN_SELCHANGE) UpdateSourceState(); return 0;
        case IDC_GPU:
            // Switching card: re-read its tuning, and re-point a running stream.
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (g_liveProc) { StopLive(); StartLive(); }
                OnReadGpu();
            }
            return 0;
        case IDC_READ:     OnReadGpu();  return 0;
        case IDC_APPLY:    OnApply();    return 0;
        case IDC_SCHEDULE: OnSchedule(); return 0;
        case IDC_STATUS:   StartRun(RUN_SHOW, L"-schedule status"); return 0;
        case IDC_REMOVE:   StartRun(RUN_SHOW, L"-schedule remove"); return 0;
        case IDC_BROWSE:   OnBrowse();   return 0;
        }
        return 0;
    case WM_DESTROY:
        StopLive();
        SaveSettings();
        DeleteObject(g_font); DeleteObject(g_mono); DeleteObject(g_title); DeleteObject(g_sub);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd) {
    const INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RadTuneGuiWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);

    // Opaque host class for the Live overlay page.
    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PageProc;
    pc.hInstance = hInst;
    pc.lpszClassName = L"RadTunePage";
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&pc);

    // Size the frame from the client area the layout needs, not the other way
    // round: guessing the frame height is how the form used to overflow/gap.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT frame = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&frame, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RadTune GUI", style,
        CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

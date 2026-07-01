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
#include <cstdlib>
#include <initializer_list>

#define IDI_APPICON 101

namespace {
// ---------------------------------------------------------------------------
// Layout constants and control IDs
// ---------------------------------------------------------------------------
constexpr int M = 16;          // outer margin
constexpr int HEADER = 58;     // header strip height
constexpr int LBLX = 32, FLDX = 196, FLDW = 150, EDH = 24;

enum : int {
    IDC_SOURCE = 1001, IDC_GPU, IDC_READ, IDC_CORE, IDC_COREMIN, IDC_VOLT,
    IDC_VRAM, IDC_POWER, IDC_ZERORPM, IDC_PROFILE, IDC_BROWSE, IDC_APPLY,
    IDC_TRIGGER, IDC_TIME, IDC_SCHEDULE, IDC_STATUS, IDC_REMOVE, IDC_OUTPUT
};

HFONT g_font = nullptr, g_mono = nullptr, g_title = nullptr, g_sub = nullptr;
HWND g_source, g_gpu, g_core, g_coremin, g_volt, g_vram, g_power, g_zerorpm,
     g_profile, g_browse, g_trigger, g_time, g_output;

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
    c(g_source, L"source"); e(g_gpu, L"gpu"); e(g_core, L"core"); e(g_coremin, L"coremin");
    e(g_volt, L"volt"); e(g_vram, L"vram"); e(g_power, L"power"); c(g_zerorpm, L"zerorpm");
    e(g_profile, L"profile"); c(g_trigger, L"trigger"); e(g_time, L"time");
}

void LoadSettings() {
    auto e = [](HWND h, const wchar_t* n) { std::wstring v = RegRead(n); if (!v.empty()) SetWindowTextW(h, v.c_str()); };
    auto c = [](HWND h, const wchar_t* n) { std::wstring v = RegRead(n); if (!v.empty()) SetCombo(h, _wtoi(v.c_str())); };
    c(g_source, L"source"); e(g_gpu, L"gpu"); e(g_core, L"core"); e(g_coremin, L"coremin");
    e(g_volt, L"volt"); e(g_vram, L"vram"); e(g_power, L"power"); c(g_zerorpm, L"zerorpm");
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
    for (HWND h : { g_core, g_coremin, g_volt, g_vram, g_power, g_zerorpm })
        EnableWindow(h, !profile);
    EnableWindow(g_profile, profile);
    EnableWindow(g_browse, profile);
}

std::wstring BuildPayload(std::wstring& err) {
    const std::wstring gpu = Trim(GetText(g_gpu));
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
    const int zr = ComboSel(g_zerorpm);   // 0 leave, 1 enable, 2 disable
    if (zr == 1) tail += L" zerorpm=1";
    else if (zr == 2) tail += L" zerorpm=0";
    if (tail.empty()) { err = L"Enter at least one tuning value."; return L""; }
    std::wstring s = L"-set";
    if (!gpu.empty()) s += L" gpu=" + gpu;
    return s + tail;
}

std::string RunAndCapture(const std::wstring& cmdLine) {
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
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

void ShowOutput(const std::string& acp) {
    SetWindowTextW(g_output, AcpToWide(CleanOutput(acp)).c_str());
}

void RunRadTune(const std::wstring& args) {
    const std::wstring cmd = Quote(RadTunePath()) + L" " + args;
    ShowOutput("> " + WideToAcp(cmd) + "\r\n\r\n" + RunAndCapture(cmd));
    SaveSettings();
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
    std::wstring trigger = Trim(GetText(g_trigger));  // logon | startup | daily
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

// Parse "key=value" lines from RadTune -get and prefill the manual fields.
void OnReadGpu() {
    std::wstring gpu = Trim(GetText(g_gpu));
    if (gpu.empty()) gpu = L"0";
    const std::wstring cmd = Quote(RadTunePath()) + L" -get gpu=" + gpu;
    const std::string raw = RunAndCapture(cmd);
    const std::string clean = CleanOutput(raw);

    SetCombo(g_source, 0);  // switch to Manual so fields are visible/editable
    UpdateSourceState();

    bool got = false;
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
        else if (key == "coremin") SetWindowTextW(g_coremin, v.c_str());
        else if (key == "volt") SetWindowTextW(g_volt, v.c_str());
        else if (key == "vram") SetWindowTextW(g_vram, v.c_str());
        else if (key == "power") SetWindowTextW(g_power, v.c_str());
        else if (key == "zerorpm") SetCombo(g_zerorpm, v == L"1" ? 1 : 2);
        else known = false;
        if (known) got = true;
    }
    const std::string note = got ? "" : "[GUI] No tuning values were read from the GPU.\r\n\r\n";
    ShowOutput(note + "> " + WideToAcp(cmd) + "\r\n\r\n" + raw);
    SaveSettings();
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

    // ---- Group 1: GPU tuning ----
    int gy = HEADER + 8;
    MkGroup(w, L" GPU tuning ", M, gy, GW, 336);
    int y = gy + 24;
    MkLabel(w, L"Source", LBLX, y + 4, FLDX - LBLX - 8);
    g_source = MkCombo(w, IDC_SOURCE, FLDX, y, 300, { L"Manual tuning  (-set)", L"Load profile  (-load)" });
    y += 32;

    MkLabel(w, L"GPU index", LBLX, y + 4, FLDX - LBLX - 8);
    g_gpu = MkEdit(w, IDC_GPU, FLDX, y, 60);
    SetWindowTextW(g_gpu, L"0");
    MkButton(w, IDC_READ, L"Read from GPU", FLDX + 72, y - 1, 150, 26);
    y += 34;

    g_core    = LabeledEdit(w, L"Core max (MHz)",      IDC_CORE,    y); y += 30;
    g_coremin = LabeledEdit(w, L"Core min (MHz)",      IDC_COREMIN, y); y += 30;
    g_volt    = LabeledEdit(w, L"Voltage offset (mV)", IDC_VOLT,    y); y += 30;
    g_vram    = LabeledEdit(w, L"VRAM max (MHz)",      IDC_VRAM,    y); y += 30;
    g_power   = LabeledEdit(w, L"Power limit (%)",     IDC_POWER,   y); y += 30;

    MkLabel(w, L"Zero RPM fan", LBLX, y + 4, FLDX - LBLX - 8);
    g_zerorpm = MkCombo(w, IDC_ZERORPM, FLDX, y, 200, { L"Leave unchanged", L"Enable", L"Disable" });
    y += 32;

    MkLabel(w, L"Profile .xml", LBLX, y + 4, FLDX - LBLX - 8);
    g_profile = MkEdit(w, IDC_PROFILE, FLDX, y, GW - (FLDX - M) - 120);
    g_browse = MkButton(w, IDC_BROWSE, L"Browse...", M + GW - 104, y - 1, 88, 26);

    // ---- Apply button ----
    int by = gy + 336 + 10;
    MkButton(w, IDC_APPLY, L"Apply now", M, by, 200, 32);

    // ---- Group 2: Automation ----
    int ay = by + 44;
    MkGroup(w, L" Automation (Task Scheduler) ", M, ay, GW, 104);
    int ty = ay + 26;
    MkLabel(w, L"Trigger", LBLX, ty + 4, 55);
    g_trigger = MkCombo(w, IDC_TRIGGER, 92, ty, 110, { L"logon", L"startup", L"daily" });
    MkLabel(w, L"Time (daily)", 224, ty + 4, 80);
    g_time = MkEdit(w, IDC_TIME, 312, ty, 70);
    SetWindowTextW(g_time, L"09:00");
    ty += 36;
    MkButton(w, IDC_SCHEDULE, L"Create schedule", LBLX,       ty, 170, 28);
    MkButton(w, IDC_STATUS,   L"Show status",     LBLX + 182, ty, 140, 28);
    MkButton(w, IDC_REMOVE,   L"Remove schedule", LBLX + 330, ty, 170, 28);

    // ---- Group 3: Output ----
    int oy = ay + 104 + 10;
    MkGroup(w, L" Output ", M, oy, GW, rc.bottom - oy - M);
    g_output = Mk(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                  WS_EX_CLIENTEDGE, LBLX, oy + 22, GW - 32, rc.bottom - oy - M - 32, w, IDC_OUTPUT, g_mono);

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

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_font  = MakeFont(9,  FW_NORMAL,   L"Segoe UI");
        g_mono  = MakeFont(9,  FW_NORMAL,   L"Consolas");
        g_title = MakeFont(15, FW_SEMIBOLD, L"Segoe UI");
        g_sub   = MakeFont(9,  FW_NORMAL,   L"Segoe UI");
        BuildUi(w);
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SOURCE: if (HIWORD(wp) == CBN_SELCHANGE) UpdateSourceState(); return 0;
        case IDC_READ:     OnReadGpu();  return 0;
        case IDC_APPLY:    OnApply();    return 0;
        case IDC_SCHEDULE: OnSchedule(); return 0;
        case IDC_STATUS:   RunRadTune(L"-schedule status"); return 0;
        case IDC_REMOVE:   RunRadTune(L"-schedule remove"); return 0;
        case IDC_BROWSE:   OnBrowse();   return 0;
        }
        return 0;
    case WM_DESTROY:
        SaveSettings();
        DeleteObject(g_font); DeleteObject(g_mono); DeleteObject(g_title); DeleteObject(g_sub);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd) {
    const INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
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

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RadTune GUI",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 812, nullptr, nullptr, hInst, nullptr);
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

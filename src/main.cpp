// main.cpp - Win32 GUI for Ultimate AutoClicker.
#include "core.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

using namespace ac;

// ---------------------------------------------------------------- ids
enum {
    ID_HRS = 201, ID_MINS, ID_SECS, ID_MS, ID_US, ID_WARN_STATIC,
    ID_BTN_COMBO, ID_TYPE_COMBO, ID_MODE_COMBO,
    ID_ACTION_COMBO, ID_TEXT_EDIT,
    ID_RAND_CHECK, ID_RAND_MIN, ID_RAND_MAX,
    ID_REP_INF, ID_REP_TIMES, ID_REP_COUNT,
    ID_POS_CUR, ID_POS_CUST, ID_POS_X, ID_POS_Y, ID_PICK_BTN,
    ID_POS_SEQ, ID_SEQ_BTN,
    ID_DELAY_EDIT, ID_HOLD_EDIT,
    ID_HOTKEY_LABEL, ID_CHANGE_HOTKEY_BTN,
    ID_TOP_CHECK, ID_MAIN_BTN,
    ID_STAT_CLICKS,
    ID_PROF_COMBO, ID_PROF_SAVE, ID_PROF_LOAD, ID_PROF_DEL,
    ID_UI_MODE, ID_UI_TABS, ID_SAFETY_STATUS,
    ID_SEQ_LIST = 401, ID_SEQ_ADD_BTN, ID_SEQ_DEL_BTN, ID_SEQ_CLR_BTN,
    ID_SEQ_IN_X, ID_SEQ_IN_Y, ID_SEQ_IN_D, ID_SEQ_REC_BTN
};

#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_PICK   (WM_APP + 2)
#define WM_APP_REC    (WM_APP + 3)
#define WM_APP_STATE  (WM_APP + 4)
#define WM_APP_EMERGENCY (WM_APP + 5)

#define IDI_APPICON 101

// ---------------------------------------------------------------- globals
static Config   g_cfg;
static Engine*  g_engine = nullptr;
static Clock*   g_clock = nullptr;
static InputSink* g_sink = nullptr;

static HWND  g_mainWnd = nullptr;
static HFONT g_font = nullptr, g_bold = nullptr;
static HHOOK g_kbHook = nullptr, g_mouseHook = nullptr;
static HHOOK g_emergencyMouseHook = nullptr;
static HWND g_emergencyOverlay = nullptr;
static ULONGLONG g_emergencyEffectStart = 0;
static BOOL g_emergencyAnimate = TRUE;
static HANDLE hWake = nullptr, hInterrupt = nullptr;
static bool  g_running = true;
static bool  g_periodActive = false;
static bool  g_lastBusy = false;
static bool  g_advanced = false;
static int MainContentHeight() { return g_advanced ? 570 : 530; }
enum { UI_INTERVAL, UI_RANDOM, UI_CLICK, UI_ACTION, UI_REPEAT, UI_POSITION,
       UI_TIMING, UI_SHORTCUT, UI_PROFILES, UI_FOOTER };
struct MainControl { HWND hwnd; int x, y, w, h, section; };
static std::vector<MainControl> g_mainControls;

static int   g_pickMode = 0;              // 0 none, 1 pick location, 2 record clicks
static bool  g_hotkeyDlgOpen = false;
static bool  g_seqDlgOpen = false;
static HWND  g_seqDlg = nullptr;

// hotkey tracking
static std::vector<int> g_pressedKeys;
static BYTE g_heldMods = 0;
static bool g_capturing = false;
static std::vector<int> g_captureHeld;
static BYTE g_captureHeldMods = 0;
static Combo g_captureResult;
static bool  g_captureValid = false;
static HWND  g_hotkeyDlg = nullptr;

static std::vector<std::wstring> g_profiles;
static std::map<std::wstring, std::wstring> g_profileData;
static std::wstring g_iniPath;

// ---------------------------------------------------------------- win32 clock / sink
struct WinClock : Clock {
    HANDLE hTimer = nullptr;
    HANDLE hInterrupt = nullptr;
    LARGE_INTEGER qpf{};

    unsigned long long NowUs() override {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (unsigned long long)((c.QuadPart * 1000000LL) / qpf.QuadPart);
    }

    bool WaitUntilUs(unsigned long long targetUs, const std::atomic<bool>* cancel) override {
        for (;;) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            unsigned long long nowUs = (unsigned long long)((now.QuadPart * 1000000LL) / qpf.QuadPart);
            if (nowUs >= targetUs) return true;
            if (cancel && cancel->load()) return false;

            LONGLONG remainingTicks = (LONGLONG)(((long long)(targetUs - nowUs) * qpf.QuadPart) / 1000000LL);
            LONGLONG coarseThreshold = (2000LL * qpf.QuadPart) / 1000000LL;
            if (remainingTicks > coarseThreshold) {
                LONGLONG coarseTicks = remainingTicks - (1500LL * qpf.QuadPart) / 1000000LL;
                LARGE_INTEGER due; due.QuadPart = -((coarseTicks * 10000000LL) / qpf.QuadPart);
                SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, 0);
                HANDLE h[2] = { hTimer, hInterrupt };
                DWORD res = WaitForMultipleObjects(2, h, FALSE, INFINITE);
                CancelWaitableTimer(hTimer);
                if (res == WAIT_OBJECT_0 + 1) return false;    // interrupted
            } else {
                YieldProcessor();
            }
        }
    }
};

struct WinSink : InputSink {
    void Send(const INPUT* in, int count) override { SendInput(count, (LPINPUT)in, sizeof(INPUT)); }
    void MoveCursor(int x, int y) override {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)(((x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535LL) /
            std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
        in.mi.dy = (LONG)(((y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535LL) /
            std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dwExtraInfo = kInjectTag;
        Send(&in, 1);
    }
};

// ---------------------------------------------------------------- helpers
static void SetText(HWND h, const std::wstring& s) {
    std::wstring current(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, &current[0], (int)current.size());
    current.resize(wcslen(current.c_str()));
    if (current != s) SetWindowTextW(h, s.c_str());
}
static void SetInt(HWND h, int id, int v) { SetDlgItemInt(h, id, (UINT)v, TRUE); }
static int  GetInt(HWND h, int id) { return (int)GetDlgItemInt(h, id, nullptr, TRUE); }
static bool IsChecked(HWND h, int id) { return SendMessage(GetDlgItem(h, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void SetChecked(HWND h, int id, bool on) { SendMessage(GetDlgItem(h, id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static std::wstring GetText(HWND h, int id) {
    wchar_t buf[2048] = {0};
    GetDlgItemTextW(h, id, buf, 2048);
    return buf;
}
static double GetDouble(HWND h, int id) {
    std::wstring s = GetText(h, id);
    for (wchar_t& ch : s) if (ch == L',') ch = L'.';
    return wcstod(s.c_str(), nullptr);
}
static std::wstring Fmt(const wchar_t* f, ...) {
    // MSVCRT's wide printf reads %s as a *narrow* string, which silently
    // truncates wide arguments. Promote every %s to %ls.
    std::wstring fmt(f);
    for (size_t i = 0; i + 1 < fmt.size(); i++) {
        if (fmt[i] == L'%') {
            if (fmt[i + 1] == L'%') { i++; continue; }
            if (fmt[i + 1] == L's') fmt[i + 1] = L'l';
        }
    }
    wchar_t buf[512] = {0};
    va_list ap; va_start(ap, f);
    std::vswprintf(buf, 512, fmt.c_str(), ap);
    va_end(ap);
    return buf;
}

static UINT WindowDpi(HWND hwnd) {
    using GetDpi = UINT (WINAPI*)(HWND);
    auto getDpi = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpi) return getDpi(hwnd);
    HDC dc = GetDC(hwnd);
    UINT dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi;
}

static void ScaleUi(HWND hwnd, UINT dpi) {
    UINT oldDpi = (UINT)(UINT_PTR)GetPropW(hwnd, L"UiDpi");
    if (!oldDpi) oldDpi = 96;
    HFONT font = CreateFontW(MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(MulDiv(16, dpi, 96), 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT oldBold = (HFONT)GetPropW(hwnd, L"UiBold");
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, hwnd, (POINT*)&r, 2);
        wchar_t cls[32];
        GetClassNameW(child, cls, 32);
        int height = r.bottom - r.top;
        if (wcscmp(cls, L"ComboBox") == 0) {
            RECT drop;
            SendMessage(child, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&drop);
            height = drop.bottom - drop.top;
        }
        HFONT current = (HFONT)SendMessage(child, WM_GETFONT, 0, 0);
        SendMessage(child, WM_SETFONT, (WPARAM)((current == g_bold || (oldBold && current == oldBold)) ? bold : font), FALSE);
        SetWindowPos(child, nullptr, MulDiv(r.left, dpi, oldDpi), MulDiv(r.top, dpi, oldDpi),
            MulDiv(r.right - r.left, dpi, oldDpi), MulDiv(height, dpi, oldDpi), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    HFONT oldFont = (HFONT)RemovePropW(hwnd, L"UiFont");
    if (oldFont) DeleteObject(oldFont);
    if (oldBold) DeleteObject(oldBold);
    SetPropW(hwnd, L"UiFont", font);
    SetPropW(hwnd, L"UiBold", bold);
    SetPropW(hwnd, L"UiDpi", (HANDLE)(UINT_PTR)dpi);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

static void FreeUiFonts(HWND hwnd) {
    HFONT font = (HFONT)RemovePropW(hwnd, L"UiFont");
    HFONT bold = (HFONT)RemovePropW(hwnd, L"UiBold");
    if (font) DeleteObject(font);
    if (bold) DeleteObject(bold);
    RemovePropW(hwnd, L"UiDpi");
}

static void SizeClient(HWND hwnd, int width, int height) {
    UINT dpi = WindowDpi(hwnd);
    UINT contentDpi = hwnd == g_mainWnd ? 96 : dpi;
    RECT r = {0, 0, MulDiv(width, contentDpi, 96), MulDiv(height, contentDpi, 96)};
    using AdjustRect = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adjust = (AdjustRect)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (hwnd == g_mainWnd) style &= ~WS_VSCROLL;
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (adjust) adjust(&r, style, FALSE, exStyle, dpi);
    else AdjustWindowRectEx(&r, style, FALSE, exStyle);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    int w = r.right - r.left;
    int h = std::min((int)(r.bottom - r.top), (int)(monitor.rcWork.bottom - monitor.rcWork.top));
    RECT owner;
    GetWindowRect(GetWindow(hwnd, GW_OWNER) ? GetWindow(hwnd, GW_OWNER) : hwnd, &owner);
    int x = std::max((int)monitor.rcWork.left, std::min((int)(owner.left + owner.right - w) / 2, (int)monitor.rcWork.right - w));
    int y = std::max((int)monitor.rcWork.top, std::min((int)(owner.top + owner.bottom - h) / 2, (int)monitor.rcWork.bottom - h));
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ScrollMain(HWND hwnd, int position) {
    SCROLLINFO info = {sizeof(info), SIF_POS};
    GetScrollInfo(hwnd, SB_VERT, &info);
    int old = info.nPos;
    info.nPos = position;
    SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
    GetScrollInfo(hwnd, SB_VERT, &info);
    ScrollWindowEx(hwnd, 0, old - info.nPos, nullptr, nullptr, nullptr, nullptr,
        SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
}

static void UpdateScroll(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    GetScrollInfo(hwnd, SB_VERT, &info);
    int old = info.nPos;
    info.nMin = 0;
    info.nMax = MainContentHeight() - 1;
    info.nPage = client.bottom;
    SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
    GetScrollInfo(hwnd, SB_VERT, &info);
    if (old != info.nPos)
        ScrollWindowEx(hwnd, 0, old - info.nPos, nullptr, nullptr, nullptr, nullptr,
            SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
}

// ---------------------------------------------------------------- ini (portable, next to the exe)
static std::wstring IniPath() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"UltimateAutoClicker.ini";
    return p.substr(0, slash + 1) + L"UltimateAutoClicker.ini";
}

// the ini is plain UTF-8 so it stays readable and hand editable
static std::wstring ReadFileText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::string bytes;
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, 4096, &n, nullptr) && n) bytes.append(buf, n);
    CloseHandle(h);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);                                  // strip BOM
    if (bytes.empty()) return L"";
    int nch = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out;
    out.resize(nch);
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &out[0], nch);
    return out;
}

static bool WriteFileText(const std::wstring& path, const std::wstring& text) {
    int nch = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes;
    bytes.resize(nch);
    if (nch) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &bytes[0], nch, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == bytes.size();
}

// section -> key -> value.  The [Settings] section values are joined with ';'
// so a single blob can span several lines.
static std::map<std::wstring, std::wstring> ReadIni(const std::wstring& path) {
    std::map<std::wstring, std::wstring> out;
    std::wstring text = ReadFileText(path);
    std::wstring section = L"";
    std::wstring joined;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            if (section == L"Settings" && !joined.empty()) out[L"Settings/config"] = joined;
            joined.clear();
            size_t e = line.find(L']');
            section = (e == std::wstring::npos) ? L"" : line.substr(1, e - 1);
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq), val = line.substr(eq + 1);
        if (section == L"Settings") {
            if (!joined.empty()) joined += L";";
            joined += key + L"=" + val;
        } else {
            out[section + L"/" + key] = val;
        }
    }
    if (section == L"Settings" && !joined.empty()) out[L"Settings/config"] = joined;
    return out;
}

static void SaveIni() {
    std::wstring blob = EncodeConfig(g_cfg);
    std::wstring out = L"[Settings]\r\n";
    // one key per line: readable and still a single blob
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t sc = blob.find(L';', pos);
        std::wstring part = blob.substr(pos, sc == std::wstring::npos ? std::wstring::npos : sc - pos);
        if (!part.empty()) out += part + L"\r\n";
        if (sc == std::wstring::npos) break;
        pos = sc + 1;
    }
    out += L"\r\n[Profiles]\r\n";
    out += Fmt(L"count=%d\r\n", (int)g_profiles.size());
    for (size_t i = 0; i < g_profiles.size(); i++) {
        out += Fmt(L"name_%d=%ls\r\n", (int)i, g_profiles[i].c_str());
        auto it = g_profileData.find(g_profiles[i]);
        if (it != g_profileData.end()) out += L"data_" + Fmt(L"%d", (int)i) + L"=" + it->second + L"\r\n";
    }
    WriteFileText(g_iniPath, out);
}

static void LoadIni() {
    std::map<std::wstring, std::wstring> ini = ReadIni(g_iniPath);
    auto it = ini.find(L"Settings/config");
    if (it != ini.end()) {
        Config c;
        if (DecodeConfig(it->second, c)) g_cfg = c;
    }
    g_profiles.clear();
    g_profileData.clear();
    for (size_t i = 0;; i++) {
        auto n = ini.find(L"Profiles/name_" + Fmt(L"%d", (int)i));
        if (n == ini.end()) break;
        std::wstring name = n->second;
        auto d = ini.find(L"Profiles/data_" + Fmt(L"%d", (int)i));
        g_profiles.push_back(name);
        if (d != ini.end()) g_profileData[name] = d->second;
    }
}

// ---------------------------------------------------------------- engine plumbing
static void ApplyUiToConfig() {
    g_cfg.intervalUs = ClampIntervalUs(FieldsToUs(
        GetDouble(g_mainWnd, ID_HRS), GetDouble(g_mainWnd, ID_MINS), GetDouble(g_mainWnd, ID_SECS),
        GetDouble(g_mainWnd, ID_MS), GetDouble(g_mainWnd, ID_US)));
    g_cfg.randomize = IsChecked(g_mainWnd, ID_RAND_CHECK);
    g_cfg.randMinUs = ClampIntervalUs((long long)(GetDouble(g_mainWnd, ID_RAND_MIN) * 1000.0));
    g_cfg.randMaxUs = ClampIntervalUs((long long)(GetDouble(g_mainWnd, ID_RAND_MAX) * 1000.0));
    if (g_cfg.randMaxUs < g_cfg.randMinUs) std::swap(g_cfg.randMinUs, g_cfg.randMaxUs);

    g_cfg.mouseBtn  = (int)SendMessage(GetDlgItem(g_mainWnd, ID_BTN_COMBO), CB_GETCURSEL, 0, 0);
    g_cfg.clickType = (int)SendMessage(GetDlgItem(g_mainWnd, ID_TYPE_COMBO), CB_GETCURSEL, 0, 0);
    g_cfg.clickMode = (int)SendMessage(GetDlgItem(g_mainWnd, ID_MODE_COMBO), CB_GETCURSEL, 0, 0);
    g_cfg.action    = (int)SendMessage(GetDlgItem(g_mainWnd, ID_ACTION_COMBO), CB_GETCURSEL, 0, 0);
    g_cfg.typeText  = GetText(g_mainWnd, ID_TEXT_EDIT);

    g_cfg.maxClicks = IsChecked(g_mainWnd, ID_REP_TIMES) ? GetInt(g_mainWnd, ID_REP_COUNT) : 0;
    if (g_cfg.maxClicks < 0) g_cfg.maxClicks = 0;

    if (IsChecked(g_mainWnd, ID_POS_CUST))      g_cfg.posMode = POS_FIXED;
    else if (IsChecked(g_mainWnd, ID_POS_SEQ))  g_cfg.posMode = POS_SEQ;
    else                                        g_cfg.posMode = POS_CURRENT;
    g_cfg.targetX = GetInt(g_mainWnd, ID_POS_X);
    g_cfg.targetY = GetInt(g_mainWnd, ID_POS_Y);

    g_cfg.startDelaySec = GetInt(g_mainWnd, ID_DELAY_EDIT);
    if (g_cfg.startDelaySec < 0) g_cfg.startDelaySec = 0;
    if (g_cfg.startDelaySec > 300) g_cfg.startDelaySec = 300;
    g_cfg.clickHoldUs = (long long)GetDouble(g_mainWnd, ID_HOLD_EDIT) * 1000.0;
    g_cfg.alwaysOnTop = IsChecked(g_mainWnd, ID_TOP_CHECK);
}

static void ApplyConfigToUi() {
    double h, m, s, ms, us;
    UsToFields(g_cfg.intervalUs, h, m, s, ms, us);
    SetDlgItemTextW(g_mainWnd, ID_HRS, Fmt(L"%.0f", h).c_str());
    SetDlgItemTextW(g_mainWnd, ID_MINS, Fmt(L"%.0f", m).c_str());
    SetDlgItemTextW(g_mainWnd, ID_SECS, Fmt(L"%.0f", s).c_str());
    SetDlgItemTextW(g_mainWnd, ID_MS, Fmt(L"%.0f", ms).c_str());
    SetDlgItemTextW(g_mainWnd, ID_US, Fmt(L"%.0f", us).c_str());

    SetChecked(g_mainWnd, ID_RAND_CHECK, g_cfg.randomize);
    SetDlgItemTextW(g_mainWnd, ID_RAND_MIN, Fmt(L"%.2f", (double)g_cfg.randMinUs / 1000.0).c_str());
    SetDlgItemTextW(g_mainWnd, ID_RAND_MAX, Fmt(L"%.2f", (double)g_cfg.randMaxUs / 1000.0).c_str());

    SendMessage(GetDlgItem(g_mainWnd, ID_BTN_COMBO), CB_SETCURSEL, g_cfg.mouseBtn, 0);
    SendMessage(GetDlgItem(g_mainWnd, ID_TYPE_COMBO), CB_SETCURSEL, g_cfg.clickType, 0);
    SendMessage(GetDlgItem(g_mainWnd, ID_MODE_COMBO), CB_SETCURSEL, g_cfg.clickMode, 0);
    SendMessage(GetDlgItem(g_mainWnd, ID_ACTION_COMBO), CB_SETCURSEL, g_cfg.action, 0);
    SetDlgItemTextW(g_mainWnd, ID_TEXT_EDIT, g_cfg.typeText.c_str());

    SetChecked(g_mainWnd, ID_REP_INF, g_cfg.maxClicks == 0);
    SetChecked(g_mainWnd, ID_REP_TIMES, g_cfg.maxClicks != 0);
    SetInt(g_mainWnd, ID_REP_COUNT, g_cfg.maxClicks ? g_cfg.maxClicks : 10);

    SetChecked(g_mainWnd, ID_POS_CUR, g_cfg.posMode == POS_CURRENT);
    SetChecked(g_mainWnd, ID_POS_CUST, g_cfg.posMode == POS_FIXED);
    SetChecked(g_mainWnd, ID_POS_SEQ, g_cfg.posMode == POS_SEQ);
    SetInt(g_mainWnd, ID_POS_X, g_cfg.targetX);
    SetInt(g_mainWnd, ID_POS_Y, g_cfg.targetY);

    SetInt(g_mainWnd, ID_DELAY_EDIT, g_cfg.startDelaySec);
    SetDlgItemTextW(g_mainWnd, ID_HOLD_EDIT, Fmt(L"%.0f", (double)g_cfg.clickHoldUs / 1000.0).c_str());
    SetChecked(g_mainWnd, ID_TOP_CHECK, g_cfg.alwaysOnTop);

    std::wstring hk = g_cfg.hotkey.empty() ? std::wstring(L"(none)") : g_cfg.hotkey.Name();
    SetText(GetDlgItem(g_mainWnd, ID_HOTKEY_LABEL), L"Hotkey: " + hk);
    SetWindowPos(g_mainWnd, g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void UpdateEnabledStates() {
    int action = (int)SendMessage(GetDlgItem(g_mainWnd, ID_ACTION_COMBO), CB_GETCURSEL, 0, 0);
    bool isClick = (action == ACT_CLICK);
    EnableWindow(GetDlgItem(g_mainWnd, ID_BTN_COMBO), isClick);
    EnableWindow(GetDlgItem(g_mainWnd, ID_TYPE_COMBO), isClick);
    EnableWindow(GetDlgItem(g_mainWnd, ID_TEXT_EDIT), action == ACT_TYPE);
    EnableWindow(GetDlgItem(g_mainWnd, ID_RAND_MIN), IsChecked(g_mainWnd, ID_RAND_CHECK));
    EnableWindow(GetDlgItem(g_mainWnd, ID_RAND_MAX), IsChecked(g_mainWnd, ID_RAND_CHECK));
    EnableWindow(GetDlgItem(g_mainWnd, ID_REP_COUNT), IsChecked(g_mainWnd, ID_REP_TIMES));
    bool fixed = IsChecked(g_mainWnd, ID_POS_CUST);
    EnableWindow(GetDlgItem(g_mainWnd, ID_POS_X), fixed);
    EnableWindow(GetDlgItem(g_mainWnd, ID_POS_Y), fixed);
}

static void UpdateWarning() {
    long long iv = g_cfg.intervalUs;
    double cps = iv > 0 ? 1000000.0 / (double)iv : 0.0;
    wchar_t buf[256];
    if (g_cfg.randomize) {
        double lo = (double)g_cfg.randMinUs / 1000.0, hi = (double)g_cfg.randMaxUs / 1000.0;
        swprintf_s(buf, 256, L"Random interval: %.2f - %.2f ms  (%.0f - %.0f CPS)", lo, hi, 1000.0 / hi, 1000.0 / lo);
    } else {
        swprintf_s(buf, 256, L"Interval: %.3f ms  (%.2f CPS)", (double)iv / 1000.0, cps);
    }
    std::wstring warn;
    if (g_cfg.randomize ? g_cfg.randMinUs < 5000 : iv < 5000)
        warn = std::wstring(buf) + L"\nBelow 5 ms: the target app may lag.";
    else
        warn = buf;
    SetText(GetDlgItem(g_mainWnd, ID_WARN_STATIC), warn);
}

static void ConvertAndSaveInterval() {
    ApplyUiToConfig();
    double h, m, s, ms, us;
    UsToFields(g_cfg.intervalUs, h, m, s, ms, us);
    SetDlgItemTextW(g_mainWnd, ID_HRS, Fmt(L"%.0f", h).c_str());
    SetDlgItemTextW(g_mainWnd, ID_MINS, Fmt(L"%.0f", m).c_str());
    SetDlgItemTextW(g_mainWnd, ID_SECS, Fmt(L"%.0f", s).c_str());
    SetDlgItemTextW(g_mainWnd, ID_MS, Fmt(L"%.0f", ms).c_str());
    SetDlgItemTextW(g_mainWnd, ID_US, Fmt(L"%.0f", us).c_str());
    UpdateWarning();
    if (g_engine) g_engine->ApplyConfig(g_cfg);
}

// ---------------------------------------------------------------- hotkey helpers
static bool IsModVk(int vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:    case VK_RWIN:
            return true;
    }
    return false;
}
static BYTE ModBit(int vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:   return MOD_SHIFT;
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:    return MOD_ALT;
        case VK_LWIN:    case VK_RWIN:                     return MOD_WIN;
        default: return 0;
    }
}
static bool VecHas(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
static void VecDel(std::vector<int>& v, int x) {
    v.erase(std::remove(v.begin(), v.end(), x), v.end());
}
static bool OurWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------- start / stop
static void TimerResolutionOn() {
    if (g_periodActive) return;
    g_periodActive = true;
    typedef MMRESULT (WINAPI *pfnTimeBeginPeriod)(UINT);
    HMODULE w = LoadLibraryA("winmm.dll");
    if (w) {
        pfnTimeBeginPeriod f = (pfnTimeBeginPeriod)GetProcAddress(w, "timeBeginPeriod");
        if (f) f(1);
        FreeLibrary(w);
    }
}
static void TimerResolutionOff() {
    if (!g_periodActive) return;
    g_periodActive = false;
    typedef MMRESULT (WINAPI *pfnTimeEndPeriod)(UINT);
    HMODULE w = LoadLibraryA("winmm.dll");
    if (w) {
        pfnTimeEndPeriod f = (pfnTimeEndPeriod)GetProcAddress(w, "timeEndPeriod");
        if (f) f(1);
        FreeLibrary(w);
    }
}

// ---------------------------------------------------------------- emergency stop feedback
static LRESULT CALLBACK EmergencyEffectProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const COLORREF transparent = RGB(1, 2, 3);
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_TIMER) {
        ULONGLONG elapsed = GetTickCount64() - g_emergencyEffectStart;
        if (elapsed >= 2500) { KillTimer(hwnd, 1); ShowWindow(hwnd, SW_HIDE); return 0; }
        int pulse = (int)(elapsed % 800);
        if (pulse > 400) pulse = 800 - pulse;
        int alpha = g_emergencyAnimate ? 170 + pulse * 65 / 400 : 235;
        if (g_emergencyAnimate && elapsed > 2000) alpha = alpha * (2500 - elapsed) / 500;
        SetLayeredWindowAttributes(hwnd, transparent, (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC paint = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        HDC dc = CreateCompatibleDC(paint);
        HBITMAP bitmap = CreateCompatibleBitmap(paint, r.right, r.bottom);
        if (!dc || !bitmap) {
            if (dc) DeleteDC(dc);
            if (bitmap) DeleteObject(bitmap);
            EndPaint(hwnd, &ps); return 0;
        }
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        HBRUSH background = CreateSolidBrush(transparent);
        FillRect(dc, &r, background); DeleteObject(background);
        UINT dpi = WindowDpi(hwnd);
        auto px = [dpi](int n) { return MulDiv(n, dpi, 96); };
        HPEN glow = CreatePen(PS_SOLID, px(12), RGB(100, 20, 38));
        HGDIOBJ oldPen = SelectObject(dc, glow);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, px(6), px(6), r.right - px(6), r.bottom - px(6));
        HPEN red = CreatePen(PS_SOLID, px(3), RGB(255, 75, 100));
        SelectObject(dc, red); DeleteObject(glow);
        Rectangle(dc, px(6), px(6), r.right - px(6), r.bottom - px(6));
        int width = std::min(px(460), std::max(1, (int)r.right - px(32)));
        RECT card = {(r.right - width) / 2, px(24), (r.right + width) / 2, px(134)};
        HBRUSH fill = CreateSolidBrush(RGB(31, 18, 25));
        SelectObject(dc, fill);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, px(16), px(16));
        HFONT title = CreateFontW(px(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        HFONT body = CreateFontW(px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, title);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT text = {card.left + px(16), card.top + px(15), card.right - px(16), card.top + px(45)};
        DrawTextW(dc, L"EMERGENCY STOP", -1, &text, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, body);
        text.top = card.top + px(56); text.bottom = card.bottom - px(10);
        std::wstring message = L"All clicks stopped. Use Start";
        if (!g_cfg.hotkey.empty()) message += L" or " + g_cfg.hotkey.Name();
        message += L" to restart.";
        DrawTextW(dc, message.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        BitBlt(paint, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont); SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(title); DeleteObject(body); DeleteObject(red); DeleteObject(fill);
        SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
        EndPaint(hwnd, &ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowEmergencyEffect(POINT point) {
    MONITORINFO monitor = {sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    if (!g_emergencyOverlay) {
        WNDCLASSW wc = {};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = EmergencyEffectProc;
        wc.lpszClassName = L"AutoClickerEmergencyStop";
        RegisterClassW(&wc);
        g_emergencyOverlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"EMERGENCY STOP - all clicks stopped",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!g_emergencyOverlay) return;
    g_emergencyEffectStart = GetTickCount64();
    g_emergencyAnimate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &g_emergencyAnimate, 0);
    SetLayeredWindowAttributes(g_emergencyOverlay, RGB(1, 2, 3), 235, LWA_COLORKEY | LWA_ALPHA);
    RECT r = monitor.rcMonitor;
    SetWindowPos(g_emergencyOverlay, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_emergencyOverlay, nullptr, FALSE);
    SetTimer(g_emergencyOverlay, 1, 16, nullptr);
}

static bool PointAtScreenEdge(POINT point) {
    MONITORINFO monitor = {sizeof(monitor)};
    return GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONULL), &monitor) &&
        IsScreenEdge(point.x, point.y, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right, monitor.rcMonitor.bottom);
}

static void HandleEmergencyMotion(POINT point, bool injected) {
    if (!injected && !g_pickMode && g_engine && g_engine->IsRunning() && PointAtScreenEdge(point) &&
        g_engine->EmergencyStop()) {
        SetEvent(hInterrupt);
        PostMessageW(g_mainWnd, WM_APP_EMERGENCY, (WPARAM)point.x, (LPARAM)point.y);
    }
}

static LRESULT CALLBACK EmergencyMouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && wParam == WM_MOUSEMOVE) {
        auto* mouse = (MSLLHOOKSTRUCT*)lParam;
        HandleEmergencyMotion(mouse->pt, (mouse->flags & LLMHF_INJECTED) || mouse->dwExtraInfo == kInjectTag);
    }
    return CallNextHookEx(g_emergencyMouseHook, code, wParam, lParam);
}

static void ToggleClicking() {
    if (!g_engine) return;
    if (g_engine->IsRunning()) {
        g_engine->Stop();
        SetEvent(hInterrupt);
    } else {
        if (!g_emergencyMouseHook) {
            MessageBoxW(g_mainWnd, L"The screen-edge emergency stop could not be enabled.\nRestart the app before starting clicks.",
                L"Emergency stop unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        if (g_emergencyOverlay) { KillTimer(g_emergencyOverlay, 1); ShowWindow(g_emergencyOverlay, SW_HIDE); }
        ApplyUiToConfig();
        g_engine->ApplyConfig(g_cfg);
        TimerResolutionOn();
        g_engine->Start();
        SetEvent(hWake);
    }
}

static void UpdateButton() {
    EngineStatus st = g_engine->Status();
    std::wstring hk = g_cfg.hotkey.empty() ? std::wstring(L"(none)") : g_cfg.hotkey.Name();
    std::wstring txt;
    if (st.state == ST_COUNTDOWN) {
        txt = Fmt(L"CANCEL - STARTING IN %d s", st.countdownSec);
    } else if (st.state == ST_RUNNING) {
        txt = Fmt(L"STOP CLICKING (%ls)", hk.c_str());
    } else {
        txt = Fmt(L"START CLICKING (%ls)", hk.c_str());
    }
    SetText(GetDlgItem(g_mainWnd, ID_MAIN_BTN), txt);
}

static void UpdateStats() {
    EngineStatus st = g_engine->Status();
    wchar_t buf[64];
    swprintf_s(buf, 64, L"%d", st.clicksDone);
    SetText(GetDlgItem(g_mainWnd, ID_STAT_CLICKS), L"Clicks: " + std::wstring(buf));
    SetText(GetDlgItem(g_mainWnd, ID_SAFETY_STATUS), st.emergencyStopped ?
        L"EMERGENCY STOP - all clicks stopped. Restart manually." : g_emergencyMouseHook ?
        L"Emergency stop: move the cursor to any screen edge." : L"Screen-edge stop unavailable. Clicking is disabled.");

    bool busy = st.state != ST_IDLE;
    if (busy != g_lastBusy) {
        g_lastBusy = busy;
        if (!busy) TimerResolutionOff();
    }
}

// ---------------------------------------------------------------- mouse hook
static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lParam;
        if (m->dwExtraInfo != kInjectTag) {
            if (g_pickMode == 1) {
                PostMessage(g_mainWnd, WM_APP_PICK, (WPARAM)m->pt.x, (LPARAM)m->pt.y);
                g_pickMode = 0;
                return 1;                                    // swallow the picking click
            }
            if (g_pickMode == 2) {
                HWND w = WindowFromPoint(m->pt);
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                if (pid != GetCurrentProcessId())
                    PostMessage(g_mainWnd, WM_APP_REC, (WPARAM)m->pt.x, (LPARAM)m->pt.y);
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------- keyboard hook
static void PreviewCombo(HWND dlg, const std::vector<int>& keys, BYTE mods, bool valid) {
    Combo c; c.mods = mods; c.keys = keys;
    std::wstring name = keys.empty() ? std::wstring(L"(press keys)") : c.Name();
    SetText(GetDlgItem(dlg, 302), (valid ? L"Captured: " : L"Pressing: ") + name);
}

static int HandleKeyDown(KBDLLHOOKSTRUCT* k) {
    int vk = (int)k->vkCode;
    bool mod = IsModVk(vk);

    if (g_capturing) {
        if (mod) {
            g_captureHeldMods |= ModBit(vk);
        } else {
            if (!VecHas(g_captureHeld, vk)) g_captureHeld.push_back(vk);
        }
        g_captureResult.mods = g_captureHeldMods;
        g_captureResult.keys = g_captureHeld;
        g_captureValid = !g_captureHeld.empty();
        PreviewCombo(g_hotkeyDlg, g_captureHeld, g_captureHeldMods, false);
        return 1;                                            // swallow while capturing
    }

    if (mod) { g_heldMods |= ModBit(vk); return 0; }
    if (VecHas(g_pressedKeys, vk)) return 0;                 // auto repeat
    g_pressedKeys.push_back(vk);

    if (!g_hotkeyDlgOpen && !OurWindowFocused()) {
        if (ComboMatches(g_cfg.hotkey, g_pressedKeys, g_heldMods)) {
            g_pressedKeys.clear();
            ToggleClicking();
            return 1;                                        // swallow the trigger key
        }
    }
    return 0;
}

static int HandleKeyUp(KBDLLHOOKSTRUCT* k) {
    int vk = (int)k->vkCode;
    bool mod = IsModVk(vk);

    if (g_capturing) {
        if (mod) {
            g_captureHeldMods &= (BYTE)~ModBit(vk);
        } else {
            VecDel(g_captureHeld, vk);
        }
        if (g_captureHeld.empty() && g_captureHeldMods == 0) {
            if (g_captureValid) {
                PreviewCombo(g_hotkeyDlg, g_captureResult.keys, g_captureResult.mods, true);
            }
        } else {
            PreviewCombo(g_hotkeyDlg, g_captureHeld, g_captureHeldMods, false);
        }
        return 1;
    }

    if (mod) { g_heldMods &= (BYTE)~ModBit(vk); return 0; }
    VecDel(g_pressedKeys, vk);
    return 0;
}

static LRESULT CALLBACK KbHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
        if (k->dwExtraInfo != kInjectTag) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (k->vkCode == VK_ESCAPE && (g_pickMode || g_capturing)) {
                    if (g_capturing && g_hotkeyDlg) PostMessage(g_hotkeyDlg, WM_CLOSE, 0, 0);
                    else PostMessage(g_mainWnd, WM_APP_STATE, 0, 0);   // cancel pick / recording
                    return 1;
                }
                HandleKeyDown(k);
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                HandleKeyUp(k);
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------- worker thread
static DWORD WINAPI ClickerThread(LPVOID) {
    while (g_running) {
        if (!g_engine->IsRunning()) {
            WaitForSingleObject(hWake, 100);
            continue;
        }
        g_engine->Tick();
    }
    return 0;
}

// ---------------------------------------------------------------- tray
static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;

static void TrayAdd() {
    if (g_trayAdded) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(g_nid.szTip, L"Ultimate AutoClicker");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = true;
}
static void TrayRemove() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}
static void ShowTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 9001, L"Show / Hide");
    AppendMenuW(m, MF_STRING, 9002, g_engine && g_engine->IsRunning() ? L"Stop Clicking" : L"Start Clicking");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9003, L"Exit");
    SetForegroundWindow(g_mainWnd);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_mainWnd, nullptr);
    DestroyMenu(m);
    if (cmd == 9001) {
        if (IsWindowVisible(g_mainWnd)) ShowWindow(g_mainWnd, SW_HIDE);
        else { ShowWindow(g_mainWnd, SW_SHOW); SetForegroundWindow(g_mainWnd); }
    } else if (cmd == 9002) {
        ToggleClicking();
    } else if (cmd == 9003) {
        DestroyWindow(g_mainWnd);
    }
}

// ---------------------------------------------------------------- hotkey dialog
static LRESULT CALLBACK HotkeyModalProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hotkeyDlg = hwnd;
            g_capturing = true;
            g_captureHeld.clear();
            g_captureHeldMods = 0;
            g_captureResult = Combo();
            g_captureValid = false;

            HWND l1 = CreateWindowExW(0, L"STATIC", L"Press the key combination you want to use.",
                WS_VISIBLE | WS_CHILD, 16, 16, 388, 24, hwnd, nullptr, nullptr, nullptr);
            SendMessage(l1, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND l2 = CreateWindowExW(0, L"STATIC", L"(press keys)",
                WS_VISIBLE | WS_CHILD, 16, 46, 388, 48, hwnd, (HMENU)302, nullptr, nullptr);
            SendMessage(l2, WM_SETFONT, (WPARAM)g_bold, TRUE);
            HWND l3 = CreateWindowExW(0, L"STATIC", L"Use modifiers + a key, or hold several keys (X + Z).\nRelease the keys, then click Save. Esc cancels.",
                WS_VISIBLE | WS_CHILD, 16, 100, 388, 44, hwnd, nullptr, nullptr, nullptr);
            SendMessage(l3, WM_SETFONT, (WPARAM)g_font, TRUE);

            HWND b1 = CreateWindowExW(0, L"BUTTON", L"Save", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP,
                180, 156, 105, 32, hwnd, (HMENU)IDOK, nullptr, nullptr);
            SendMessage(b1, WM_SETFONT, (WPARAM)g_bold, TRUE);
            HWND b2 = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                297, 156, 107, 32, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
            SendMessage(b2, WM_SETFONT, (WPARAM)g_font, TRUE);
            SetFocus(hwnd);
            ScaleUi(hwnd, WindowDpi(hwnd));
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                if (g_captureValid) {
                    Combo c = g_captureResult;
                    if (!c.keys.empty()) {
                        g_cfg.hotkey = c;
                        ApplyUiToConfig();
                        if (g_engine) g_engine->ApplyConfig(g_cfg);
                        std::wstring hk = c.Name();
                        SetText(GetDlgItem(g_mainWnd, ID_HOTKEY_LABEL), L"Hotkey: " + hk);
                        UpdateButton();
                    }
                }
                g_capturing = false;
                g_hotkeyDlgOpen = false;
                EnableWindow(g_mainWnd, TRUE);
                SetForegroundWindow(g_mainWnd);
                DestroyWindow(hwnd);
            } else if (id == IDCANCEL) {
                g_capturing = false;
                g_hotkeyDlgOpen = false;
                EnableWindow(g_mainWnd, TRUE);
                SetForegroundWindow(g_mainWnd);
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_CLOSE:
            g_capturing = false;
            g_hotkeyDlgOpen = false;
            EnableWindow(g_mainWnd, TRUE);
            SetForegroundWindow(g_mainWnd);
            DestroyWindow(hwnd);
            break;
        case WM_CTLCOLORSTATIC: {
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_DPICHANGED:
            ScaleUi(hwnd, HIWORD(wParam));
            SizeClient(hwnd, 420, 204);
            break;
        case WM_DESTROY: FreeUiFonts(hwnd); break;
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------- points dialog
static unsigned long long g_recLastUs = 0;

static void RefreshSeqList(HWND hList) {
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    wchar_t buf[128];
    int extent = 0;
    HDC dc = GetDC(hList);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessage(hList, WM_GETFONT, 0, 0));
    for (int i = 0; i < (int)g_cfg.seq.size(); i++) {
        const SeqPoint& p = g_cfg.seq[i];
        if (p.delayUs >= 0)
            swprintf_s(buf, 128, L"#%d:  (X: %d,  Y: %d)   +%.0f ms", i + 1, p.x, p.y, (double)p.delayUs / 1000.0);
        else
            swprintf_s(buf, 128, L"#%d:  (X: %d,  Y: %d)", i + 1, p.x, p.y);
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)buf);
        SIZE size;
        GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &size);
        extent = std::max(extent, (int)size.cx + 16);
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hList, dc);
    SendMessage(hList, LB_SETHORIZONTALEXTENT, extent, 0);
    SendMessageW(GetDlgItem(g_mainWnd, ID_SEQ_BTN), WM_SETTEXT, 0, (LPARAM)Fmt(L"Manage Points (%d)...", (int)g_cfg.seq.size()).c_str());
}

static LRESULT CALLBACK SeqModalProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_seqDlg = hwnd;
            g_seqDlgOpen = true;
            HWND l0 = CreateWindowExW(0, L"STATIC", L"Click points (recorded or manual):",
                WS_VISIBLE | WS_CHILD, 16, 16, 388, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessage(l0, WM_SETFONT, (WPARAM)g_bold, TRUE);

            HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                WS_VISIBLE | WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, 16, 44, 388, 148, hwnd, (HMENU)ID_SEQ_LIST, nullptr, nullptr);
            SendMessage(hList, WM_SETFONT, (WPARAM)g_font, TRUE);

            HWND bRec = CreateWindowExW(0, L"BUTTON", L"Record clicks...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                16, 204, 202, 32, hwnd, (HMENU)ID_SEQ_REC_BTN, nullptr, nullptr);
            SendMessage(bRec, WM_SETFONT, (WPARAM)g_bold, TRUE);
            HWND bDel = CreateWindowExW(0, L"BUTTON", L"Delete Selected", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                230, 204, 174, 32, hwnd, (HMENU)ID_SEQ_DEL_BTN, nullptr, nullptr);
            SendMessage(bDel, WM_SETFONT, (WPARAM)g_font, TRUE);

            HWND lx = CreateWindowExW(0, L"STATIC", L"X:", WS_VISIBLE | WS_CHILD, 16, 252, 18, 24, hwnd, nullptr, nullptr, nullptr);
            SendMessage(lx, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND hX = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP,
                36, 248, 60, 26, hwnd, (HMENU)ID_SEQ_IN_X, nullptr, nullptr);
            SendMessage(hX, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND ly = CreateWindowExW(0, L"STATIC", L"Y:", WS_VISIBLE | WS_CHILD, 108, 252, 18, 24, hwnd, nullptr, nullptr, nullptr);
            SendMessage(ly, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND hY = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP,
                128, 248, 60, 26, hwnd, (HMENU)ID_SEQ_IN_Y, nullptr, nullptr);
            SendMessage(hY, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND ld = CreateWindowExW(0, L"STATIC", L"Delay ms:", WS_VISIBLE | WS_CHILD, 200, 252, 68, 24, hwnd, nullptr, nullptr, nullptr);
            SendMessage(ld, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND hD = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_CENTER | WS_TABSTOP,
                270, 248, 60, 26, hwnd, (HMENU)ID_SEQ_IN_D, nullptr, nullptr);
            SendMessage(hD, WM_SETFONT, (WPARAM)g_font, TRUE);

            HWND bAdd = CreateWindowExW(0, L"BUTTON", L"Add", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                342, 248, 62, 28, hwnd, (HMENU)ID_SEQ_ADD_BTN, nullptr, nullptr);
            SendMessage(bAdd, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND bClr = CreateWindowExW(0, L"BUTTON", L"Clear All", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                16, 290, 108, 32, hwnd, (HMENU)ID_SEQ_CLR_BTN, nullptr, nullptr);
            SendMessage(bClr, WM_SETFONT, (WPARAM)g_font, TRUE);
            HWND bDone = CreateWindowExW(0, L"BUTTON", L"Done", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP,
                284, 290, 120, 32, hwnd, (HMENU)IDOK, nullptr, nullptr);
            SendMessage(bDone, WM_SETFONT, (WPARAM)g_bold, TRUE);

            RefreshSeqList(hList);
            ScaleUi(hwnd, WindowDpi(hwnd));
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            HWND hList = GetDlgItem(hwnd, ID_SEQ_LIST);
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);

            if (id == ID_SEQ_ADD_BTN) {
                SeqPoint p;
                p.x = (int)GetDlgItemInt(hwnd, ID_SEQ_IN_X, nullptr, TRUE);
                p.y = (int)GetDlgItemInt(hwnd, ID_SEQ_IN_Y, nullptr, TRUE);
                wchar_t dbuf[32] = {0};
                GetDlgItemTextW(hwnd, ID_SEQ_IN_D, dbuf, 32);
                p.delayUs = dbuf[0] ? (long long)wcstod(dbuf, nullptr) * 1000.0 : -1;
                g_cfg.seq.push_back(p);
                RefreshSeqList(hList);
            } else if (id == ID_SEQ_DEL_BTN) {
                if (sel != LB_ERR && sel < (int)g_cfg.seq.size()) {
                    g_cfg.seq.erase(g_cfg.seq.begin() + sel);
                    RefreshSeqList(hList);
                }
            } else if (id == ID_SEQ_CLR_BTN) {
                g_cfg.seq.clear();
                g_recLastUs = 0;
                RefreshSeqList(hList);
            } else if (id == ID_SEQ_REC_BTN) {
                if (g_pickMode == 2) {
                    g_pickMode = 0;
                    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                    SetDlgItemTextW(hwnd, ID_SEQ_REC_BTN, L"Record clicks...");
                } else {
                    g_pickMode = 2;
                    g_recLastUs = 0;
                    g_cfg.seq.clear();
                    RefreshSeqList(hList);
                    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
                    SetDlgItemTextW(hwnd, ID_SEQ_REC_BTN, L"Stop recording (ESC)");
                }
            } else if (id == IDOK || id == IDCANCEL) {
                if (g_pickMode == 2) {
                    g_pickMode = 0;
                    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                }
                g_seqDlgOpen = false;
                EnableWindow(g_mainWnd, TRUE);
                SetForegroundWindow(g_mainWnd);
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_APP_REC: {
            // a click was captured while recording
            unsigned long long now = g_clock ? g_clock->NowUs() : 0;
            SeqPoint p;
            p.x = (int)wParam; p.y = (int)lParam;
            p.delayUs = g_recLastUs ? (long long)(now - g_recLastUs) : 0;
            g_recLastUs = now;
            g_cfg.seq.push_back(p);
            RefreshSeqList(GetDlgItem(hwnd, ID_SEQ_LIST));
            break;
        }
        case WM_CLOSE:
            if (g_pickMode == 2) {
                g_pickMode = 0;
                if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
            }
            g_seqDlgOpen = false;
            EnableWindow(g_mainWnd, TRUE);
            SetForegroundWindow(g_mainWnd);
            DestroyWindow(hwnd);
            break;
        case WM_CTLCOLORSTATIC: {
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_DPICHANGED:
            ScaleUi(hwnd, HIWORD(wParam));
            SizeClient(hwnd, 420, 338);
            RefreshSeqList(GetDlgItem(hwnd, ID_SEQ_LIST));
            break;
        case WM_DESTROY: FreeUiFonts(hwnd); break;
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------- enter key in the interval boxes
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (uMsg) {
        case WM_GETDLGCODE:
            if (wParam == VK_RETURN) return DLGC_WANTALLKEYS;
            break;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) { ConvertAndSaveInterval(); return 0; }
            break;
        case WM_CHAR:
            if (wParam == VK_RETURN) return 0;
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, EditSubclassProc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------- main window
static void LayoutMain(HWND hwnd) {
    ScrollMain(hwnd, 0);
    HWND tabs = GetDlgItem(hwnd, ID_UI_TABS);
    ShowWindow(tabs, g_advanced ? SW_SHOW : SW_HIDE);
    int page = TabCtrl_GetCurSel(tabs);
    const int originalTop[] = {8, 90, 144, 205, 266, 320, 431, 485, 539, 596};
    const int basicTop[] = {52, -1, 164, -1, 260, -1, -1, 340, -1, 420};
    const int clickingTop[] = {92, -1, 204, -1, 300, -1, -1, 380, -1, 460};
    const int automationTop[] = {-1, 92, -1, 172, -1, 252, 388, -1, -1, 460};
    const int profilesTop[] = {-1, -1, -1, -1, -1, -1, -1, -1, 92, 460};
    const int* top = !g_advanced ? basicTop : page == 1 ? automationTop : page == 2 ? profilesTop : clickingTop;
    for (const auto& c : g_mainControls) {
        int id = GetDlgCtrlID(c.hwnd);
        bool visible = top[c.section] >= 0;
        bool group = c.y == originalTop[c.section] && c.section != UI_FOOTER;
        int x = c.x, y = top[c.section] + c.y - originalTop[c.section], w = c.w, h = c.h;
        if (group) {
            x = 16; w = 468;
            const int heights[] = {96, 64, 80, 64, 64, 120, 64, 64, 104};
            h = heights[c.section];
        } else {
            x = std::max(28, x);
            y += 8;
        }
        if (c.section == UI_INTERVAL && !group) {
            const int fields[] = {ID_HRS, ID_MINS, ID_SECS, ID_MS, ID_US};
            for (int i = 0; i < 5; ++i) {
                if (id == fields[i]) { x = 28 + i * 90; y = top[UI_INTERVAL] + 42; w = 78; h = 26; }
            }
            if (!id) { x = 28 + ((c.x - 18 + 35) / 69) * 90; y = top[UI_INTERVAL] + 22; w = 78; }
            if (id == ID_WARN_STATIC) { x = 28; y = top[UI_INTERVAL] + 72; w = 438; h = 20; }
        }
        if (c.section == UI_CLICK && !group) {
            int column = c.x < 190 ? 0 : c.x < 320 ? 1 : 2;
            bool combo = id == ID_BTN_COMBO || id == ID_TYPE_COMBO || id == ID_MODE_COMBO;
            visible = visible && (g_advanced || column != 2);
            int columnWidth = g_advanced ? 150 : 225;
            x = 28 + column * columnWidth;
            y = top[UI_CLICK] + (combo ? 42 : 22);
            w = columnWidth - 16;
        }
        if (c.section == UI_PROFILES && !group) {
            if (id == ID_PROF_COMBO) { x = 28; y = top[UI_PROFILES] + 24; w = 438; }
            else { x = 28 + (id - ID_PROF_SAVE) * 150; y = top[UI_PROFILES] + 60; w = 138; }
        }
        if (id == ID_TOP_CHECK) { x = 320; y = top[UI_FOOTER] + 8; w = 158; }
        if (id == ID_SAFETY_STATUS) { x = 22; y = top[UI_FOOTER] + 24; w = 456; h = 18; }
        if (id == ID_MAIN_BTN) { x = 16; y = top[UI_FOOTER] + 42; w = 468; h = 48; }
        SetWindowPos(c.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(c.hwnd, visible ? SW_SHOW : SW_HIDE);
    }
    SizeClient(hwnd, 500, MainContentHeight());
    UpdateScroll(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void BuildUi(HWND hwnd) {
    int section = -1;
    auto Add = [&](LPCWSTR cls, LPCWSTR txt, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
        if (wcscmp(cls, L"BUTTON") == 0 && (style & 0xf) == BS_GROUPBOX) {
            ++section;
            w += 60;
            style |= WS_GROUP;
        }
        x = 10 + MulDiv(x - 10, 480, 540);
        w = MulDiv(w, 480, 540);
        if (wcscmp(cls, L"BUTTON") == 0) style |= BS_NOTIFY;
        if (wcscmp(cls, L"EDIT") == 0) style |= ES_AUTOHSCROLL;
        HWND c = CreateWindowExW(0, cls, txt, WS_VISIBLE | WS_CHILD | style, x, y, w, h, hwnd,
                                 (HMENU)(UINT_PTR)id, nullptr, nullptr);
        g_mainControls.push_back({c, x, y, w, h, section});
        SendMessage(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
        return c;
    };

    // --- click interval
    Add(L"BUTTON", L"Click Interval", BS_GROUPBOX, 10, 8, 480, 76, 0);
    Add(L"STATIC", L"Hrs", 0, 20, 30, 26, 20, 0);
    HWND hHrs = Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 48, 27, 38, 22, ID_HRS);
    Add(L"STATIC", L"Mins", 0, 94, 30, 30, 20, 0);
    HWND hMins = Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 126, 27, 38, 22, ID_MINS);
    Add(L"STATIC", L"Secs", 0, 172, 30, 30, 20, 0);
    HWND hSecs = Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 204, 27, 38, 22, ID_SECS);
    Add(L"STATIC", L"Ms", 0, 250, 30, 22, 20, 0);
    HWND hMs = Add(L"EDIT", L"100", WS_BORDER | WS_TABSTOP | ES_CENTER, 274, 27, 44, 22, ID_MS);
    Add(L"STATIC", L"\xB5s", 0, 326, 30, 20, 20, 0);
    HWND hUs = Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 348, 27, 50, 22, ID_US);
    SetWindowSubclass(hHrs, EditSubclassProc, 1, (DWORD_PTR)hwnd);
    SetWindowSubclass(hMins, EditSubclassProc, 1, (DWORD_PTR)hwnd);
    SetWindowSubclass(hSecs, EditSubclassProc, 1, (DWORD_PTR)hwnd);
    SetWindowSubclass(hMs, EditSubclassProc, 1, (DWORD_PTR)hwnd);
    SetWindowSubclass(hUs, EditSubclassProc, 1, (DWORD_PTR)hwnd);
    Add(L"STATIC", L"", 0, 22, 50, 516, 30, ID_WARN_STATIC);

    // --- randomisation
    Add(L"BUTTON", L"Randomisation", BS_GROUPBOX, 10, 90, 480, 48, 0);
    HWND hRand = Add(L"BUTTON", L"Random interval", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 108, 140, 20, ID_RAND_CHECK);
    (void)hRand;
    Add(L"STATIC", L"Min (ms)", 0, 170, 108, 58, 20, 0);
    Add(L"EDIT", L"50", WS_BORDER | WS_TABSTOP | ES_CENTER, 230, 105, 60, 23, ID_RAND_MIN);
    Add(L"STATIC", L"Max (ms)", 0, 298, 108, 58, 20, 0);
    Add(L"EDIT", L"150", WS_BORDER | WS_TABSTOP | ES_CENTER, 358, 105, 60, 23, ID_RAND_MAX);

    // --- click options
    Add(L"BUTTON", L"Click Options", BS_GROUPBOX, 10, 144, 480, 55, 0);
    Add(L"STATIC", L"Button:", 0, 22, 162, 95, 20, 0);
    HWND hBtnCombo = Add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 118, 158, 95, 120, ID_BTN_COMBO);
    Add(L"STATIC", L"Type:", 0, 222, 162, 40, 20, 0);
    HWND hTypeCombo = Add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 264, 158, 90, 120, ID_TYPE_COMBO);
    Add(L"STATIC", L"Run:", 0, 362, 162, 42, 20, 0);
    HWND hModeCombo = Add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 406, 158, 132, 120, ID_MODE_COMBO);
    SendMessageW(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)L"Left");
    SendMessageW(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)L"Right");
    SendMessageW(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)L"Middle");
    SendMessageW(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Single");
    SendMessageW(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Double");
    SendMessageW(hModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Repeat");
    SendMessageW(hModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Hold");
    SendMessageW(hModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Once");
    SendMessage(hBtnCombo, CB_SETCURSEL, 0, 0);
    SendMessage(hTypeCombo, CB_SETCURSEL, 0, 0);
    SendMessage(hModeCombo, CB_SETCURSEL, 0, 0);

    // --- action
    Add(L"BUTTON", L"Action", BS_GROUPBOX, 10, 205, 480, 55, 0);
    Add(L"STATIC", L"Action:", 0, 22, 223, 52, 20, 0);
    HWND hActCombo = Add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 76, 219, 145, 120, ID_ACTION_COMBO);
    SendMessageW(hActCombo, CB_ADDSTRING, 0, (LPARAM)L"Click");
    SendMessageW(hActCombo, CB_ADDSTRING, 0, (LPARAM)L"Scroll Up");
    SendMessageW(hActCombo, CB_ADDSTRING, 0, (LPARAM)L"Scroll Down");
    SendMessageW(hActCombo, CB_ADDSTRING, 0, (LPARAM)L"Type Text");
    SendMessage(hActCombo, CB_SETCURSEL, 0, 0);
    Add(L"STATIC", L"Text:", 0, 228, 223, 40, 20, 0);
    Add(L"EDIT", L"Hello", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 270, 219, 268, 23, ID_TEXT_EDIT);

    // --- repeat
    Add(L"BUTTON", L"When to Stop", BS_GROUPBOX, 10, 266, 480, 48, 0);
    Add(L"BUTTON", L"Repeat until stopped", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 284, 180, 20, ID_REP_INF);
    Add(L"BUTTON", L"Repeat", BS_AUTORADIOBUTTON, 224, 284, 78, 20, ID_REP_TIMES);
    Add(L"EDIT", L"10", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 310, 282, 72, 23, ID_REP_COUNT);
    Add(L"STATIC", L"times", 0, 392, 284, 50, 20, 0);

    // --- cursor position
    Add(L"BUTTON", L"Cursor Position", BS_GROUPBOX, 10, 320, 480, 105, 0);
    Add(L"BUTTON", L"Current Location", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 338, 160, 20, ID_POS_CUR);
    Add(L"BUTTON", L"Specific Location", BS_AUTORADIOBUTTON, 22, 364, 160, 20, ID_POS_CUST);
    Add(L"STATIC", L"X:", 0, 190, 364, 18, 20, 0);
    Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 210, 362, 64, 23, ID_POS_X);
    Add(L"STATIC", L"Y:", 0, 284, 364, 18, 20, 0);
    Add(L"EDIT", L"0", WS_BORDER | WS_TABSTOP | ES_CENTER, 304, 362, 64, 23, ID_POS_Y);
    Add(L"BUTTON", L"Pick Location", BS_PUSHBUTTON | WS_TABSTOP, 380, 360, 158, 28, ID_PICK_BTN);
    Add(L"BUTTON", L"Consecutive Points", BS_AUTORADIOBUTTON, 22, 392, 174, 20, ID_POS_SEQ);
    Add(L"BUTTON", L"Manage Points (0)...", BS_PUSHBUTTON | WS_TABSTOP, 210, 390, 328, 28, ID_SEQ_BTN);

    // --- timing
    Add(L"BUTTON", L"Timing", BS_GROUPBOX, 10, 431, 480, 48, 0);
    Add(L"STATIC", L"Start delay:", 0, 22, 449, 78, 20, 0);
    Add(L"EDIT", L"3", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 102, 446, 45, 23, ID_DELAY_EDIT);
    Add(L"STATIC", L"sec", 0, 152, 449, 30, 20, 0);
    Add(L"STATIC", L"Hold:", 0, 200, 449, 38, 20, 0);
    Add(L"EDIT", L"10", WS_BORDER | WS_TABSTOP | ES_CENTER, 240, 446, 50, 23, ID_HOLD_EDIT);
    Add(L"STATIC", L"ms", 0, 294, 449, 26, 20, 0);
    Add(L"STATIC", L"(button down time)", 0, 326, 449, 212, 20, 0);

    // --- hotkey
    Add(L"BUTTON", L"Start / Stop Shortcut", BS_GROUPBOX, 10, 485, 480, 48, 0);
    Add(L"STATIC", L"Hotkey: F6", SS_ENDELLIPSIS, 22, 503, 336, 20, ID_HOTKEY_LABEL, g_bold);
    Add(L"BUTTON", L"Change Hotkey...", BS_PUSHBUTTON | WS_TABSTOP, 370, 499, 168, 28, ID_CHANGE_HOTKEY_BTN);

    // --- profiles
    Add(L"BUTTON", L"Profiles", BS_GROUPBOX, 10, 539, 480, 48, 0);
    HWND hProfCombo = Add(L"COMBOBOX", nullptr, CBS_DROPDOWN | WS_TABSTOP | ES_AUTOHSCROLL, 22, 555, 242, 120, ID_PROF_COMBO);
    (void)hProfCombo;
    Add(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, 276, 555, 76, 28, ID_PROF_SAVE);
    Add(L"BUTTON", L"Load", BS_PUSHBUTTON | WS_TABSTOP, 364, 555, 76, 28, ID_PROF_LOAD);
    Add(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 452, 555, 86, 28, ID_PROF_DEL);

    // --- status
    section = UI_FOOTER;
    Add(L"STATIC", L"Clicks: 0", 0, 22, 596, 170, 20, ID_STAT_CLICKS);
    Add(L"STATIC", L"Emergency stop: move the cursor to any screen edge.", 0, 22, 612, 456, 18, ID_SAFETY_STATUS);
    Add(L"BUTTON", L"Always on Top", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 620, 180, 20, ID_TOP_CHECK);
    Add(L"BUTTON", L"START CLICKING (F6)", BS_PUSHBUTTON | WS_TABSTOP, 10, 650, 540, 36, ID_MAIN_BTN, g_bold);

    SendMessage(GetDlgItem(hwnd, ID_REP_INF), BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(GetDlgItem(hwnd, ID_POS_CUR), BM_SETCHECK, BST_CHECKED, 0);
    size_t contentControls = g_mainControls.size();
    Add(L"STATIC", L"View:", 0, 22, 8, 76, 20, 0);
    HWND mode = Add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 102, 5, 180, 100, ID_UI_MODE);
    SendMessageW(mode, CB_ADDSTRING, 0, (LPARAM)L"Basic");
    SendMessageW(mode, CB_ADDSTRING, 0, (LPARAM)L"Advanced");
    SendMessage(mode, CB_SETCURSEL, 0, 0);
    // Keep the view selector first in keyboard navigation, before the click controls.
    SetWindowPos(mode, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    HWND tabs = Add(WC_TABCONTROLW, L"", WS_TABSTOP, 16, 42, 522, 30, ID_UI_TABS);
    for (const wchar_t* caption : {L"Clicking", L"Automation", L"Profiles"}) {
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(caption);
        TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item);
    }
    g_mainControls.resize(contentControls);
}

static void RefreshProfileCombo() {
    HWND c = GetDlgItem(g_mainWnd, ID_PROF_COMBO);
    SendMessage(c, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_profiles.size(); i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_profiles[i].c_str());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NOTIFY: {
            NMHDR* notification = (NMHDR*)lParam;
            if (notification->idFrom == ID_UI_TABS && notification->code == TCN_SELCHANGE) {
                LayoutMain(hwnd);
                return 0;
            }
            break;
        }
        case WM_CREATE: {
            g_mainWnd = hwnd;
            BuildUi(hwnd);
            ScaleUi(hwnd, 96);
            LayoutMain(hwnd);
            ApplyConfigToUi();
            UpdateEnabledStates();
            UpdateWarning();
            UpdateButton();
            RefreshProfileCombo();
            TrayAdd();
            SetTimer(hwnd, 1, 100, nullptr);
            g_emergencyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, EmergencyMouseHookProc, GetModuleHandleW(nullptr), 0);
            UpdateStats();
            break;
        }
        case WM_TIMER: {
            UpdateButton();
            UpdateStats();
            break;
        }
        case WM_APP_EMERGENCY: {
            if (!g_engine->Status().emergencyStopped) break;
            ShowEmergencyEffect({(LONG)wParam, (LONG)lParam});
            TimerResolutionOff();
            UpdateButton(); UpdateStats();
            break;
        }
        case WM_APP_STATE: {
            // ESC: cancel a pick or a hotkey capture
            if (g_pickMode == 1) {
                g_pickMode = 0;
                if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                ShowWindow(g_mainWnd, SW_SHOW);
                SetForegroundWindow(g_mainWnd);
            } else if (g_pickMode == 2) {
                g_pickMode = 0;
                if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                if (g_seqDlg) SetDlgItemTextW(g_seqDlg, ID_SEQ_REC_BTN, L"Record clicks...");
            } else if (g_capturing) {
                g_capturing = false;
                if (g_hotkeyDlg) DestroyWindow(g_hotkeyDlg);
            }
            break;
        }
        case WM_APP_PICK: {
            if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
            ShowWindow(g_mainWnd, SW_SHOW);
            SetForegroundWindow(g_mainWnd);
            SetInt(hwnd, ID_POS_X, (int)wParam);
            SetInt(hwnd, ID_POS_Y, (int)lParam);
            SetChecked(hwnd, ID_POS_CUST, true);
            SetChecked(hwnd, ID_POS_CUR, false);
            SetChecked(hwnd, ID_POS_SEQ, false);
            g_cfg.posMode = POS_FIXED;
            g_cfg.targetX = (int)wParam;
            g_cfg.targetY = (int)lParam;
            UpdateEnabledStates();
            break;
        }
        case WM_APP_TRAY: {
            if (lParam == WM_RBUTTONUP) ShowTrayMenu();
            else if (lParam == WM_LBUTTONDBLCLK) { ShowWindow(g_mainWnd, SW_SHOW); SetForegroundWindow(g_mainWnd); }
            break;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);
            else UpdateScroll(hwnd);
            break;
        case WM_VSCROLL: {
            SCROLLINFO info = {sizeof(info), SIF_ALL};
            GetScrollInfo(hwnd, SB_VERT, &info);
            int pos = info.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: pos -= MulDiv(32, WindowDpi(hwnd), 96); break;
                case SB_LINEDOWN: pos += MulDiv(32, WindowDpi(hwnd), 96); break;
                case SB_PAGEUP: pos -= info.nPage; break;
                case SB_PAGEDOWN: pos += info.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = info.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = info.nMax; break;
            }
            ScrollMain(hwnd, pos);
            break;
        }
        case WM_MOUSEWHEEL: {
            static int wheel = 0;
            wheel += GET_WHEEL_DELTA_WPARAM(wParam);
            SCROLLINFO info = {sizeof(info), SIF_POS};
            GetScrollInfo(hwnd, SB_VERT, &info);
            ScrollMain(hwnd, info.nPos - (wheel / WHEEL_DELTA) * MulDiv(96, WindowDpi(hwnd), 96));
            wheel %= WHEEL_DELTA;
            break;
        }
        case WM_DPICHANGED: {
            ScrollMain(hwnd, 0);
            ScaleUi(hwnd, 96);
            RECT* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            SizeClient(hwnd, 500, MainContentHeight());
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == CBN_SETFOCUS ||
                HIWORD(wParam) == BN_SETFOCUS) {
                HWND focus = GetFocus();
                if (focus && IsChild(hwnd, focus)) {
                    RECT r, client;
                    GetWindowRect(focus, &r);
                    MapWindowPoints(nullptr, hwnd, (POINT*)&r, 2);
                    GetClientRect(hwnd, &client);
                    SCROLLINFO info = {sizeof(info), SIF_POS};
                    GetScrollInfo(hwnd, SB_VERT, &info);
                    if (r.top < 0) ScrollMain(hwnd, info.nPos + r.top);
                    else if (r.bottom > client.bottom) ScrollMain(hwnd, info.nPos + r.bottom - client.bottom);
                }
            }
            if (lParam && HIWORD(wParam) != BN_CLICKED) {
                wchar_t cls[32];
                GetClassNameW((HWND)lParam, cls, 32);
                if (wcscmp(cls, L"Button") == 0) break;
            }
            switch (id) {
                case ID_UI_MODE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_advanced = SendDlgItemMessage(hwnd, ID_UI_MODE, CB_GETCURSEL, 0, 0) == 1;
                        LayoutMain(hwnd);
                    }
                    break;
                case ID_MAIN_BTN: ToggleClicking(); break;
                case ID_PICK_BTN: {
                    g_pickMode = 1;
                    ShowWindow(hwnd, SW_HIDE);
                    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
                    break;
                }
                case ID_SEQ_BTN: {
                    if (g_engine && g_engine->IsRunning()) { g_engine->Stop(); SetEvent(hInterrupt); }
                    EnableWindow(hwnd, FALSE);
                    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"SeqModalClass", L"Manage Consecutive Clicks",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 0, 0, 340, 275, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                    SizeClient(dlg, 420, 338);
                    break;
                }
                case ID_CHANGE_HOTKEY_BTN: {
                    if (g_engine && g_engine->IsRunning()) { g_engine->Stop(); SetEvent(hInterrupt); }
                    EnableWindow(hwnd, FALSE);
                    g_hotkeyDlgOpen = true;
                    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"HotkeyModalClass", L"Change Hotkey",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 0, 0, 320, 140, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                    SizeClient(dlg, 420, 204);
                    break;
                }
                case ID_TOP_CHECK: {
                    bool on = IsChecked(hwnd, ID_TOP_CHECK);
                    g_cfg.alwaysOnTop = on;
                    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    break;
                }
                case ID_ACTION_COMBO: UpdateEnabledStates(); break;
                case ID_REP_INF: case ID_REP_TIMES:
                case ID_POS_CUR: case ID_POS_CUST: case ID_POS_SEQ:
                    UpdateEnabledStates(); break;
                case ID_RAND_CHECK: UpdateEnabledStates(); ApplyUiToConfig(); UpdateWarning(); break;
                case ID_RAND_MIN: case ID_RAND_MAX:
                case ID_HRS: case ID_MINS: case ID_SECS: case ID_MS: case ID_US:
                    ApplyUiToConfig();
                    UpdateWarning();
                    break;
                case ID_PROF_SAVE: {
                    wchar_t name[128] = {0};
                    GetDlgItemTextW(hwnd, ID_PROF_COMBO, name, 128);
                    std::wstring n = name;
                    if (n.empty()) { MessageBoxW(hwnd, L"Type a name for the profile first.", L"Profiles", MB_ICONINFORMATION); break; }
                    ApplyUiToConfig();
                    g_profileData[n] = EncodeConfig(g_cfg);
                    bool exists = false;
                    for (size_t i = 0; i < g_profiles.size(); i++) if (g_profiles[i] == n) exists = true;
                    if (!exists) g_profiles.push_back(n);
                    RefreshProfileCombo();
                    SendMessageW(GetDlgItem(hwnd, ID_PROF_COMBO), CB_SELECTSTRING, (WPARAM)-1, (LPARAM)n.c_str());
                    SaveIni();
                    break;
                }
                case ID_PROF_LOAD: {
                    int sel = (int)SendMessage(GetDlgItem(hwnd, ID_PROF_COMBO), CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < (int)g_profiles.size()) {
                        auto it = g_profileData.find(g_profiles[sel]);
                        if (it != g_profileData.end()) {
                            Config c;
                            if (DecodeConfig(it->second, c)) {
                                g_cfg = c;
                                ApplyConfigToUi();
                                UpdateEnabledStates();
                                UpdateWarning();
                                UpdateButton();
                                if (g_engine) g_engine->ApplyConfig(g_cfg);
                            }
                        }
                    }
                    break;
                }
                case ID_PROF_DEL: {
                    int sel = (int)SendMessage(GetDlgItem(hwnd, ID_PROF_COMBO), CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < (int)g_profiles.size()) {
                        g_profileData.erase(g_profiles[sel]);
                        g_profiles.erase(g_profiles.begin() + sel);
                        RefreshProfileCombo();
                        SaveIni();
                    }
                    break;
                }
                default: break;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            if ((HWND)lParam == GetDlgItem(hwnd, ID_SAFETY_STATUS)) {
                HIGHCONTRASTW contrast = {sizeof(contrast)};
                SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
                SetTextColor((HDC)wParam, g_engine->Status().emergencyStopped && !(contrast.dwFlags & HCF_HIGHCONTRASTON) ?
                    RGB(170, 25, 50) : GetSysColor(COLOR_BTNTEXT));
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            if ((HWND)lParam == GetDlgItem(hwnd, ID_WARN_STATIC)) {
                HIGHCONTRASTW contrast = {sizeof(contrast)};
                SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
                bool fast = g_cfg.randomize ? g_cfg.randMinUs < 5000 : g_cfg.intervalUs < 5000;
                SetTextColor((HDC)wParam, fast && !(contrast.dwFlags & HCF_HIGHCONTRASTON) ?
                    RGB(160, 45, 20) : GetSysColor(COLOR_BTNTEXT));
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY: {
            FreeUiFonts(hwnd);
            g_running = false;
            if (g_engine && g_engine->IsRunning()) { g_engine->Stop(); SetEvent(hInterrupt); }
            KillTimer(hwnd, 1);
            if (g_emergencyOverlay) { DestroyWindow(g_emergencyOverlay); g_emergencyOverlay = nullptr; }
            if (g_emergencyMouseHook) { UnhookWindowsHookEx(g_emergencyMouseHook); g_emergencyMouseHook = nullptr; }
            TrayRemove();
            if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
            if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
            SaveIni();
            PostQuitMessage(0);
            break;
        }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------- self test
// Drives the real window with the real engine, so the GUI wiring is covered too.
static int g_selfChecks = 0, g_selfFails = 0;

static void SelfTestLog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) { fputs(buf, stdout); fflush(stdout); }
}

// Keeps the window alive while the test waits: the timer that updates the
// button text and the stats only runs while messages are pumped.
static void PumpMessages(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}
#define ST_CHECK(cond, name) do { g_selfChecks++; if (!(cond)) { g_selfFails++; \
    SelfTestLog("  FAIL  %s\n", name); } } while (0)

static std::wstring CtlText(HWND h, int id) {
    wchar_t buf[512] = {0};
    GetDlgItemTextW(h, id, buf, 512);
    return buf;
}

static void CheckControlBounds(HWND parent) {
    RECT client;
    GetClientRect(parent, &client);
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, parent, (POINT*)&r, 2);
        ST_CHECK(r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom,
                 "control fits inside its client area");
    }
}

static void CheckCaptionFits(HWND parent, int id) {
    HWND child = GetDlgItem(parent, id);
    RECT r;
    GetClientRect(child, &r);
    HDC dc = GetDC(child);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessage(child, WM_GETFONT, 0, 0));
    std::wstring text = CtlText(parent, id);
    SIZE size;
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
    ST_CHECK(size.cx + 12 <= r.right && size.cy <= r.bottom, "button caption fits without clipping");
    SelectObject(dc, old);
    ReleaseDC(child, dc);
}

static void CheckDialogScaling(HWND dialog, int width, int height) {
    UINT original = WindowDpi(dialog);
    for (UINT dpi : {96u, 144u, 192u}) {
        ScaleUi(dialog, dpi);
        SetWindowPos(dialog, nullptr, 0, 0, MulDiv(width, dpi, 96) + 40,
            MulDiv(height, dpi, 96) + 80, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CheckControlBounds(dialog);
        LOGFONTW font;
        GetObjectW((HFONT)SendMessage(GetDlgItem(dialog, IDOK), WM_GETFONT, 0, 0), sizeof(font), &font);
        ST_CHECK(font.lfHeight == MulDiv(16, dpi, 96), "dialog font scales with DPI");
        CheckCaptionFits(dialog, IDOK);
    }
    ScaleUi(dialog, original);
}

static int RunSelfTest(HWND hwnd) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONIN$", "r", stdin);
    }
    SelfTestLog("Ultimate AutoClicker - GUI self test\n");
    SelfTestLog("===============================\n");

    SelfTestLog("- interface modes\n");
    ST_CHECK(!g_advanced && SendDlgItemMessage(hwnd, ID_UI_MODE, CB_GETCURSEL, 0, 0) == 0,
             "each launch defaults to Basic");
    ST_CHECK(!IsWindowVisible(GetDlgItem(hwnd, ID_RAND_CHECK)) &&
             !IsWindowVisible(GetDlgItem(hwnd, ID_PROF_COMBO)), "Basic hides advanced controls");
    ST_CHECK(IsWindowVisible(GetDlgItem(hwnd, ID_MAIN_BTN)) &&
             IsWindowVisible(GetDlgItem(hwnd, ID_MS)), "Basic keeps essential controls");
    CheckCaptionFits(hwnd, ID_PICK_BTN);
    std::wstring intervalBeforeSwitch = CtlText(hwnd, ID_MS);
    SetDlgItemTextW(hwnd, ID_TEXT_EDIT, L"mode switch preserves settings");
    for (int mode : {1, 0, 1}) {
        SendDlgItemMessage(hwnd, ID_UI_MODE, CB_SETCURSEL, mode, 0);
        SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(ID_UI_MODE, CBN_SELCHANGE),
                    (LPARAM)GetDlgItem(hwnd, ID_UI_MODE));
        ST_CHECK(g_advanced == (mode == 1), "mode selector changes interface");
        ST_CHECK(!IsWindowVisible(GetDlgItem(hwnd, ID_ACTION_COMBO)) &&
                 !IsWindowVisible(GetDlgItem(hwnd, ID_POS_CUR)) &&
                 IsWindowVisible(GetDlgItem(hwnd, ID_MODE_COMBO)) == (mode == 1),
                 "Clicking view only shows relevant controls");
        ST_CHECK(IsWindowVisible(GetDlgItem(hwnd, ID_TYPE_COMBO)) &&
                 IsWindowVisible(GetDlgItem(hwnd, ID_REP_COUNT)), "common click controls stay available");
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t textBuffer[128] = {0};
            GetWindowTextW(child, textBuffer, 128);
            std::wstring text(textBuffer);
            ST_CHECK(text.find(L"Time:") != 0 && text.find(L"CPS:") != 0 &&
                     text != L"Advanced settings stay active.", "removed labels are absent");
            if (!IsWindowVisible(child)) continue;
            RECT bounds;
            GetWindowRect(child, &bounds);
            MapWindowPoints(nullptr, hwnd, (POINT*)&bounds, 2);
            ST_CHECK(bounds.left >= 0 && bounds.right <= 500 && bounds.top >= 0 &&
                     bounds.bottom <= MainContentHeight(), "visible controls fit each mode layout");
        }
        ST_CHECK(CtlText(hwnd, ID_TEXT_EDIT) == L"mode switch preserves settings" &&
                 CtlText(hwnd, ID_MS) == intervalBeforeSwitch, "mode changes preserve settings");
        RECT button, client;
        GetWindowRect(GetDlgItem(hwnd, ID_MAIN_BTN), &button);
        MapWindowPoints(nullptr, hwnd, (POINT*)&button, 2);
        GetClientRect(hwnd, &client);
        SCROLLINFO scroll = {sizeof(scroll), SIF_RANGE};
        GetScrollInfo(hwnd, SB_VERT, &scroll);
        ST_CHECK(button.bottom <= client.bottom || scroll.nMax == MainContentHeight() - 1,
                 "mode layout fits or scrolls");
    }

    for (int page : {1, 2, 0}) {
        TabCtrl_SetCurSel(GetDlgItem(hwnd, ID_UI_TABS), page);
        NMHDR notification = {GetDlgItem(hwnd, ID_UI_TABS), ID_UI_TABS, TCN_SELCHANGE};
        SendMessage(hwnd, WM_NOTIFY, ID_UI_TABS, (LPARAM)&notification);
        ST_CHECK(IsWindowVisible(GetDlgItem(hwnd, ID_POS_CUR)) == (page == 1) &&
                 IsWindowVisible(GetDlgItem(hwnd, ID_PROF_COMBO)) == (page == 2) &&
                 IsWindowVisible(GetDlgItem(hwnd, ID_MS)) == (page == 0), "tabs show only their own settings");
        ST_CHECK(IsWindowVisible(GetDlgItem(hwnd, ID_MAIN_BTN)), "Start remains visible on every tab");
        for (const auto& control : g_mainControls) {
            if (!IsWindowVisible(control.hwnd)) continue;
            RECT bounds;
            GetWindowRect(control.hwnd, &bounds);
            MapWindowPoints(nullptr, hwnd, (POINT*)&bounds, 2);
            ST_CHECK(bounds.left >= 0 && bounds.right <= 500 && bounds.top >= 0 &&
                     bounds.bottom <= MainContentHeight(), "tab controls fit the window");
        }
    }

    // --- window basics
    {
        wchar_t t[128] = {0};
        GetWindowTextW(hwnd, t, 128);
        SelfTestLog("- window title\n");
        ST_CHECK(wcscmp(t, L"Ultimate AutoClicker") == 0, "window title is intact");
    }

    // --- interval conversion (Enter in the box)
    SelfTestLog("- interval conversion\n");
    SetDlgItemTextW(hwnd, ID_MS, L"5000");
    SendMessage(GetDlgItem(hwnd, ID_MS), WM_KEYDOWN, VK_RETURN, 0);
    ST_CHECK(CtlText(hwnd, ID_SECS) == L"5", "5000 ms converts to 5 s");
    ST_CHECK(CtlText(hwnd, ID_MS) == L"0", "the ms field is cleared");
    ST_CHECK(g_cfg.intervalUs == 5000000, "the engine interval follows the fields");

    // --- zero interval is clamped
    SelfTestLog("- zero interval clamp\n");
    SetDlgItemTextW(hwnd, ID_HRS, L"0");
    SetDlgItemTextW(hwnd, ID_MINS, L"0");
    SetDlgItemTextW(hwnd, ID_SECS, L"0");
    SetDlgItemTextW(hwnd, ID_MS, L"0");
    SetDlgItemTextW(hwnd, ID_US, L"0");
    SendMessage(GetDlgItem(hwnd, ID_US), WM_KEYDOWN, VK_RETURN, 0);
    ST_CHECK(g_cfg.intervalUs >= ac::kMinIntervalUs, "a zero interval is clamped to the minimum");

    // --- fast interval for the rest of the test
    SetDlgItemTextW(hwnd, ID_MS, L"10");
    SendMessage(GetDlgItem(hwnd, ID_MS), WM_KEYDOWN, VK_RETURN, 0);

    // --- hotkey label
    SelfTestLog("- hotkey\n");
    RECT compact;
    GetClientRect(hwnd, &compact);
    ST_CHECK(compact.right == 500, "main window keeps the original compact width");
    RECT beforeDpi;
    GetWindowRect(hwnd, &beforeDpi);
    SendMessage(hwnd, WM_DPICHANGED, MAKEWPARAM(192, 192), (LPARAM)&beforeDpi);
    GetClientRect(hwnd, &compact);
    ST_CHECK(compact.right <= 500 && compact.bottom <= MainContentHeight(), "high DPI keeps the compact pixel footprint");
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT outer;
    GetWindowRect(hwnd, &outer);
    if (monitor.rcWork.bottom - monitor.rcWork.top >= outer.bottom - outer.top + MainContentHeight() - compact.bottom) {
        ST_CHECK(compact.bottom == MainContentHeight(), "window borders leave the full content height");
        ST_CHECK(!(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_VSCROLL), "full compact layout needs no scrollbar");
    }
    ST_CHECK(CtlText(hwnd, ID_HOTKEY_LABEL).find(L"F6") != std::wstring::npos, "the default hotkey is shown");
    ST_CHECK(CtlText(hwnd, ID_MAIN_BTN) == L"START CLICKING (F6)", "the start button shows the full hotkey");
    CheckCaptionFits(hwnd, ID_MAIN_BTN);
    CheckCaptionFits(hwnd, ID_PICK_BTN);
    CheckCaptionFits(hwnd, ID_CHANGE_HOTKEY_BTN);
    SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(ID_MAIN_BTN, BN_SETFOCUS), (LPARAM)GetDlgItem(hwnd, ID_MAIN_BTN));
    ST_CHECK(!g_engine->IsRunning(), "focusing start does not activate it");
    ST_CHECK(!IsWindowEnabled(GetDlgItem(hwnd, ID_REP_COUNT)), "unlimited repeat disables the count");
    ST_CHECK(!IsWindowEnabled(GetDlgItem(hwnd, ID_POS_X)), "current position disables fixed coordinates");

    SelfTestLog("- small-screen scrolling\n");
    RECT originalWindow;
    GetWindowRect(hwnd, &originalWindow);
    SetWindowPos(hwnd, nullptr, 0, 0, originalWindow.right - originalWindow.left, 400,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(hwnd, WM_VSCROLL, SB_BOTTOM, 0);
    RECT start, client;
    GetWindowRect(GetDlgItem(hwnd, ID_MAIN_BTN), &start);
    MapWindowPoints(nullptr, hwnd, (POINT*)&start, 2);
    GetClientRect(hwnd, &client);
    ST_CHECK(start.top >= 0 && start.bottom <= client.bottom, "start button is reachable on a short screen");
    ScrollMain(hwnd, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, originalWindow.right - originalWindow.left,
        originalWindow.bottom - originalWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // --- countdown + repeat N + stats
    SelfTestLog("- start countdown, repeat N, live stats\n");
    SendMessage(GetDlgItem(hwnd, ID_ACTION_COMBO), CB_SETCURSEL, ACT_TYPE, 0);   // type text
    SetDlgItemTextW(hwnd, ID_TEXT_EDIT, L"");                                     // empty -> no real input
    SendMessage(GetDlgItem(hwnd, ID_REP_TIMES), BM_CLICK, 0, 0);
    SetDlgItemTextW(hwnd, ID_REP_COUNT, L"3");
    SetDlgItemTextW(hwnd, ID_DELAY_EDIT, L"2");

    SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
    PumpMessages(400);
    EngineStatus st = g_engine->Status();
    ST_CHECK(st.state == ST_COUNTDOWN, "pressing start begins a countdown");
    ST_CHECK(st.countdownSec >= 1 && st.countdownSec <= 2, "the countdown shows 1..2 seconds");
    ST_CHECK(CtlText(hwnd, ID_MAIN_BTN).find(L"STARTING IN") != std::wstring::npos, "the button shows the countdown");
    ST_CHECK(g_engine->Status().clicksDone == 0, "nothing is clicked during the countdown");

    for (int i = 0; i < 200 && g_engine->IsRunning(); i++) PumpMessages(50);
    st = g_engine->Status();
    ST_CHECK(st.state == ST_IDLE, "the engine goes back to idle");
    ST_CHECK(st.clicksDone == 3, "exactly 3 clicks were performed");
    PumpMessages(600);
    ST_CHECK(g_engine->Status().clicksDone == 3, "it does not restart itself (repeat N regression)");
    ST_CHECK(CtlText(hwnd, ID_STAT_CLICKS).find(L"3") != std::wstring::npos, "the click counter is shown");
    ST_CHECK(CtlText(hwnd, ID_MAIN_BTN).find(L"START CLICKING") != std::wstring::npos, "the button is back to idle");

    // --- settings round trip through the ini file
    SelfTestLog("- settings file round trip\n");
    {
        std::wstring realIni = g_iniPath;
        g_iniPath = realIni + L".selftest";
        ApplyUiToConfig();
        SaveIni();
        std::map<std::wstring, std::wstring> ini = ReadIni(g_iniPath);
        auto it = ini.find(L"Settings/config");
        ST_CHECK(it != ini.end(), "the settings file was written");
        Config loaded;
        bool ok = it != ini.end() && DecodeConfig(it->second, loaded);
        ST_CHECK(ok, "the settings file can be decoded");
        if (ok) {
            ST_CHECK(loaded.intervalUs == g_cfg.intervalUs, "the interval survives the round trip");
            ST_CHECK(loaded.startDelaySec == g_cfg.startDelaySec, "the start delay survives the round trip");
            ST_CHECK(loaded.maxClicks == g_cfg.maxClicks, "the repeat count survives the round trip");
            ST_CHECK(loaded.action == g_cfg.action, "the action survives the round trip");
            ST_CHECK(loaded.hotkey == g_cfg.hotkey, "the hotkey survives the round trip");
        }
        DeleteFileW(g_iniPath.c_str());
        g_iniPath = realIni;
    }

    // --- profiles
    SelfTestLog("- profiles\n");
    {
        SendMessage(GetDlgItem(hwnd, ID_PROF_COMBO), WM_SETTEXT, 0, (LPARAM)L"SelfTest");
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_PROF_SAVE, 0);
        ST_CHECK(g_profiles.size() == 1 && g_profiles[0] == L"SelfTest", "a profile is saved");
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_PROF_DEL, 0);
        ST_CHECK(g_profiles.empty(), "a profile is deleted");
    }

    // --- screen-edge safety uses the real worker, hook and non-activating overlay
    SelfTestLog("- screen-edge emergency stop\n");
    {
        ST_CHECK(g_emergencyMouseHook != nullptr, "the screen-edge mouse hook is installed");
        ST_CHECK(IsWindowVisible(GetDlgItem(hwnd, ID_SAFETY_STATUS)), "the emergency-stop hint is visible");
        MONITORINFO monitor = {sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        POINT center = {(monitor.rcMonitor.left + monitor.rcMonitor.right) / 2,
            (monitor.rcMonitor.top + monitor.rcMonitor.bottom) / 2};
        POINT edge = {monitor.rcMonitor.left, center.y}, original;
        GetCursorPos(&original);
        SetDlgItemTextW(hwnd, ID_DELAY_EDIT, L"5");
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
        PumpMessages(40);
        HandleEmergencyMotion(center, false);
        HandleEmergencyMotion(edge, true);
        WinSink cursor;
        cursor.MoveCursor(edge.x, edge.y);
        PumpMessages(100);
        ST_CHECK(g_engine->Status().state == ST_COUNTDOWN, "injected edge moves do not stop the countdown");
        ShowWindow(hwnd, SW_MINIMIZE);
        PumpMessages(40);
        HandleEmergencyMotion(edge, false);
        PumpMessages(100);
        ST_CHECK(!g_engine->IsRunning() && g_engine->Status().emergencyStopped, "physical edge movement cancels the worker");
        ST_CHECK(g_engine->Status().clicksDone == 0, "edge cancellation sends no clicks during countdown");
        ST_CHECK(IsWindowVisible(g_emergencyOverlay), "the emergency-stop visual is shown");
        ST_CHECK(IsIconic(hwnd) && GetWindow(g_emergencyOverlay, GW_OWNER) == nullptr,
            "the warning remains independent of the minimized main window");
        LONG_PTR style = GetWindowLongPtrW(g_emergencyOverlay, GWL_EXSTYLE);
        ST_CHECK((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) ==
            (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE), "the visual is transparent to clicks and cannot activate");
        ST_CHECK(GetForegroundWindow() != g_emergencyOverlay, "the warning does not steal focus");
        ST_CHECK(GetText(hwnd, ID_SAFETY_STATUS).find(L"EMERGENCY STOP") != std::wstring::npos, "the stop reason stays visible in the app");
        ShowWindow(hwnd, SW_RESTORE);
        cursor.MoveCursor(original.x, original.y);
        PumpMessages(2600);
        ST_CHECK(!IsWindowVisible(g_emergencyOverlay) && g_engine->Status().emergencyStopped, "the visual expires without clearing the stop reason");
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
        PumpMessages(40);
        ST_CHECK(g_engine->IsRunning() && !g_engine->Status().emergencyStopped, "only an explicit restart clears the emergency stop");
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
        PumpMessages(40);
    }

    // --- the countdown can be cancelled
    SelfTestLog("- cancelling the countdown\n");
    SetDlgItemTextW(hwnd, ID_DELAY_EDIT, L"5");
    SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
    PumpMessages(300);
    ST_CHECK(g_engine->Status().state == ST_COUNTDOWN, "the countdown is running");
    SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_MAIN_BTN, 0);
    PumpMessages(300);
    ST_CHECK(g_engine->Status().state == ST_IDLE, "pressing start again cancels the countdown");
    ST_CHECK(g_engine->Status().clicksDone == 0, "nothing was clicked after the cancel");

    // --- the dialogs open and close
    SelfTestLog("- dialogs\n");
    {
        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_CHANGE_HOTKEY_BTN, 0);
        HWND hk = FindWindowW(L"HotkeyModalClass", nullptr);
        ST_CHECK(hk != nullptr, "the hotkey dialog opens");
        if (hk) { CheckControlBounds(hk); CheckDialogScaling(hk, 420, 204); }
        if (hk) { PostMessage(hk, WM_CLOSE, 0, 0); PumpMessages(200); }
        ST_CHECK(FindWindowW(L"HotkeyModalClass", nullptr) == nullptr, "the hotkey dialog closes");

        SendMessage(hwnd, WM_COMMAND, (WPARAM)ID_SEQ_BTN, 0);
        HWND sq = FindWindowW(L"SeqModalClass", nullptr);
        ST_CHECK(sq != nullptr, "the points dialog opens");
        if (sq) {
            CheckControlBounds(sq);
            SetDlgItemTextW(sq, ID_SEQ_REC_BTN, L"Stop recording (ESC)");
            CheckCaptionFits(sq, ID_SEQ_REC_BTN);
            CheckDialogScaling(sq, 420, 338);
        }
        if (sq) { PostMessage(sq, WM_CLOSE, 0, 0); PumpMessages(200); }
        ST_CHECK(FindWindowW(L"SeqModalClass", nullptr) == nullptr, "the points dialog closes");
    }

    // --- restore the defaults
    SetDlgItemTextW(hwnd, ID_DELAY_EDIT, L"3");
    SendMessage(GetDlgItem(hwnd, ID_ACTION_COMBO), CB_SETCURSEL, ACT_CLICK, 0);
    SendMessage(GetDlgItem(hwnd, ID_REP_INF), BM_CLICK, 0, 0);
    ApplyUiToConfig();

    SelfTestLog("===============================\n");
    SelfTestLog("%d checks, %d failures\n", g_selfChecks, g_selfFails);
    return g_selfFails == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // DPI awareness
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *pfnSetProcessDpiAwarenessContext)(HANDLE);
        pfnSetProcessDpiAwarenessContext p = (pfnSetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (p) p((HANDLE)-4);
        else {
            typedef BOOL (WINAPI *pfnSetProcessDPIAware)(void);
            pfnSetProcessDPIAware q = (pfnSetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
            if (q) q();
        }
    }

    hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hInterrupt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    WinClock clock;
    clock.hInterrupt = hInterrupt;
    QueryPerformanceFrequency(&clock.qpf);
    clock.hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!clock.hTimer) clock.hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // high resolution timer for the whole session, released on exit
    HMODULE hNtdll = LoadLibraryA("ntdll.dll");
    typedef NTSTATUS (NTAPI *pfnNtSetTimerResolution)(ULONG, BOOLEAN, PULONG);
    pfnNtSetTimerResolution pNtSetTimerResolution = nullptr;
    if (hNtdll) pNtSetTimerResolution = (pfnNtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
    if (pNtSetTimerResolution) { ULONG actual = 0; pNtSetTimerResolution(5000, TRUE, &actual); }

    WinSink sink;
    Engine engine(clock, sink);
    g_clock = &clock;
    g_sink = &sink;
    g_engine = &engine;

    g_font = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    g_bold = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"UltimateAutoClicker";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW wcHk = { 0 };
    wcHk.cbSize = sizeof(wcHk);
    wcHk.lpfnWndProc = HotkeyModalProc;
    wcHk.hInstance = hInstance;
    wcHk.lpszClassName = L"HotkeyModalClass";
    wcHk.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcHk.hIcon = wc.hIcon;
    RegisterClassExW(&wcHk);

    WNDCLASSEXW wcSeq = { 0 };
    wcSeq.cbSize = sizeof(wcSeq);
    wcSeq.lpfnWndProc = SeqModalProc;
    wcSeq.hInstance = hInstance;
    wcSeq.lpszClassName = L"SeqModalClass";
    wcSeq.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcSeq.hIcon = wc.hIcon;
    RegisterClassExW(&wcSeq);

    // default config + saved settings
    g_cfg.hotkey.mods = 0;
    g_cfg.hotkey.keys = { VK_F6 };
    g_iniPath = IniPath();
    LoadIni();
    if (g_cfg.hotkey.empty()) { g_cfg.hotkey.keys = { VK_F6 }; }
    engine.ApplyConfig(g_cfg);

    RECT rc = { 0, 0, 500, 700 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, L"UltimateAutoClicker", L"Ultimate AutoClicker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    SizeClient(hwnd, 500, MainContentHeight());
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHookProc, GetModuleHandleW(nullptr), 0);

    HANDLE hThread = CreateThread(nullptr, 0, ClickerThread, nullptr, 0, nullptr);

    // --selftest: drive the real UI and report, then exit
    bool selftest = wcsstr(GetCommandLineW(), L"--selftest") != nullptr;
    if (selftest) {
        int rc = RunSelfTest(hwnd);
        if (g_engine->IsRunning()) { g_engine->Stop(); SetEvent(hInterrupt); }
        DestroyWindow(hwnd);
        SetEvent(hWake);
        SetEvent(hInterrupt);
        WaitForSingleObject(hThread, 2000);
        CloseHandle(hThread);
        return rc;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        HWND dialog = g_hotkeyDlgOpen ? g_hotkeyDlg : (g_seqDlgOpen ? g_seqDlg : hwnd);
        if (!IsWindow(dialog) || !IsDialogMessage(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_running = false;
    SetEvent(hWake);
    SetEvent(hInterrupt);
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    CloseHandle(hWake);
    CloseHandle(hInterrupt);

    if (clock.hTimer) CloseHandle(clock.hTimer);
    if (pNtSetTimerResolution) { ULONG actual = 0; pNtSetTimerResolution(5000, FALSE, &actual); }   // release the resolution
    if (hNtdll) FreeLibrary(hNtdll);
    TimerResolutionOff();
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    return 0;
}

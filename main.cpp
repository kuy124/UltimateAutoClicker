#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>

#define IDI_APPICON 101

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x0002
#endif

// Control IDs
enum {
    ID_HRS = 201, ID_MINS, ID_SECS, ID_MS, ID_US, ID_WARN_STATIC,
    ID_BTN_COMBO, ID_TYPE_COMBO,
    ID_REP_INF, ID_REP_TIMES, ID_REP_COUNT,
    ID_POS_CUR, ID_POS_CUST, ID_POS_X, ID_POS_Y, ID_PICK_BTN,
    ID_HOTKEY_LABEL, ID_CHANGE_HOTKEY_BTN,
    ID_TOP_CHECK, ID_MAIN_BTN
};

// Global State
volatile bool running = true;
volatile bool clicking = false;
volatile long long intervalUs = 100000; // Microseconds (100ms default)
volatile int mouseBtn = 0, clickType = 0, maxClicks = 0, clicksDone = 0;
volatile bool customPos = false;
volatile int targetX = 0, targetY = 0;

HANDLE hTimerHandle = NULL;
HMODULE hWinmm = NULL;
HMODULE hNtdll = NULL;
bool g_isFormatting = false;

typedef NTSTATUS (NTAPI *pfnNtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG ActualResolution);
typedef MMRESULT (WINAPI *pfnTimeBeginPeriod)(UINT);
typedef MMRESULT (WINAPI *pfnTimeEndPeriod)(UINT);

// Hotkey State
WORD currentHkVal = MAKEWORD(VK_F6, 0); // Default F6
char currentHotkeyName[64] = "F6";

HWND hMainWnd;
HHOOK hMouseHook = NULL;
HFONT hFont = NULL, hBold = NULL;

HANDLE hClickEvent = NULL;

// Helper to format hotkey key combinations into readable text
void FormatHotkeyName(WORD hkVal, char* outBuffer, size_t bufSize) {
    UINT vkey = LOBYTE(hkVal);
    UINT mods = HIBYTE(hkVal);
    
    outBuffer[0] = '\0';
    if (mods & HOTKEYF_CONTROL) lstrcatA(outBuffer, "Ctrl + ");
    if (mods & HOTKEYF_SHIFT)   lstrcatA(outBuffer, "Shift + ");
    if (mods & HOTKEYF_ALT)     lstrcatA(outBuffer, "Alt + ");

    char keyName[32] = {0};
    LONG scanCode = MapVirtualKeyA(vkey, MAPVK_VK_TO_VSC) << 16;
    switch (vkey) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scanCode |= 0x01000000; break;
    }
    GetKeyNameTextA(scanCode, keyName, sizeof(keyName));
    lstrcatA(outBuffer, keyName);
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
        PostMessage(hMainWnd, WM_USER + 2, (WPARAM)pMouse->pt.x, (LPARAM)pMouse->pt.y);
        UnhookWindowsHookEx(hMouseHook);
        hMouseHook = NULL;
        return 1; 
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Read floating point values from edit boxes (supports '.' and ',')
double GetEditDouble(HWND hwnd, int id) {
    char buf[64] = {0};
    GetDlgItemTextA(hwnd, id, buf, sizeof(buf));
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == ',') buf[i] = '.';
    }
    return atof(buf);
}

// Convert units, update edit boxes, and save interval when Enter is pressed
void ConvertAndSaveInterval(HWND hwnd) {
    if (g_isFormatting) return;
    g_isFormatting = true;

    double h  = GetEditDouble(hwnd, ID_HRS);
    double m  = GetEditDouble(hwnd, ID_MINS);
    double s  = GetEditDouble(hwnd, ID_SECS);
    double ms = GetEditDouble(hwnd, ID_MS);
    double us = GetEditDouble(hwnd, ID_US);

    double totalUsD = (us) + (ms * 1000.0) + (s * 1000000.0) + (m * 60000000.0) + (h * 3600000000.0);
    if (totalUsD < 0) totalUsD = 0;

    long long totalUs = (long long)(totalUsD + 0.5);
    intervalUs = totalUs;

    long long outH  = totalUs / 3600000000LL;
    long long rem1  = totalUs % 3600000000LL;

    long long outM  = rem1 / 60000000LL;
    long long rem2  = rem1 % 60000000LL;

    long long outS  = rem2 / 1000000LL;
    long long rem3  = rem2 % 1000000LL;

    long long outMs = rem3 / 1000LL;
    long long outUs = rem3 % 1000LL;

    SetDlgItemInt(hwnd, ID_HRS,  (UINT)outH,  FALSE);
    SetDlgItemInt(hwnd, ID_MINS, (UINT)outM,  FALSE);
    SetDlgItemInt(hwnd, ID_SECS, (UINT)outS,  FALSE);
    SetDlgItemInt(hwnd, ID_MS,   (UINT)outMs, FALSE);
    SetDlgItemInt(hwnd, ID_US,   (UINT)outUs, FALSE);

    if (intervalUs < 5000) {
        SetDlgItemTextA(hwnd, ID_WARN_STATIC, "[!] Warning: Interval is below 5 ms (< 200 CPS). Target app may lag.");
    } else {
        SetDlgItemTextA(hwnd, ID_WARN_STATIC, "");
    }

    g_isFormatting = false;
}

// Subclass procedure to catch Enter key inside edit controls
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (uMsg) {
        case WM_GETDLGCODE:
            if (wParam == VK_RETURN) return DLGC_WANTALLKEYS;
            break;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                HWND hMain = (HWND)dwRefData;
                ConvertAndSaveInterval(hMain);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wParam == VK_RETURN) return 0; // Suppress beep
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, EditSubclassProc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Modal Dialog Procedure for Changing Hotkey
LRESULT CALLBACK HotkeyModalProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT f = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
            HFONT fBold = CreateFontA(15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");

            HWND hText = CreateWindowExA(0, "STATIC", "Press keys on your keyboard:", WS_VISIBLE | WS_CHILD, 15, 15, 270, 20, hwnd, NULL, NULL, NULL);
            SendMessage(hText, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hHkCtrl = CreateWindowExA(0, HOTKEY_CLASSA, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | WS_TABSTOP, 15, 40, 275, 28, hwnd, (HMENU)301, NULL, NULL);
            SendMessage(hHkCtrl, WM_SETFONT, (WPARAM)fBold, TRUE);
            SendMessage(hHkCtrl, HKM_SETHOTKEY, currentHkVal, 0);
            SendMessage(hHkCtrl, HKM_SETRULES, HKCOMB_NONE, 0);

            HWND hBtnSave = CreateWindowExA(0, "BUTTON", "Save Hotkey", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP, 50, 85, 110, 30, hwnd, (HMENU)IDOK, NULL, NULL);
            SendMessage(hBtnSave, WM_SETFONT, (WPARAM)fBold, TRUE);

            HWND hBtnCancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 170, 85, 120, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
            SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)f, TRUE);

            SetFocus(hHkCtrl);
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                HWND hHkCtrl = GetDlgItem(hwnd, 301);
                LRESULT hk = SendMessage(hHkCtrl, HKM_GETHOTKEY, 0, 0);
                UINT vkey = LOBYTE(hk), mods = HIBYTE(hk), regMods = 0;

                if (mods & HOTKEYF_ALT) regMods |= MOD_ALT;
                if (mods & HOTKEYF_CONTROL) regMods |= MOD_CONTROL;
                if (mods & HOTKEYF_SHIFT) regMods |= MOD_SHIFT;

                if (vkey != 0) {
                    UnregisterHotKey(hMainWnd, 1);
                    if (RegisterHotKey(hMainWnd, 1, regMods, vkey)) {
                        currentHkVal = (WORD)hk;
                        FormatHotkeyName(currentHkVal, currentHotkeyName, sizeof(currentHotkeyName));

                        char lblBuf[128], btnText[128];
                        wsprintfA(lblBuf, "Current Hotkey: %s", currentHotkeyName);
                        SetDlgItemTextA(hMainWnd, ID_HOTKEY_LABEL, lblBuf);

                        wsprintfA(btnText, clicking ? "STOP CLICKING (%s)" : "START CLICKING (%s)", currentHotkeyName);
                        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);

                        EnableWindow(hMainWnd, TRUE);
                        SetForegroundWindow(hMainWnd);
                        DestroyWindow(hwnd);
                    } else {
                        // Re-register previous hotkey on error
                        UINT prevVk = LOBYTE(currentHkVal), prevMods = HIBYTE(currentHkVal), prevReg = 0;
                        if (prevMods & HOTKEYF_ALT) prevReg |= MOD_ALT;
                        if (prevMods & HOTKEYF_CONTROL) prevReg |= MOD_CONTROL;
                        if (prevMods & HOTKEYF_SHIFT) prevReg |= MOD_SHIFT;
                        RegisterHotKey(hMainWnd, 1, prevReg, prevVk);

                        MessageBoxA(hwnd, "Failed to register hotkey. It might be in use by another application.", "Hotkey In Use", MB_ICONERROR);
                    }
                }
            }
            if (id == IDCANCEL) {
                EnableWindow(hMainWnd, TRUE);
                SetForegroundWindow(hMainWnd);
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_CLOSE: {
            EnableWindow(hMainWnd, TRUE);
            SetForegroundWindow(hMainWnd);
            DestroyWindow(hwnd);
            break;
        }
        case WM_CTLCOLORSTATIC: {
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Ultra-optimized, zero-lag sub-millisecond timer delay routine
void UltraSleepUs(long long us) {
    if (!running || !clicking) return;

    if (us <= 0) {
        YieldProcessor();
        Sleep(0);
        return;
    }

    if (hTimerHandle) {
        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = -(us * 10LL); // 100ns units
        SetWaitableTimer(hTimerHandle, &liDueTime, 0, NULL, NULL, 0);

        HANDLE handles[2] = { hTimerHandle, hClickEvent };
        DWORD res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (res == WAIT_OBJECT_0 + 1) {
            CancelWaitableTimer(hTimerHandle);
            return;
        }
    } else {
        DWORD ms = (DWORD)(us / 1000);
        if (ms > 0) {
            WaitForSingleObject(hClickEvent, ms);
        } else {
            YieldProcessor();
            Sleep(0);
        }
    }
}

DWORD WINAPI ClickerThread(LPVOID lpParam) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    INPUT inputs[2] = { 0 };
    inputs[0].type = inputs[1].type = INPUT_MOUSE;

    while (running) {
        if (!clicking) {
            WaitForSingleObject(hClickEvent, INFINITE);
        }
        if (!running) break;

        if (maxClicks > 0 && clicksDone >= maxClicks) {
            clicking = false;
            PostMessage(hMainWnd, WM_USER + 1, 0, 0); 
            continue;
        }

        if (customPos) SetCursorPos(targetX, targetY);

        DWORD downFlag = MOUSEEVENTF_LEFTDOWN, upFlag = MOUSEEVENTF_LEFTUP;
        if (mouseBtn == 1) { downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP; }
        else if (mouseBtn == 2) { downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP; }

        inputs[0].mi.dwFlags = downFlag;
        inputs[1].mi.dwFlags = upFlag;

        SendInput(2, inputs, sizeof(INPUT));

        if (clickType == 1) { 
            UltraSleepUs(20000); // 20ms pause for double click
            SendInput(2, inputs, sizeof(INPUT));
        }

        clicksDone++;

        if (!running || !clicking) continue;

        UltraSleepUs(intervalUs);
    }
    return 0;
}

// Safely stop the auto-clicker
void StopClicking() {
    if (clicking) {
        clicking = false;
        char btnText[128];
        wsprintfA(btnText, "START CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
        ResetEvent(hClickEvent);
        SetEvent(hClickEvent);
    }
}

void ToggleClicking() {
    if (!clicking) {
        ConvertAndSaveInterval(hMainWnd);

        clicking = true;

        mouseBtn = SendMessage(GetDlgItem(hMainWnd, ID_BTN_COMBO), CB_GETCURSEL, 0, 0);
        clickType = SendMessage(GetDlgItem(hMainWnd, ID_TYPE_COMBO), CB_GETCURSEL, 0, 0);

        bool isInfinite = SendMessage(GetDlgItem(hMainWnd, ID_REP_INF), BM_GETCHECK, 0, 0) == BST_CHECKED;
        maxClicks = isInfinite ? 0 : GetDlgItemInt(hMainWnd, ID_REP_COUNT, 0, 0);
        clicksDone = 0;

        customPos = SendMessage(GetDlgItem(hMainWnd, ID_POS_CUST), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (customPos) {
            targetX = GetDlgItemInt(hMainWnd, ID_POS_X, 0, 1);
            targetY = GetDlgItemInt(hMainWnd, ID_POS_Y, 0, 1);
        }

        char btnText[128];
        wsprintfA(btnText, "STOP CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
        SetEvent(hClickEvent); 
    } else {
        StopClicking();
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hMainWnd = hwnd;
            hFont = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
            hBold = CreateFontA(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");

            auto AddCtrl = [&](LPCSTR cls, LPCSTR txt, DWORD style, int x, int y, int w, int h, int id, HFONT f = hFont) {
                HWND hCtrl = CreateWindowExA(0, cls, txt, WS_VISIBLE | WS_CHILD | style, x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, NULL, NULL);
                SendMessage(hCtrl, WM_SETFONT, (WPARAM)f, TRUE);
                return hCtrl;
            };

            AddCtrl("BUTTON", "Click Interval", BS_GROUPBOX, 10, 10, 365, 55, 0);
            AddCtrl("STATIC", "Hrs", 0, 18, 33, 24, 20, 0);    
            HWND hHrs  = AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_CENTER, 44, 30, 30, 20, ID_HRS);
            AddCtrl("STATIC", "Mins", 0, 80, 33, 30, 20, 0);   
            HWND hMins = AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_CENTER, 112, 30, 30, 20, ID_MINS);
            AddCtrl("STATIC", "Secs", 0, 148, 33, 28, 20, 0);  
            HWND hSecs = AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_CENTER, 178, 30, 30, 20, ID_SECS);
            AddCtrl("STATIC", "Ms", 0, 214, 33, 20, 20, 0);    
            HWND hMs   = AddCtrl("EDIT", "100", WS_BORDER | WS_TABSTOP | ES_CENTER, 236, 30, 45, 20, ID_MS);
            AddCtrl("STATIC", "\xB5s", 0, 287, 33, 18, 20, 0);  
            HWND hUs   = AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_CENTER, 307, 30, 55, 20, ID_US);

            // Subclass all 5 interval edit controls to catch Enter key
            SetWindowSubclass(hHrs,  EditSubclassProc, 1, (DWORD_PTR)hwnd);
            SetWindowSubclass(hMins, EditSubclassProc, 1, (DWORD_PTR)hwnd);
            SetWindowSubclass(hSecs, EditSubclassProc, 1, (DWORD_PTR)hwnd);
            SetWindowSubclass(hMs,   EditSubclassProc, 1, (DWORD_PTR)hwnd);
            SetWindowSubclass(hUs,   EditSubclassProc, 1, (DWORD_PTR)hwnd);

            AddCtrl("STATIC", "", 0, 15, 68, 355, 18, ID_WARN_STATIC);

            AddCtrl("BUTTON", "Click Options", BS_GROUPBOX, 10, 88, 365, 60, 0);
            AddCtrl("STATIC", "Mouse Button:", 0, 25, 113, 90, 20, 0);
            HWND hBtnCombo = AddCtrl("COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 120, 108, 80, 100, ID_BTN_COMBO);
            AddCtrl("STATIC", "Type:", 0, 220, 113, 40, 20, 0);
            HWND hTypeCombo = AddCtrl("COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 260, 108, 95, 100, ID_TYPE_COMBO);
            
            SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Left"); SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Right"); SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Middle");
            SendMessageA(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)"Single"); SendMessageA(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)"Double");
            SendMessageA(hBtnCombo, CB_SETCURSEL, 0, 0); SendMessageA(hTypeCombo, CB_SETCURSEL, 0, 0);

            AddCtrl("BUTTON", "Click Repeat", BS_GROUPBOX, 10, 158, 365, 80, 0);
            HWND hRad1 = AddCtrl("BUTTON", "Repeat until stopped", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 25, 178, 150, 20, ID_REP_INF);
            AddCtrl("BUTTON", "Repeat           times", BS_AUTORADIOBUTTON, 25, 208, 150, 20, ID_REP_TIMES);
            AddCtrl("EDIT", "10", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 90, 208, 45, 20, ID_REP_COUNT);
            SendMessage(hRad1, BM_SETCHECK, BST_CHECKED, 0);

            AddCtrl("BUTTON", "Cursor Position", BS_GROUPBOX, 10, 248, 365, 80, 0);
            HWND hPos1 = AddCtrl("BUTTON", "Current Location", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 25, 268, 130, 20, ID_POS_CUR);
            AddCtrl("BUTTON", "Specific Location", BS_AUTORADIOBUTTON, 25, 298, 130, 20, ID_POS_CUST);
            AddCtrl("STATIC", "X:", 0, 155, 298, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 175, 298, 40, 20, ID_POS_X);
            AddCtrl("STATIC", "Y:", 0, 225, 298, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 245, 298, 40, 20, ID_POS_Y);
            AddCtrl("BUTTON", "Pick Location", BS_PUSHBUTTON | WS_TABSTOP, 290, 296, 75, 24, ID_PICK_BTN);
            SendMessage(hPos1, BM_SETCHECK, BST_CHECKED, 0);

            AddCtrl("BUTTON", "Start / Stop Hotkey", BS_GROUPBOX, 10, 338, 365, 65, 0);
            AddCtrl("STATIC", "Current Hotkey: F6", 0, 25, 362, 180, 20, ID_HOTKEY_LABEL, hBold);
            AddCtrl("BUTTON", "Change Hotkey...", BS_PUSHBUTTON | WS_TABSTOP, 215, 357, 145, 28, ID_CHANGE_HOTKEY_BTN);

            AddCtrl("BUTTON", "Always on Top", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 415, 120, 20, ID_TOP_CHECK);
            
            AddCtrl("BUTTON", "START CLICKING (F6)", BS_PUSHBUTTON | WS_TABSTOP, 10, 440, 365, 45, ID_MAIN_BTN, hBold);

            RegisterHotKey(hwnd, 1, 0, VK_F6);
            break;
        }
        case WM_HOTKEY: {
            // Ignore hotkeys while modal dialog is open / main window is disabled
            if (wParam == 1 && IsWindowEnabled(hwnd)) {
                ToggleClicking();
            }
            break;
        }
        case WM_USER + 1: ToggleClicking(); break; 
        
        case WM_USER + 2: {
            SetDlgItemInt(hwnd, ID_POS_X, (int)wParam, TRUE);
            SetDlgItemInt(hwnd, ID_POS_Y, (int)lParam, TRUE);
            SendMessage(GetDlgItem(hwnd, ID_POS_CUST), BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(GetDlgItem(hwnd, ID_POS_CUR), BM_SETCHECK, BST_UNCHECKED, 0);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);

            if (id == ID_MAIN_BTN) { ToggleClicking(); SetFocus(hwnd); }
            if (id == ID_PICK_BTN) {
                ShowWindow(hwnd, SW_HIDE);
                hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);
            }
            if (id == ID_TOP_CHECK) {
                bool isTop = SendMessage((HWND)lParam, BM_GETCHECK, 0, 0);
                SetWindowPos(hwnd, isTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            if (id == ID_CHANGE_HOTKEY_BTN) {
                StopClicking(); // Automatically disable the autoclicker if running!

                EnableWindow(hwnd, FALSE);

                RECT rcMain;
                GetWindowRect(hwnd, &rcMain);
                int modalW = 310, modalH = 165;
                int modalX = rcMain.left + (rcMain.right - rcMain.left - modalW) / 2;
                int modalY = rcMain.top + (rcMain.bottom - rcMain.top - modalH) / 2;

                HWND hModal = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "HotkeyModalClass", "Change Hotkey",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    modalX, modalY, modalW, modalH, hwnd, NULL, GetModuleHandle(NULL), NULL);
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            if ((HWND)lParam == GetDlgItem(hwnd, ID_WARN_STATIC)) {
                HDC hdcStatic = (HDC)wParam;
                SetTextColor(hdcStatic, RGB(200, 30, 30));
                SetBkMode(hdcStatic, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_DESTROY: { 
            running = false; 
            if (hMouseHook) UnhookWindowsHookEx(hMouseHook);
            UnregisterHotKey(hwnd, 1); 
            if (hFont) DeleteObject(hFont);
            if (hBold) DeleteObject(hBold);

            if (hTimerHandle) {
                CloseHandle(hTimerHandle);
                hTimerHandle = NULL;
            }

            if (hNtdll) {
                FreeLibrary(hNtdll);
            }

            if (hWinmm) {
                pfnTimeEndPeriod pTimeEndPeriod = (pfnTimeEndPeriod)GetProcAddress(hWinmm, "timeEndPeriod");
                if (pTimeEndPeriod) pTimeEndPeriod(1);
                FreeLibrary(hWinmm);
            }

            PostQuitMessage(0); 
            break; 
        }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    hTimerHandle = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimerHandle) {
        hTimerHandle = CreateWaitableTimerW(NULL, FALSE, NULL);
    }

    hNtdll = LoadLibraryA("ntdll.dll");
    if (hNtdll) {
        pfnNtSetTimerResolution pNtSetTimerResolution = (pfnNtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (pNtSetTimerResolution) {
            ULONG actualRes = 0;
            pNtSetTimerResolution(5000, TRUE, &actualRes);
        }
    }

    hWinmm = LoadLibraryA("winmm.dll");
    if (hWinmm) {
        pfnTimeBeginPeriod pTimeBeginPeriod = (pfnTimeBeginPeriod)GetProcAddress(hWinmm, "timeBeginPeriod");
        if (pTimeBeginPeriod) pTimeBeginPeriod(1);
    }

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icex);

    // Register Modal Window Class
    WNDCLASSEXA wcModal = { 0 };
    wcModal.cbSize = sizeof(WNDCLASSEXA);
    wcModal.lpfnWndProc = HotkeyModalProc;
    wcModal.hInstance = hInstance;
    wcModal.lpszClassName = "HotkeyModalClass";
    wcModal.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcModal.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassExA(&wcModal);

    // Register Main Window Class
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "UltimateAutoClicker";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); 
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA("UltimateAutoClicker", "Ultimate AutoClicker", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 540, NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);

    hClickEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    HANDLE hThread = CreateThread(NULL, 0, ClickerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    running = false;
    SetEvent(hClickEvent); 
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(hClickEvent);

    return 0;
}
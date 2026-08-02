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
    ID_HOTKEY_CTRL, ID_SET_HOTKEY,
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

// Store the readable name of the hotkey so we can show it on the button
char currentHotkeyName[64] = "F6";

HWND hMainWnd;
HHOOK hMouseHook = NULL;
HFONT hFont = NULL, hBold = NULL;

HANDLE hClickEvent = NULL;

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

    // Compute total microseconds
    double totalUsD = (us) + (ms * 1000.0) + (s * 1000000.0) + (m * 60000000.0) + (h * 3600000000.0);
    if (totalUsD < 0) totalUsD = 0;

    long long totalUs = (long long)(totalUsD + 0.5);
    intervalUs = totalUs;

    // Convert across units
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

    // Update red warning label
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
            if (wParam == VK_RETURN) {
                return DLGC_WANTALLKEYS;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                HWND hMain = (HWND)dwRefData;
                ConvertAndSaveInterval(hMain);
                return 0; // Handled
            }
            break;
        case WM_CHAR:
            if (wParam == VK_RETURN) {
                return 0; // Suppress beep sound
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, EditSubclassProc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
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

void ToggleClicking() {
    char btnText[128];

    if (!clicking) {
        // Ensure values are saved/converted when starting
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

        wsprintfA(btnText, "STOP CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
        SetEvent(hClickEvent); 
    } else {
        clicking = false;
        wsprintfA(btnText, "START CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
        ResetEvent(hClickEvent);
        SetEvent(hClickEvent); 
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
            AddCtrl("STATIC", "1. Press keys:", 0, 25, 368, 90, 20, 0);
            HWND hHotkey = AddCtrl(HOTKEY_CLASSA, NULL, WS_BORDER | WS_TABSTOP, 115, 365, 110, 24, ID_HOTKEY_CTRL);
            SendMessage(hHotkey, HKM_SETHOTKEY, VK_F6, 0);
            AddCtrl("BUTTON", "2. Apply Hotkey", BS_PUSHBUTTON | WS_TABSTOP, 235, 364, 120, 26, ID_SET_HOTKEY);

            AddCtrl("BUTTON", "Always on Top", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 415, 120, 20, ID_TOP_CHECK);
            
            AddCtrl("BUTTON", "START CLICKING (F6)", BS_PUSHBUTTON | WS_TABSTOP, 10, 440, 365, 45, ID_MAIN_BTN, hBold);

            RegisterHotKey(hwnd, 1, 0, VK_F6);
            break;
        }
        case WM_HOTKEY: if (wParam == 1) ToggleClicking(); break;
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
            if (id == ID_SET_HOTKEY) {
                LRESULT hk = SendMessage(GetDlgItem(hwnd, ID_HOTKEY_CTRL), HKM_GETHOTKEY, 0, 0);
                UINT vkey = LOBYTE(hk), mods = HIBYTE(hk), regMods = 0;
                
                if (mods & HOTKEYF_ALT) regMods |= MOD_ALT;
                if (mods & HOTKEYF_CONTROL) regMods |= MOD_CONTROL;
                if (mods & HOTKEYF_SHIFT) regMods |= MOD_SHIFT;

                if (vkey != 0) {
                    UnregisterHotKey(hwnd, 1);
                    if (RegisterHotKey(hwnd, 1, regMods, vkey)) {
                        
                        currentHotkeyName[0] = '\0';
                        if (mods & HOTKEYF_CONTROL) lstrcatA(currentHotkeyName, "Ctrl + ");
                        if (mods & HOTKEYF_SHIFT) lstrcatA(currentHotkeyName, "Shift + ");
                        if (mods & HOTKEYF_ALT) lstrcatA(currentHotkeyName, "Alt + ");
                        
                        char keyName[32] = {0};
                        LONG scanCode = MapVirtualKeyA(vkey, MAPVK_VK_TO_VSC) << 16;
                        switch (vkey) {
                            case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
                            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
                            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                                scanCode |= 0x01000000; break;
                        }
                        GetKeyNameTextA(scanCode, keyName, sizeof(keyName));
                        lstrcatA(currentHotkeyName, keyName);

                        char btnText[128];
                        wsprintfA(btnText, clicking ? "STOP CLICKING (%s)" : "START CLICKING (%s)", currentHotkeyName);
                        SetWindowTextA(GetDlgItem(hwnd, ID_MAIN_BTN), btnText);

                        char successMsg[128];
                        wsprintfA(successMsg, "Hotkey successfully changed to:\n\n%s", currentHotkeyName);
                        MessageBoxA(hwnd, successMsg, "Hotkey Saved", MB_ICONINFORMATION);
                        SetFocus(hwnd);
                    } else {
                        MessageBoxA(hwnd, "Failed to register hotkey. It might be in use by another application.", "Error", MB_ICONERROR);
                    }
                }
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
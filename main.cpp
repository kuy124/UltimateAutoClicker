#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>

#define IDI_APPICON 101

// Control IDs
enum {
    ID_HRS = 201, ID_MINS, ID_SECS, ID_MS,
    ID_BTN_COMBO, ID_TYPE_COMBO,
    ID_REP_INF, ID_REP_TIMES, ID_REP_COUNT,
    ID_POS_CUR, ID_POS_CUST, ID_POS_X, ID_POS_Y, ID_PICK_BTN,
    ID_HOTKEY_CTRL, ID_SET_HOTKEY,
    ID_TOP_CHECK, ID_MAIN_BTN
};

// Global State
volatile bool running = true;
volatile bool clicking = false;
volatile long intervalMs = 100;
volatile int mouseBtn = 0, clickType = 0, maxClicks = 0, clicksDone = 0;
volatile bool customPos = false;
volatile int targetX = 0, targetY = 0;

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

DWORD WINAPI ClickerThread(LPVOID lpParam) {
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

        INPUT inputs[2] = { 0 };
        inputs[0].type = inputs[1].type = INPUT_MOUSE;
        
        DWORD downFlag = MOUSEEVENTF_LEFTDOWN, upFlag = MOUSEEVENTF_LEFTUP;
        if (mouseBtn == 1) { downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP; }
        else if (mouseBtn == 2) { downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP; }

        inputs[0].mi.dwFlags = downFlag;
        inputs[1].mi.dwFlags = upFlag;
        SendInput(2, inputs, sizeof(INPUT));

        if (clickType == 1) { 
            Sleep(20);
            SendInput(2, inputs, sizeof(INPUT));
        }

        clicksDone++;

        if (!running || !clicking) continue;

        if (intervalMs > 0) {
            WaitForSingleObject(hClickEvent, intervalMs);
        } else {
            SwitchToThread(); 
        }
    }
    return 0;
}

void ToggleClicking() {
    clicking = !clicking;
    char btnText[128]; // Buffer for dynamic button text

    if (clicking) {
        long h = GetDlgItemInt(hMainWnd, ID_HRS, 0, 0);
        long m = GetDlgItemInt(hMainWnd, ID_MINS, 0, 0);
        long s = GetDlgItemInt(hMainWnd, ID_SECS, 0, 0);
        long ms = GetDlgItemInt(hMainWnd, ID_MS, 0, 0);
        intervalMs = ms + (s * 1000) + (m * 60000) + (h * 3600000);
        if (intervalMs < 0) intervalMs = 0; 

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

        // Dynamically insert the hotkey into the Stop text
        wsprintfA(btnText, "STOP CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
        SetEvent(hClickEvent); 
    } else {
        // Dynamically insert the hotkey into the Start text
        wsprintfA(btnText, "START CLICKING (%s)", currentHotkeyName);
        SetWindowTextA(GetDlgItem(hMainWnd, ID_MAIN_BTN), btnText);
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
            AddCtrl("STATIC", "Hours", 0, 25, 33, 40, 20, 0);  AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 65, 30, 35, 20, ID_HRS);
            AddCtrl("STATIC", "Mins", 0, 110, 33, 35, 20, 0);  AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 145, 30, 35, 20, ID_MINS);
            AddCtrl("STATIC", "Secs", 0, 190, 33, 35, 20, 0);  AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 225, 30, 35, 20, ID_SECS);
            AddCtrl("STATIC", "Ms", 0, 270, 33, 25, 20, 0);    AddCtrl("EDIT", "100", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 295, 30, 60, 20, ID_MS);

            AddCtrl("BUTTON", "Click Options", BS_GROUPBOX, 10, 75, 365, 60, 0);
            AddCtrl("STATIC", "Mouse Button:", 0, 25, 100, 90, 20, 0);
            HWND hBtnCombo = AddCtrl("COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 120, 95, 80, 100, ID_BTN_COMBO);
            AddCtrl("STATIC", "Type:", 0, 220, 100, 40, 20, 0);
            HWND hTypeCombo = AddCtrl("COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 260, 95, 95, 100, ID_TYPE_COMBO);
            
            SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Left"); SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Right"); SendMessageA(hBtnCombo, CB_ADDSTRING, 0, (LPARAM)"Middle");
            SendMessageA(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)"Single"); SendMessageA(hTypeCombo, CB_ADDSTRING, 0, (LPARAM)"Double");
            SendMessageA(hBtnCombo, CB_SETCURSEL, 0, 0); SendMessageA(hTypeCombo, CB_SETCURSEL, 0, 0);

            AddCtrl("BUTTON", "Click Repeat", BS_GROUPBOX, 10, 145, 365, 80, 0);
            HWND hRad1 = AddCtrl("BUTTON", "Repeat until stopped", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 25, 165, 150, 20, ID_REP_INF);
            AddCtrl("BUTTON", "Repeat           times", BS_AUTORADIOBUTTON, 25, 195, 150, 20, ID_REP_TIMES);
            AddCtrl("EDIT", "10", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 90, 195, 45, 20, ID_REP_COUNT);
            SendMessage(hRad1, BM_SETCHECK, BST_CHECKED, 0);

            AddCtrl("BUTTON", "Cursor Position", BS_GROUPBOX, 10, 235, 365, 80, 0);
            HWND hPos1 = AddCtrl("BUTTON", "Current Location", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 25, 255, 130, 20, ID_POS_CUR);
            AddCtrl("BUTTON", "Specific Location", BS_AUTORADIOBUTTON, 25, 285, 130, 20, ID_POS_CUST);
            AddCtrl("STATIC", "X:", 0, 155, 285, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 175, 285, 40, 20, ID_POS_X);
            AddCtrl("STATIC", "Y:", 0, 225, 285, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 245, 285, 40, 20, ID_POS_Y);
            AddCtrl("BUTTON", "Pick Location", BS_PUSHBUTTON | WS_TABSTOP, 290, 283, 75, 24, ID_PICK_BTN);
            SendMessage(hPos1, BM_SETCHECK, BST_CHECKED, 0);

            // --- REDESIGNED HOTKEY UI ---
            AddCtrl("BUTTON", "Start / Stop Hotkey", BS_GROUPBOX, 10, 325, 365, 65, 0);
            AddCtrl("STATIC", "1. Press keys:", 0, 25, 355, 90, 20, 0);
            HWND hHotkey = AddCtrl(HOTKEY_CLASSA, NULL, WS_BORDER | WS_TABSTOP, 115, 352, 110, 24, ID_HOTKEY_CTRL);
            SendMessage(hHotkey, HKM_SETHOTKEY, VK_F6, 0);
            AddCtrl("BUTTON", "2. Apply Hotkey", BS_PUSHBUTTON | WS_TABSTOP, 235, 351, 120, 26, ID_SET_HOTKEY);

            AddCtrl("BUTTON", "Always on Top", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 405, 120, 20, ID_TOP_CHECK);
            
            // Start button now shows the hotkey directly inside it!
            AddCtrl("BUTTON", "START CLICKING (F6)", BS_PUSHBUTTON | WS_TABSTOP, 10, 430, 365, 45, ID_MAIN_BTN, hBold);

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
                        
                        // Parse the key combination into readable text
                        currentHotkeyName[0] = '\0';
                        if (mods & HOTKEYF_CONTROL) lstrcatA(currentHotkeyName, "Ctrl + ");
                        if (mods & HOTKEYF_SHIFT) lstrcatA(currentHotkeyName, "Shift + ");
                        if (mods & HOTKEYF_ALT) lstrcatA(currentHotkeyName, "Alt + ");
                        
                        char keyName[32] = {0};
                        LONG scanCode = MapVirtualKeyA(vkey, MAPVK_VK_TO_VSC) << 16;
                        // Handle extended keys (Arrows, Page Up/Down, etc.)
                        switch (vkey) {
                            case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
                            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
                            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                                scanCode |= 0x01000000; break;
                        }
                        GetKeyNameTextA(scanCode, keyName, sizeof(keyName));
                        lstrcatA(currentHotkeyName, keyName);

                        // Update Main Button Text to reflect new hotkey
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
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_DESTROY: { 
            running = false; 
            if (hMouseHook) UnhookWindowsHookEx(hMouseHook);
            UnregisterHotKey(hwnd, 1); 
            if (hFont) DeleteObject(hFont);
            if (hBold) DeleteObject(hBold);
            PostQuitMessage(0); 
            break; 
        }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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

    // Notice we slightly increased the height (525) to perfectly fit the new layout
    HWND hwnd = CreateWindowA("UltimateAutoClicker", "Ultimate AutoClicker", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 525, NULL, NULL, hInstance, NULL);

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
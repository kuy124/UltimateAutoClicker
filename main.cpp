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
    ID_POS_SEQ, ID_SEQ_BTN,
    ID_HOTKEY_LABEL, ID_CHANGE_HOTKEY_BTN,
    ID_TOP_CHECK, ID_MAIN_BTN,
    ID_SEQ_LIST = 401, ID_SEQ_ADD_BTN, ID_SEQ_PICK_BTN, ID_SEQ_DEL_BTN, ID_SEQ_CLR_BTN, ID_SEQ_IN_X, ID_SEQ_IN_Y
};

// Global State
volatile bool running = true;
volatile bool clicking = false;
volatile long long intervalUs = 100000; // Microseconds (100ms default)
volatile int mouseBtn = 0, clickType = 0, maxClicks = 0, clicksDone = 0;
volatile int posMode = 0;
volatile int targetX = 0, targetY = 0;
POINT seqPoints[2048];
volatile int seqCount = 0;
volatile int seqIndex = 0;
HWND hSeqModalWnd = NULL;
bool g_pickingForSeq = false;

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
        if (g_pickingForSeq && hSeqModalWnd) {
            PostMessage(hSeqModalWnd, WM_USER + 3, (WPARAM)pMouse->pt.x, (LPARAM)pMouse->pt.y);
        } else {
            PostMessage(hMainWnd, WM_USER + 2, (WPARAM)pMouse->pt.x, (LPARAM)pMouse->pt.y);
        }
        UnhookWindowsHookEx(hMouseHook);
        hMouseHook = NULL;
        g_pickingForSeq = false;
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

void RefreshSeqList(HWND hList) {
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    char itemBuf[64];
    for (int i = 0; i < seqCount; i++) {
        wsprintfA(itemBuf, "#%d:  (X: %d,  Y: %d)", i + 1, seqPoints[i].x, seqPoints[i].y);
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)itemBuf);
    }
    char btnBuf[64];
    wsprintfA(btnBuf, "Manage Points (%d)...", seqCount);
    SetDlgItemTextA(hMainWnd, ID_SEQ_BTN, btnBuf);
}

// Modal Dialog Procedure for Consecutive Clicks Sequence
LRESULT CALLBACK SeqModalProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hSeqModalWnd = hwnd;
            HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
            HFONT fBold = CreateFontA(14, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");

            HWND hLbl = CreateWindowExA(0, "STATIC", "Consecutive Locations Queue:", WS_VISIBLE | WS_CHILD, 15, 10, 290, 18, hwnd, NULL, NULL, NULL);
            SendMessage(hLbl, WM_SETFONT, (WPARAM)fBold, TRUE);

            HWND hList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL, WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 15, 30, 295, 120, hwnd, (HMENU)ID_SEQ_LIST, NULL, NULL);
            SendMessage(hList, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hPickBtn = CreateWindowExA(0, "BUTTON", "Pick from Screen...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 15, 160, 140, 26, hwnd, (HMENU)ID_SEQ_PICK_BTN, NULL, NULL);
            SendMessage(hPickBtn, WM_SETFONT, (WPARAM)fBold, TRUE);

            HWND hDelBtn = CreateWindowExA(0, "BUTTON", "Delete Selected", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 165, 160, 145, 26, hwnd, (HMENU)ID_SEQ_DEL_BTN, NULL, NULL);
            SendMessage(hDelBtn, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hLblX = CreateWindowExA(0, "STATIC", "X:", WS_VISIBLE | WS_CHILD, 15, 198, 15, 20, hwnd, NULL, NULL, NULL);
            SendMessage(hLblX, WM_SETFONT, (WPARAM)f, TRUE);
            HWND hInX = CreateWindowExA(0, "EDIT", "0", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP, 32, 196, 45, 22, hwnd, (HMENU)ID_SEQ_IN_X, NULL, NULL);
            SendMessage(hInX, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hLblY = CreateWindowExA(0, "STATIC", "Y:", WS_VISIBLE | WS_CHILD, 85, 198, 15, 20, hwnd, NULL, NULL, NULL);
            SendMessage(hLblY, WM_SETFONT, (WPARAM)f, TRUE);
            HWND hInY = CreateWindowExA(0, "EDIT", "0", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP, 102, 196, 45, 22, hwnd, (HMENU)ID_SEQ_IN_Y, NULL, NULL);
            SendMessage(hInY, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hAddBtn = CreateWindowExA(0, "BUTTON", "Add (X, Y)", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 155, 195, 80, 24, hwnd, (HMENU)ID_SEQ_ADD_BTN, NULL, NULL);
            SendMessage(hAddBtn, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hClrBtn = CreateWindowExA(0, "BUTTON", "Clear All", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 242, 195, 68, 24, hwnd, (HMENU)ID_SEQ_CLR_BTN, NULL, NULL);
            SendMessage(hClrBtn, WM_SETFONT, (WPARAM)f, TRUE);

            HWND hDoneBtn = CreateWindowExA(0, "BUTTON", "Done", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP, 105, 238, 115, 28, hwnd, (HMENU)IDOK, NULL, NULL);
            SendMessage(hDoneBtn, WM_SETFONT, (WPARAM)fBold, TRUE);

            RefreshSeqList(hList);
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            HWND hList = GetDlgItem(hwnd, ID_SEQ_LIST);
            if (id == ID_SEQ_PICK_BTN) {
                g_pickingForSeq = true;
                ShowWindow(hwnd, SW_HIDE);
                ShowWindow(hMainWnd, SW_HIDE);
                hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);
            }
            if (id == ID_SEQ_ADD_BTN) {
                if (seqCount < 2048) {
                    seqPoints[seqCount].x = GetDlgItemInt(hwnd, ID_SEQ_IN_X, NULL, TRUE);
                    seqPoints[seqCount].y = GetDlgItemInt(hwnd, ID_SEQ_IN_Y, NULL, TRUE);
                    seqCount = seqCount + 1;
                    RefreshSeqList(hList);
                    SendMessage(GetDlgItem(hMainWnd, ID_POS_SEQ), BM_SETCHECK, BST_CHECKED, 0);
                    SendMessage(GetDlgItem(hMainWnd, ID_POS_CUR), BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessage(GetDlgItem(hMainWnd, ID_POS_CUST), BM_SETCHECK, BST_UNCHECKED, 0);
                }
            }
            if (id == ID_SEQ_DEL_BTN) {
                int sel = SendMessage(hList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR && sel < seqCount) {
                    for (int i = sel; i < seqCount - 1; i++) {
                        seqPoints[i] = seqPoints[i + 1];
                    }
                    seqCount = seqCount - 1;
                    RefreshSeqList(hList);
                }
            }
            if (id == ID_SEQ_CLR_BTN) {
                seqCount = 0;
                RefreshSeqList(hList);
            }
            if (id == IDOK || id == IDCANCEL) {
                hSeqModalWnd = NULL;
                EnableWindow(hMainWnd, TRUE);
                SetForegroundWindow(hMainWnd);
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_USER + 3: {
            if (seqCount < 2048) {
                seqPoints[seqCount].x = (int)wParam;
                seqPoints[seqCount].y = (int)lParam;
                seqCount = seqCount + 1;
                RefreshSeqList(GetDlgItem(hwnd, ID_SEQ_LIST));
                SendMessage(GetDlgItem(hMainWnd, ID_POS_SEQ), BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(GetDlgItem(hMainWnd, ID_POS_CUR), BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessage(GetDlgItem(hMainWnd, ID_POS_CUST), BM_SETCHECK, BST_UNCHECKED, 0);
            }
            ShowWindow(hMainWnd, SW_SHOW);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        }
        case WM_CLOSE: {
            hSeqModalWnd = NULL;
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

// Hardware-synced absolute timestamp wait routine
void WaitUntilQpc(LONGLONG targetQpc, LONGLONG qpf) {
    if (!running || !clicking) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= targetQpc) return;

    LONGLONG remainingTicks = targetQpc - now.QuadPart;
    LONGLONG coarseThreshold = (2000LL * qpf) / 1000000LL;

    if (remainingTicks > coarseThreshold) {
        LONGLONG coarseTicks = remainingTicks - (1500LL * qpf) / 1000000LL;
        if (hTimerHandle) {
            LARGE_INTEGER liDueTime;
            liDueTime.QuadPart = -((coarseTicks * 10000000LL) / qpf);
            SetWaitableTimer(hTimerHandle, &liDueTime, 0, NULL, NULL, 0);

            HANDLE handles[2] = { hTimerHandle, hClickEvent };
            DWORD res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            CancelWaitableTimer(hTimerHandle);
            if (res == WAIT_OBJECT_0 + 1 || !clicking || !running) {
                return;
            }
        } else {
            DWORD coarseMs = (DWORD)((coarseTicks * 1000LL) / qpf);
            if (coarseMs > 0) {
                if (WaitForSingleObject(hClickEvent, coarseMs) != WAIT_TIMEOUT) {
                    return;
                }
            }
        }
    }

    while (running && clicking) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= targetQpc) break;
        YieldProcessor();
    }
}

DWORD WINAPI ClickerThread(LPVOID lpParam) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    DWORD_PTR processAffinity = 0, systemAffinity = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity) && processAffinity) {
        DWORD_PTR primaryCore = processAffinity & (~processAffinity + 1);
        SetThreadAffinityMask(GetCurrentThread(), primaryCore);
    }

    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);

    while (running) {
        if (!clicking) {
            WaitForSingleObject(hClickEvent, INFINITE);
        }
        if (!running) break;

        DWORD downFlag = MOUSEEVENTF_LEFTDOWN, upFlag = MOUSEEVENTF_LEFTUP;
        if (mouseBtn == 1) { downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP; }
        else if (mouseBtn == 2) { downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP; }

        ULONG_PTR extraInfo = GetMessageExtraInfo();

        LARGE_INTEGER qpcStart;
        QueryPerformanceCounter(&qpcStart);
        LONGLONG nextClickQpc = qpcStart.QuadPart;

        while (running && clicking) {
            if (maxClicks > 0 && clicksDone >= maxClicks) {
                clicking = false;
                PostMessage(hMainWnd, WM_USER + 1, 0, 0); 
                break;
            }

            int clickX = 0, clickY = 0;
            bool usePos = false;

            if (posMode == 1) {
                clickX = targetX;
                clickY = targetY;
                usePos = true;
            } else if (posMode == 2 && seqCount > 0) {
                int curIdx = seqIndex % seqCount;
                clickX = seqPoints[curIdx].x;
                clickY = seqPoints[curIdx].y;
                usePos = true;
                seqIndex = (seqIndex + 1) % seqCount;
            }

            INPUT inputs[2] = { 0 };
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwExtraInfo = extraInfo;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwExtraInfo = extraInfo;

            if (usePos) {
                SetCursorPos(clickX, clickY);

                int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
                int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

                LONG normX = (LONG)(((clickX - vLeft) * 65535LL) / (vWidth > 1 ? vWidth - 1 : 1));
                LONG normY = (LONG)(((clickY - vTop) * 65535LL) / (vHeight > 1 ? vHeight - 1 : 1));

                inputs[0].mi.dx = normX;
                inputs[0].mi.dy = normY;
                inputs[0].mi.dwFlags = downFlag | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

                inputs[1].mi.dx = normX;
                inputs[1].mi.dy = normY;
                inputs[1].mi.dwFlags = upFlag | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            } else {
                inputs[0].mi.dx = 0;
                inputs[0].mi.dy = 0;
                inputs[0].mi.dwFlags = downFlag;

                inputs[1].mi.dx = 0;
                inputs[1].mi.dy = 0;
                inputs[1].mi.dwFlags = upFlag;
            }

            long long curInterval = intervalUs;
            LONGLONG intervalTicks = (curInterval * qpf.QuadPart) / 1000000LL;
            LONGLONG clickBaseQpc = nextClickQpc;

            if (curInterval <= 1000) {
                SendInput(2, inputs, sizeof(INPUT));
                if (clickType == 1) {
                    SendInput(2, inputs, sizeof(INPUT));
                }
            } else {
                long long holdUs = (curInterval >= 40000) ? 10000 : (curInterval / 4);
                LONGLONG holdTicks = (holdUs * qpf.QuadPart) / 1000000LL;

                SendInput(1, &inputs[0], sizeof(INPUT));
                if (holdTicks > 0) WaitUntilQpc(clickBaseQpc + holdTicks, qpf.QuadPart);
                SendInput(1, &inputs[1], sizeof(INPUT));

                if (clickType == 1) {
                    long long pauseUs = (curInterval >= 40000) ? 10000 : (curInterval / 4);
                    LONGLONG pauseTicks = (pauseUs * qpf.QuadPart) / 1000000LL;
                    LONGLONG secondDownQpc = clickBaseQpc + holdTicks + pauseTicks;
                    WaitUntilQpc(secondDownQpc, qpf.QuadPart);

                    SendInput(1, &inputs[0], sizeof(INPUT));
                    if (holdTicks > 0) WaitUntilQpc(secondDownQpc + holdTicks, qpf.QuadPart);
                    SendInput(1, &inputs[1], sizeof(INPUT));
                }
            }

            clicksDone = clicksDone + 1;

            if (!running || !clicking) break;

            nextClickQpc += intervalTicks;

            LARGE_INTEGER qpcNow;
            QueryPerformanceCounter(&qpcNow);

            if (nextClickQpc > qpcNow.QuadPart) {
                WaitUntilQpc(nextClickQpc, qpf.QuadPart);
            } else if (intervalTicks > 0 && (qpcNow.QuadPart - nextClickQpc > intervalTicks)) {
                nextClickQpc = qpcNow.QuadPart;
            }
        }
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

        if (SendMessage(GetDlgItem(hMainWnd, ID_POS_CUST), BM_GETCHECK, 0, 0) == BST_CHECKED) {
            posMode = 1;
            targetX = GetDlgItemInt(hMainWnd, ID_POS_X, 0, 1);
            targetY = GetDlgItemInt(hMainWnd, ID_POS_Y, 0, 1);
        } else if (SendMessage(GetDlgItem(hMainWnd, ID_POS_SEQ), BM_GETCHECK, 0, 0) == BST_CHECKED) {
            posMode = 2;
            seqIndex = 0;
        } else {
            posMode = 0;
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

            AddCtrl("BUTTON", "Cursor Position", BS_GROUPBOX, 10, 248, 365, 105, 0);
            HWND hPos1 = AddCtrl("BUTTON", "Current Location", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 25, 268, 130, 20, ID_POS_CUR);
            AddCtrl("BUTTON", "Specific Location", BS_AUTORADIOBUTTON, 25, 295, 120, 20, ID_POS_CUST);
            AddCtrl("STATIC", "X:", 0, 148, 295, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 165, 294, 38, 20, ID_POS_X);
            AddCtrl("STATIC", "Y:", 0, 208, 295, 15, 20, 0); AddCtrl("EDIT", "0", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 223, 294, 38, 20, ID_POS_Y);
            AddCtrl("BUTTON", "Pick Location", BS_PUSHBUTTON | WS_TABSTOP, 266, 292, 100, 24, ID_PICK_BTN);
            AddCtrl("BUTTON", "Consecutive Points", BS_AUTORADIOBUTTON, 25, 323, 145, 20, ID_POS_SEQ);
            AddCtrl("BUTTON", "Manage Points (0)...", BS_PUSHBUTTON | WS_TABSTOP, 175, 321, 191, 25, ID_SEQ_BTN);
            SendMessage(hPos1, BM_SETCHECK, BST_CHECKED, 0);

            AddCtrl("BUTTON", "Start / Stop Hotkey", BS_GROUPBOX, 10, 360, 365, 60, 0);
            AddCtrl("STATIC", "Current Hotkey: F6", 0, 25, 382, 180, 20, ID_HOTKEY_LABEL, hBold);
            AddCtrl("BUTTON", "Change Hotkey...", BS_PUSHBUTTON | WS_TABSTOP, 215, 378, 145, 28, ID_CHANGE_HOTKEY_BTN);

            AddCtrl("BUTTON", "Always on Top", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 427, 120, 20, ID_TOP_CHECK);
            
            AddCtrl("BUTTON", "START CLICKING (F6)", BS_PUSHBUTTON | WS_TABSTOP, 10, 452, 365, 45, ID_MAIN_BTN, hBold);

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
            SendMessage(GetDlgItem(hwnd, ID_POS_SEQ), BM_SETCHECK, BST_UNCHECKED, 0);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);

            if (id == ID_MAIN_BTN) { ToggleClicking(); SetFocus(hwnd); }
            if (id == ID_PICK_BTN) {
                g_pickingForSeq = false;
                ShowWindow(hwnd, SW_HIDE);
                hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);
            }
            if (id == ID_SEQ_BTN) {
                StopClicking();
                EnableWindow(hwnd, FALSE);

                RECT rcMain;
                GetWindowRect(hwnd, &rcMain);
                int modalW = 340, modalH = 320;
                int modalX = rcMain.left + (rcMain.right - rcMain.left - modalW) / 2;
                int modalY = rcMain.top + (rcMain.bottom - rcMain.top - modalH) / 2;

                hSeqModalWnd = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "SeqModalClass", "Manage Consecutive Clicks",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    modalX, modalY, modalW, modalH, hwnd, NULL, GetModuleHandle(NULL), NULL);
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
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *pfnSetProcessDpiAwarenessContext)(HANDLE);
        pfnSetProcessDpiAwarenessContext pSetDpi = (pfnSetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetDpi) {
            pSetDpi((HANDLE)-4);
        } else {
            typedef BOOL (WINAPI *pfnSetProcessDPIAware)(void);
            pfnSetProcessDPIAware pSetDPIAware = (pfnSetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
            if (pSetDPIAware) pSetDPIAware();
        }
    }

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

    // Register Consecutive Sequence Modal Class
    WNDCLASSEXA wcSeq = { 0 };
    wcSeq.cbSize = sizeof(WNDCLASSEXA);
    wcSeq.lpfnWndProc = SeqModalProc;
    wcSeq.hInstance = hInstance;
    wcSeq.lpszClassName = "SeqModalClass";
    wcSeq.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcSeq.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassExA(&wcSeq);

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

    RECT rcWnd = { 0, 0, 385, 515 };
    AdjustWindowRectEx(&rcWnd, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int wndW = rcWnd.right - rcWnd.left;
    int wndH = rcWnd.bottom - rcWnd.top;

    HWND hwnd = CreateWindowA("UltimateAutoClicker", "Ultimate AutoClicker", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, wndW, wndH, NULL, NULL, hInstance, NULL);

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
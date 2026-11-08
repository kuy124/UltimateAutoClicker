// Windows-compatible event records keep the shared engine and its tests unchanged.
#pragma once
#include <cstdint>
#include <cwchar>
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using LONG = int32_t;
constexpr int MOD_ALT = 1, MOD_CONTROL = 2, MOD_SHIFT = 4, MOD_WIN = 8;
constexpr int INPUT_MOUSE = 0, INPUT_KEYBOARD = 1;
constexpr DWORD MOUSEEVENTF_MOVE = 1, MOUSEEVENTF_LEFTDOWN = 2, MOUSEEVENTF_LEFTUP = 4,
    MOUSEEVENTF_RIGHTDOWN = 8, MOUSEEVENTF_RIGHTUP = 16, MOUSEEVENTF_MIDDLEDOWN = 32,
    MOUSEEVENTF_MIDDLEUP = 64, MOUSEEVENTF_WHEEL = 0x800,
    MOUSEEVENTF_VIRTUALDESK = 0x4000, MOUSEEVENTF_ABSOLUTE = 0x8000;
constexpr DWORD KEYEVENTF_KEYUP = 2, KEYEVENTF_UNICODE = 4;
struct INPUT {
    DWORD type = 0;
    struct { LONG dx = 0, dy = 0; DWORD mouseData = 0, dwFlags = 0; uintptr_t dwExtraInfo = 0; } mi;
    struct { WORD wVk = 0, wScan = 0; DWORD dwFlags = 0; uintptr_t dwExtraInfo = 0; } ki;
};
#define swprintf_s std::swprintf
#define swscanf_s std::swscanf

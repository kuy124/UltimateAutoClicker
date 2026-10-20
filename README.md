<div align="center">
  <h1>Ultimate AutoClicker</h1>
  <p>
    <b>An ultra-lightweight, hardware-timed click automation engine built with native Win32 APIs for Windows.</b>
  </p>
</div>

Ultimate AutoClicker is a high-precision, zero-drift mouse automation utility engineered for demanding tasks, game automation, and testing workflows.

Written in pure C++ against raw Windows APIs, it utilizes kernel high-resolution timers, hardware QueryPerformanceCounter scheduling, and Per-Monitor V2 DPI awareness. It consumes **0% CPU when idle**, drives well past **1,000 CPS**, and ships as a single executable of roughly half a megabyte.

<hr>

## Quick Start & Building

### Option A: 1-Click Build Script (`build.bat`)
If you have MinGW-w64 / GCC installed:
1. Double-click `build.bat` in the project directory.
2. The script compiles `resource.rc` and links `UltimateAutoClicker.exe` automatically with aggressive binary optimizations.

### Option B: Manual Command Line Compilation
From your terminal inside the project directory:
```bash
windres resource.rc -O coff -o resource.res
g++ core.cpp main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -lshell32 -static -s -Os -fno-exceptions -fno-rtti -DUNICODE -D_UNICODE
```

> `-DUNICODE -D_UNICODE` matters: without it `DefWindowProc` resolves to the ANSI variant and mangles wide strings.

<hr>

## Core Features & Capabilities

### 1. Zero-Drift QPC Engine (1,000+ CPS)
* **Master Grid Scheduling:** Employs `QueryPerformanceCounter` (QPC) absolute time grid tracking. Successive clicks are anchored to hardware timestamp deadlines, eliminating cumulative delay drift entirely.
* **Single-Core Thread Affinity:** The time-critical worker thread is pinned to the primary processor core, preventing cross-core cache invalidation and QPC timing jitter.
* **Adaptive Syscall Batching:** For sub-millisecond rates, mouse down and up events are batched into a single atomic `SendInput` driver packet, avoiding Windows input queue congestion.

### 2. Start Countdown (click where it matters)
* Press **Start / Stop** and the engine waits the configured **Start delay** (3 seconds by default) before the first click, so you can move the pointer onto the target first.
* The main button counts down (`CANCEL - STARTING IN 3 s`) and pressing it again cancels the countdown.

### 3. Randomised Intervals
* Tick **Random interval** and set a minimum and maximum in milliseconds.
* Every gap is drawn at random inside that range, which keeps the rhythm from looking mechanical.

### 4. Consecutive Multi-Point Click Queue & Record/Replay
* **Dynamic Point Sequence:** Set multiple target coordinates. The engine cycles through each position in sequence.
* **Interactive Screen Picker:** Click **Pick Location** to record exact coordinates directly with your mouse.
* **Per-Point Delays:** Give every point its own delay, or capture a real click sequence with **Record clicks** and replay it with the original timing.
* **Atomic Sub-Pixel Coordinates:** `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` is embedded in the click packets, so clicks land on target coordinates even during high-speed multi-monitor cursor jumps.

### 5. Full High-DPI Scaling Support (Per-Monitor V2)
* Coordinates remain accurate on displays scaled at **125%**, **150%**, **175%**, or **200%+**.
* Window layout dimensions adjust via `AdjustWindowRectEx` so no controls are clipped.

### 6. Smart Interval Auto-Conversion
* Enter decimals or large numbers across **Hours**, **Minutes**, **Seconds**, **Milliseconds**, and **Microseconds ($\mu\text{s}$)** fields.
* Pressing **Enter** inside any box immediately normalizes and distributes values across all unit fields.

### 7. True Multi-Key Global Hotkeys
* A low level keyboard hook (`WH_KEYBOARD_LL`) matches the exact set of held keys, so `F6`, `Ctrl + Shift + Q` and combinations like `X + Z` all work while another application has the focus.

### 8. More Actions Than Clicks
* **Action:** Click, Scroll Up, Scroll Down, or Type Text (types the text box one character per interval).
* **Mode:** Repeat until stopped, Repeat *N* times, or a single click.
* **Type:** Single, Double, or Hold (press and hold the button down until stopped).
* **Hold:** Configurable time the mouse button stays down.

### 9. System Tray, Statistics & Profiles
* Minimise the window to the system tray, or keep it floating with **Always on Top**.
* Live **click count**, **elapsed time** and measured **CPS** while it runs.
* Save the current settings as a named **profile** and load it again later.

<hr>

## How to Use

1. **Set Interval:** Specify your interval across the time boxes and press **Enter** to commit. Use **$\mu\text{s}$** for sub-millisecond intervals.
2. **Set Start Delay:** Choose how many seconds to wait after pressing start (3 by default).
3. **Select Mouse Button & Type:** Choose Left, Right, or Middle button, and Single, Double or Hold.
4. **Set Repeat Limit:** Choose **Repeat until stopped** or set a fixed click count.
5. **Choose Location Mode:**
   * **Current Location:** Clicks wherever your pointer is positioned.
   * **Specific Location:** Clicks a single static $(X, Y)$ coordinate.
   * **Consecutive Points:** Click **Manage Points...** to configure a multi-target queue.
6. **Start / Stop:** Press your global hotkey (default: **F6**) or click the main button. Click **Change Hotkey...** to assign a new hotkey.

<hr>

## Technical Specifications

| Feature | Implementation |
| :--- | :--- |
| **Language** | Native C++ (Win32 / C++20 clean) |
| **Idle CPU Usage** | 0.0% (Kernel Waitable Events) |
| **Active Timing** | Hardware QPC + High-Resolution Hybrid Wait |
| **DPI Awareness** | Per-Monitor V2 (`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`) |
| **Multi-Monitor Support** | Full virtual desktop coordinate normalization |
| **Executable Size** | ~0.45 MB (statically linked, no runtime DLLs) |
| **Dependencies** | Zero external runtime DLLs (Statically linked) |

<hr>

## Tests

```bash
test.bat
```

The suite has two parts:

* **153 engine unit checks** (`tests.exe`) run the click engine on a virtual clock: interval maths, the interval clamp, randomised intervals, drift free scheduling, double clicks, hold mode, sequence playback with per point delays, scrolling, auto typing, the hotkey matcher and the config codec. They also cover the regressions for the endless "repeat N" restart and the zero interval spin lock. The virtual clock keeps the results deterministic.
* **32 GUI checks** (`guitest.exe --selftest`) drive the real window: title, interval conversion, the countdown including cancelling it, the live statistics, the settings file round trip, profiles and the modal dialogs.

<hr>

## Settings & Portability

Everything is stored in `UltimateAutoClicker.ini` next to the executable. Delete that file to reset the app to its defaults. Nothing is written to the registry and no folders are created in AppData.

<hr>

<details>
  <summary><b>License</b> <i>(Click to expand)</i></summary>
  <br>
  <p>This project is open-source and distributed under the <strong>MIT License</strong>.</p>
</details>

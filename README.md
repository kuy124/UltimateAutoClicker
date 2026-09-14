<div align="center">
  <h1>Ultimate AutoClicker</h1>
  <p>
    <b>An ultra-lightweight, hardware-timed click automation engine built with native Win32 APIs for Windows.</b>
  </p>
</div>

Ultimate AutoClicker is a high-precision, zero-drift mouse automation utility engineered for demanding tasks, game automation, and testing workflows. 

Written in pure C++ against raw Windows APIs, it utilizes kernel high-resolution timers, hardware QueryPerformanceCounter scheduling, and Per-Monitor V2 DPI awareness. It consumes **0% CPU when idle**, operates with sub-microsecond timing accuracy at rates exceeding **1,000 to 10,000+ CPS**, and runs on less than 1 MB of RAM.

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
g++ main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -static -s -Os -fno-exceptions -fno-rtti
```

<hr>

## Core Features & Capabilities

### 1. Zero-Drift QPC Engine (1,000+ to 10,000+ CPS)
* **Master Grid Scheduling:** Employs `QueryPerformanceCounter` (QPC) absolute time grid tracking. Successive clicks are anchored to hardware timestamp deadlines, eliminating cumulative delay drift entirely.
* **Single-Core Thread Affinity:** The time-critical worker thread is pinned to the primary processor core, preventing cross-core cache invalidation and QPC timing jitter.
* **Adaptive Syscall Batching:** For sub-millisecond rates ($\le 1\,\text{ms}$ or $\ge 1,000\,\text{CPS}$), mouse down and up events are batched into a single atomic `SendInput` driver packet, avoiding Windows input queue congestion.

### 2. Consecutive Multi-Point Click Queue
* **Dynamic Point Sequence:** Set multiple target coordinates across your screens. The engine cycles through each position in sequence on consecutive clicks.
* **Interactive Screen Picker:** Click **Pick from Screen...** to record exact coordinates directly with your mouse.
* **Manual Input:** Enter precise $X$ and $Y$ coordinates and manage your queue with **Add**, **Delete Selected**, and **Clear All**.
* **Atomic Sub-Pixel Coordinates:** Automatically embeds `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` into click packets, ensuring clicks land on target coordinates even during high-speed multi-monitor cursor jumps.

### 3. Full High-DPI Scaling Support (Per-Monitor V2)
* **100% Consistent Scaling:** Fully declared `PerMonitorV2` DPI awareness via application manifest and runtime API initialization.
* Coordinates remain accurate on displays scaled at **125%**, **150%**, **175%**, or **200%+**.
* Window layout dimensions dynamically adjust via `AdjustWindowRectEx` so no controls are clipped.

### 4. Smart Interval Auto-Conversion
* Enter decimals or large numbers across **Hours**, **Minutes**, **Seconds**, **Milliseconds**, and **Microseconds ($\mu\text{s}$)** fields.
* Pressing **Enter** inside any box immediately normalizes and distributes values across all unit fields.

<hr>

## How to Use

1. **Set Interval:** Specify your interval across the time boxes and press **Enter** to commit. Use **$\mu\text{s}$** for sub-millisecond intervals.
2. **Select Mouse Button & Type:** Choose Left, Right, or Middle button, and Single or Double click.
3. **Set Repeat Limit:** Choose **Repeat until stopped** or set a fixed click count.
4. **Choose Location Mode:**
   * **Current Location:** Clicks wherever your pointer is positioned.
   * **Specific Location:** Clicks a single static $(X, Y)$ coordinate.
   * **Consecutive Points:** Click **Manage Points...** to configure a multi-target queue.
5. **Start / Stop:** Press your global hotkey (default: **F6**) or click the main button. Click **Change Hotkey...** to assign a new hotkey.

<hr>

## Technical Specifications

| Feature | Implementation |
| :--- | :--- |
| **Language** | Native C++ (Win32 / C++20 clean) |
| **Idle CPU Usage** | 0.0% (Kernel Waitable Events) |
| **Active Timing** | Hardware QPC + High-Resolution Hybrid Wait |
| **DPI Awareness** | Per-Monitor V2 (`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`) |
| **Multi-Monitor Support** | Full virtual desktop coordinate normalization |
| **Memory Footprint** | < 1 MB |
| **Dependencies** | Zero external runtime DLLs (Statically linked) |

<hr>

<details>
  <summary><b>License</b> <i>(Click to expand)</i></summary>
  <br>
  <p>This project is open-source and distributed under the <strong>MIT License</strong>.</p>
</details>

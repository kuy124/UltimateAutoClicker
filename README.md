<div align="center">
  <h1>Ultimate AutoClicker</h1>
  <p>
    <b>An ultra-lightweight, high-performance click automation utility built with native Win32 APIs for Windows.</b>
  </p>
</div>

Ultimate AutoClicker is a compact utility for automating mouse clicks, scrolling and typing. Whether you are automating repetitive workflows, testing software or playing games, it gives you precise timing, cursor control and a start delay so you can move the mouse into position before the clicking begins.

To keep the footprint tiny, the application is written entirely in native C++ using raw Win32 APIs. It uses an event-driven thread system combined with Windows kernel high-resolution timers, so it consumes **0% CPU while idle** and the compiled executable stays around half a megabyte.

<hr>

## Quick Start

### Option A: Download the pre-compiled binary
1. Go to the **Release** section of this repository.
2. Download the latest `UltimateAutoClicker.exe`.
3. Put it anywhere on your computer and run it. No installation required.

### Option B: Compile from source
Requires MSYS2 / MinGW-w64 (`g++`, `windres`).

```bash
build.bat        # builds UltimateAutoClicker.exe
test.bat         # builds and runs the whole test suite
```

or by hand:

```bash
windres resource.rc -O coff -o resource.res
g++ core.cpp main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -lshell32 -static -s -Os -fno-exceptions -fno-rtti -DUNICODE -D_UNICODE
```

> `-DUNICODE -D_UNICODE` matters: without it `DefWindowProc` resolves to the ANSI variant and mangles wide strings (window titles end up as `U`).

<hr>

## Using it

### 1. Set the click interval
Enter the time between clicks in the **Hrs / Mins / Secs / Ms / µs** boxes and press **Enter** inside a box to convert it across the other boxes.

* `5000` in **Ms** becomes `5` **Secs** and `0` **Ms**.
* `0.1` in **Hrs** becomes `6` **Mins**.
* The line below the boxes shows the effective rate in CPS. Below 5 ms a red warning appears because the target application may start to lag.

### 2. Start delay (countdown)
**Start delay** is the number of seconds between pressing start and the first click. The default is **3 seconds**, which gives you time to move the pointer onto the target before the clicking begins. The button counts down (`CANCEL - STARTING IN 3 s`) and pressing it again cancels the countdown.

### 3. Pick what happens
| Control | Meaning |
| --- | --- |
| **Mouse Button** | Left, Right or Middle |
| **Type** | Single, Double or Hold (press and hold the button down until stopped) |
| **Mode** | Repeat until stopped, Repeat *N* times, or a single click |
| **Action** | Click, Scroll Up, Scroll Down, or Type Text (types the text box one character per interval) |

### 4. Randomisation
Tick **Random interval** and set a minimum and maximum in milliseconds. Every gap is picked at random inside that range, which keeps the rhythm from looking mechanical.

### 5. Cursor position
* **Current Location** – click wherever the pointer is.
* **Specific Location** – fixed coordinates, or use **Pick Location** (the window hides itself, you click the target, press **Esc** to abort).
* **Consecutive Points** – a queue of points that are clicked in order. Open **Manage Points** to add points by hand, give each one its own delay, or hit **Record clicks** to capture a real click sequence and replay it with the original timing.

### 6. Hotkey
**Change Hotkey** opens a recorder. Press the combination you want: a single key (`F6`), a modifier plus a key (`Ctrl + Shift + Q`) or several keys held together (`X + Z`). The hotkey is global and works while another application has the focus.

### 7. Profiles
Type a name in the **Profiles** box and press **Save** to store the current settings as a preset. **Load** and **Delete** manage them.

<hr>

## Key features

* <span style="color:#2980b9"><b>Event-driven efficiency:</b></span> The background thread sleeps on a kernel waitable timer, so nothing spins on the CPU while the clicker is idle.
* <span style="color:#27ae60"><b>Sub-millisecond precision:</b></span> `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` plus `NtSetTimerResolution` allow intervals below one millisecond without freezing the desktop.
* <span style="color:#16a085"><b>True multi-key hotkeys:</b></span> A low level keyboard hook (`WH_KEYBOARD_LL`) matches the exact set of pressed keys, so combinations like `X + Z` really work.
* <span style="color:#e67e22"><b>Start countdown:</b></span> Move the pointer into place, the clicker starts on its own after the configured delay.
* <span style="color:#d35400"><b>Instant stop:</b></span> The stop hotkey interrupts the wait immediately, it does not wait for the interval to expire.
* <span style="color:#8e44ad"><b>Always on top + system tray:</b></span> Float the window above other applications or minimise it to the tray.
* <span style="color:#16a085"><b>Live statistics:</b></span> Click count, elapsed time and measured clicks per second.
* <span style="color:#2980b9"><b>Portable settings:</b></span> Everything is stored in `UltimateAutoClicker.ini` next to the executable. Delete that file to reset the app.

<hr>

## Tests

```bash
test.bat
```

The suite has two parts:

* `tests.exe` – 153 unit checks over the engine: interval maths, the interval clamp, randomised intervals, drift free scheduling, double clicks, hold mode, sequence playback with per point delays, scrolling, auto typing, the hotkey matcher, the config codec and the regressions for the endless "repeat N" restart and the zero interval spin lock. The engine runs on a virtual clock, so the results are deterministic.
* `guitest.exe --selftest` – 32 checks that drive the real window: title, interval conversion, the countdown (including cancelling it), the live statistics, the settings file round trip, profiles and the modal dialogs.

<hr>

<details>
  <summary><b>License</b> <i>(Click to expand)</i></summary>
  <br>
  <p>This project is open-source and distributed under the <strong>MIT License</strong>.</p>
</details>

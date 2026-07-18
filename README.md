<div align="center">
  <h1>Ultimate AutoClicker</h1>
  <p>
    <b>An ultra-lightweight, high-performance click automation utility built with native Win32 APIs for Windows.</b>
  </p>
</div>

Ultimate AutoClicker is a compact utility designed to automate mouse clicking tasks. Whether you are automating repetitive workflows, testing software, or playing games, this utility provides precise timing and cursor control without impacting system performance. 

To maintain the smallest possible resource footprint, the application is written entirely in native C++ using raw Win32 APIs. It uses a modern event-driven thread system, meaning it consumes **0% CPU when idle** and runs on less than 1 MB of RAM. The compiled executable is incredibly tiny, making it highly portable and efficient.

<hr>

## Quick Start & Installation

You can choose to either download a pre-compiled version or compile the utility yourself.

### Option A: Download Pre-compiled Packages
You do not need to compile the code manually. You can download the standalone executable immediately:
1. Go to the **Release** section on this repository page.
2. Download the latest `UltimateAutoClicker.exe` file.
3. Place it anywhere on your computer and run it. No installation is required.

### Option B: Compile from Source
If you prefer to build the program yourself using a lightweight compiler like MSYS2/MinGW-w64:

1. Create a folder named `AutoClicker` on your computer.
2. Save your C++ source code as `main.cpp` inside this folder.
3. Create a resource script named `resource.rc` to bind your custom icon:
   ```rc
   101 ICON "app.ico"
   ```
4. Place your custom icon file in the folder and name it `app.ico`.
5. Open your terminal in that folder and compile the resource file:
   ```bash
   windres resource.rc -O coff -o resource.res
   ```
6. Build the final executable with high-efficiency size optimization:
   ```bash
   g++ main.cpp resource.res -o UltimateAutoClicker.exe -mwindows -lcomctl32 -static -s -Os -fno-exceptions -fno-rtti
   ```

<hr>

## How to Use

Configuring and controlling your automated clicks is managed through a clean, intuitive native interface.

### Step-by-Step Configuration
1. **Set the Click Interval:** Specify the exact timing between clicks using the **Hours**, **Minutes**, **Seconds**, and **Milliseconds** fields. Leaving all fields at `0` activates the unrestricted high-speed mode.
2. **Choose Click Options:** Select which mouse button to emulate (Left, Right, or Middle) and the click type (Single or Double).
3. **Select Click Repeat Limit:** Choose to repeat infinitely until stopped, or specify an exact number of clicks.
4. **Choose Cursor Position:**
   * Choose **Current Location** to click wherever your mouse pointer is currently resting.
   * Choose **Specific Location** to target static coordinates. You can manually enter **X** and **Y** coordinates, or click **Pick Location** to hide the window and capture coordinates automatically on your next click.

### Setting up the Hotkey
To avoid confusion, configuring a global hotkey is split into a simple two-step process:
1. Click the box under **1. Press keys:** and press your desired key combination on your keyboard (e.g., `F6`, `Ctrl + Shift + Q`, or `Alt + Z`).
2. Click **2. Apply Hotkey** to register it. You will receive a confirmation message, and your new hotkey will automatically display inside the main **START CLICKING** button for quick reference.

<hr>

## Understanding Key Features

Ultimate AutoClicker is designed with several low-level optimizations to ensure responsive and reliable performance:

* <span style="color:#2980b9"><b>Event-Driven Efficiency:</b></span> Unlike typical clickers that constantly run CPU-intensive polling loops in the background, this program uses Windows `Event` handles. When clicking is inactive, the background thread is completely asleep, consuming zero processor resources.
* <span style="color:#27ae60"><b>Instant Stop Response:</b></span> If you set a long interval (e.g., 10 seconds) and need to stop clicking immediately, pressing the stop hotkey triggers a thread interrupt. It will stop instantly without waiting for the remainder of the sleep interval to finish.
* <span style="color:#e67e22"><b>High-Speed Yield Mode:</b></span> Setting the click interval to `0ms` switches the program to its fastest speed. Instead of pausing, the background thread utilizes Windows thread-yielding (`SwitchToThread`) to dispatch clicks as fast as your system hardware can process them.
* <span style="color:#8e44ad"><b>Always on Top:</b></span> Checking the **Always on Top** box floats the interface over other windows, ensuring you always have quick visual access to the controls while running other full-screen or windowed programs.

---

## Maintenance & Removal

### Completely Portable
Because the application is entirely portable and does not write to the Windows Registry or create hidden folders in AppData:
* **To Reset:** Simply close the application. Your settings will return to defaults upon your next launch.
* **To Remove:** Right-click the system tray or taskbar icon, select **Close**, and delete the `UltimateAutoClicker.exe` file. No residual files or configuration leftovers will remain on your computer.

<hr>

<details>
  <summary><b>License</b> <i>(Click to expand)</i></summary>
  <br>
  <p>This project is open-source and distributed under the <strong>MIT License</strong>.</p>
</details>
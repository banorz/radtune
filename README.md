# RadTune (ADLX)

A lightweight command-line tool to tune AMD GPU parameters using the AMD ADLX SDK. It allows for quick overclocking, undervolting, and power limit adjustments, as well as loading tuning profiles from XML files.

## Features

- **Query GPU Info**: List all detected AMD GPUs, their VRAM size, type, and current tuning settings.
- **Manual Tuning**: 
  - **GFX**: Set Min/Max Core Frequency (MHz) and Voltage (mV).
  - **VRAM**: Set Max Memory Frequency (MHz).
  - **Power**: Adjust Power Limit percentage.
  - **Fan**: Toggle Zero RPM mode.
- **XML Profile Support**: Load and apply custom tuning profiles (`.xml`) exported from AMD Software: Adrenalin Edition, with specialized mapping for undervolting and power limits.
- **Modern ADLX Integration**: Uses the latest ADLX SDK for compatibility with RDNA 2 (Navi 2x) and RDNA 3 (Navi 3x) architectures.

## Prerequisites

- **Windows 10/11**
- **AMD Radeon GPU** (Navi 2x/3x recommended for full feature support).
- **AMD ADLX SDK** (included in `adlx_sdk/` directory).
- **CMake** (v3.10+)
- **Visual Studio 2022** (with C++ development workload).

## How to Build

### Using the provided batch script
Simply run:
```bash
build.bat
```

### Manual Build
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Usage

### 1. List GPUs and Current Settings
```bash
RadTune.exe -list
```

### 2. Apply Manual Tuning
You can combine multiple parameters in a single command.
```bash
# Available parameters:
# gpu=      (Index of the GPU, default 0)
# core=     (Max GPU Frequency in MHz)
# coremin=  (Min GPU Frequency in MHz)
# volt=     (GPU Voltage/Offset in mV)
# vram=     (Max VRAM Frequency in MHz)
# power=    (Power Limit percentage, e.g. 15 for +15%)
# zerorpm=  (0 to disable, 1 to enable)

RadTune.exe -set gpu=0 core=2500 coremin=2100 volt=1050 vram=2100 power=15 zerorpm=1
```

### 3. Load Tuning Profile from XML
```bash
# Loads undervolt (ID 12) and power limit (ID 3) from an exported AMD profile
RadTune.exe -load "C:\path\to\performance_profile.xml" [gpu=N]
```

### 4. Automatic Scheduling (Task Scheduler)
Register RadTune to re-apply your tuning automatically — no manual Task Scheduler clicking. The task is created with **highest privileges** (required by ADLX) for you.

```bash
# Re-apply manual settings at every logon
RadTune.exe -schedule logon -set gpu=0 core=2500 volt=1050

# Apply a profile at system startup
RadTune.exe -schedule startup -load "C:\path\to\profile.xml"

# Apply every day at 09:00
RadTune.exe -schedule daily=09:00 -set core=2500 power=15

# Inspect or remove the scheduled task
RadTune.exe -schedule status
RadTune.exe -schedule remove
```

> Run the `-schedule` command **once from an elevated (Administrator) console** — creating a task that runs with highest privileges requires admin rights. After that, the task fires automatically with no further prompts.

## Why RadTune? (Solving Adrenalin Resets)

One common issue with the official AMD Adrenalin software is that tuning settings (overclocking/undervolting) often reset after a reboot, system crash, or even a simple driver timeout. 

**RadTune** provides a reliable way to force your preferred settings:
- **Persistence**: By using a CLI tool, you can ensure your settings are applied exactly as defined, without relying on the Adrenalin UI state.
- **Automation**: The built-in `-schedule` verb (see [Automatic Scheduling](#4-automatic-scheduling-task-scheduler)) wires RadTune into **Windows Task Scheduler** for you — at logon, at startup, or on a daily timer.

### Automating with Task Scheduler
The recommended way is the one-line `-schedule` command documented above — it creates the task with highest privileges automatically. If you prefer to configure it by hand:

1. Open **Task Scheduler** and click **Create Basic Task**.
2. **Trigger**: Select "When I log on".
3. **Action**: Select "Start a program".
4. **Program/script**: Path to `RadTune.exe`.
5. **Add arguments**: `-set gpu=0 core=2500 volt=1100 ...` (or `-load "your_profile.xml"`).
6. **Finish**: In the task properties, ensure **"Run with highest privileges"** is checked (required for ADLX tuning).

---

## Project Structure

- `src/`: Source code (`.cpp`) and headers (`.h`).
- `adlx_sdk/`: The AMD ADLX SDK files.
- `CMakeLists.txt`: Build configuration.
- `build.bat`: Windows one-click build script.
- `.gitignore`: Excludes build artifacts and local settings.

## License

This project is for educational/demonstration purposes. AMD ADLX SDK is subject to AMD's license terms.

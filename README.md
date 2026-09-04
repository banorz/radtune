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
- **AMD ADLX SDK** — vendored as a pinned git submodule in `adlx_sdk/` (ADLX V1.4). Fetch it with `git submodule update --init` (see below).
- **CMake** (v3.10+)
- **Visual Studio 2022** (with C++ development workload).

## How to Build

### Getting the source
The ADLX SDK is a **pinned git submodule** (`adlx_sdk/`), so clone with submodules:
```bash
git clone --recurse-submodules https://github.com/banorz/radtune.git
```
Already cloned without it? Pull the SDK in:
```bash
git submodule update --init
```

### Using the provided batch script
Simply run:
```bash
build.bat
```
By default CMake auto-detects the newest Visual Studio installed (2022, 2026, …). To force a specific version, pass the generator name:
```bash
build.bat "Visual Studio 18 2026"
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
It also prints the **allowed range** for each tunable and the memory-timing
presets your card actually supports, so you don't have to guess values:
```
 [GFX]         Core min: 0 MHz     Core max offset: 0 MHz     Voltage offset: -50 mV
 [GFX range]   core=[-500 .. 1000] volt=[-200 .. 0]
 [VRAM]        Max Frequency: 2518 MHz    vram=[2518 .. 3000]
 [VRAM Timing] Preset: default   Supported: default, fast
 [Power]       Power Limit: +10%        power=[-30 .. 10]
```

### 2. Apply Manual Tuning
You can combine multiple parameters in a single command.
```bash
# Available parameters:
# gpu=      (Index of the GPU, default 0)
# core=     (Max GPU clock OFFSET in MHz - signed, 0 = stock. NOT an absolute clock)
# coremin=  (Min GPU Frequency in MHz)
# volt=     (GPU voltage OFFSET in mV - negative = undervolt)
# vram=     (Max VRAM Frequency in MHz - absolute, not an offset)
# memtiming=(VRAM memory timing preset: default|fast|fast2|auto|level1|level2)
# power=    (Power Limit percentage, e.g. 15 for +15%)
# zerorpm=  (0 to disable, 1 to enable)

RadTune.exe -set gpu=0 core=-100 coremin=2100 volt=-50 vram=2600 memtiming=fast power=10 zerorpm=1
```
Only the parameters you pass are touched; everything else is left alone.
Run `-list` first to see the valid range for each one.

### 2b. Live Telemetry
Reads the *real* current values (the manual tuning above only exposes the
offset, not the resulting clock). No administrator rights needed.
```bash
RadTune.exe -monitor gpu=0
# gpuclock=227  vramclock=132  temp=53  hotspot=55  fan=961
# boardpower=22  voltage=715  usage=12  vramused=3316
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

### 5. GUI (RadTuneGUI.exe)
Prefer not to type commands? `RadTuneGUI.exe` is a small native frontend for the CLI. It builds the same `-set` / `-load` / `-schedule` commands from a form and runs `RadTune.exe` for you, showing the output.

- Keep `RadTuneGUI.exe` **next to** `RadTune.exe` (both land in `build/Release/`); the GUI looks for the CLI in its own folder.
- It requests administrator rights on launch, so the tuning it triggers has the privileges ADLX needs.
- **Apply now** runs the tuning immediately; **Create schedule** registers the Task Scheduler entry with the chosen trigger (logon / startup / daily); **Show status** / **Remove schedule** manage it.
- It **reads the card at startup** — the form shows the GPU's real values without pressing anything. **Read from GPU** re-reads on demand.
- The **GPU dropdown** lists your actual cards; the **VRAM mem timing** dropdown lists only the presets *your* card supports.
- The **Live** tab shows telemetry refreshed once a second (streamed from a single `RadTune -monitor watch=1000` process, started when you open the tab and stopped when you leave it).
- Results appear in a **dialog**, with an error icon when something was rejected.
- The form **remembers your last settings** between runs (stored under `HKCU\Software\RadTune`).

The GUI is a thin wrapper — the CLI remains the engine and is fully usable on its own.

The CLI also exposes machine-readable queries used by the GUI (these omit the
banner when the output is not a terminal):
```bash
RadTune.exe -gpus                          # gpu0=AMD Radeon RX 9070 XT
RadTune.exe -get [gpu=N]                   # current tuning (core=, volt=, memtimingsupported=, ...)
RadTune.exe -monitor [gpu=N]               # one telemetry sample
RadTune.exe -monitor [gpu=N] watch=1000    # stream a sample every second
```

## Exit Codes

RadTune returns **0 only when the requested work actually succeeded**, so
scheduled tasks and scripts can detect failures:

| Code | Meaning |
|------|---------|
| `0`  | Success (or nothing to do) |
| `1`  | A setting failed or was rejected, bad argument, GPU index out of range, unreadable profile, unknown verb, or ADLX initialization failure |

If a `-set` reports `[!] N setting(s) failed`, the value was outside the range
your card accepts — check `RadTune.exe -list`.

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

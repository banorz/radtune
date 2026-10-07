# Architecture

## Stack

- **Language**: C++17.
- **Build**: CMake 3.10+ (Visual Studio 17 2022 generator, x64) via [build.bat](../build.bat) / [CMakeLists.txt](../CMakeLists.txt).
- **GPU control**: AMD **ADLX SDK** (headers under `adlx_sdk/SDK/Include`, not committed — see [deploy.md](deploy.md)).
- **Platform**: Win32 (`User32` linked; ADLX is loaded dynamically from the installed driver).

## Three executables

The build produces **three independent exes** from one CMake project. They must ship side by side (the release zip):

- **`RadTune.exe`** — the CLI engine (needs the ADLX SDK). A **console** program on purpose: cmd/PowerShell wait for it and see its exit code, which scripts depend on.
- **`RadTuneGUI.exe`** — a native Win32 frontend that *shells out* to `RadTune.exe`. It does **not** include or link ADLX, so it builds even without the SDK. See the GUI section below.
- **`RadTuneTask.exe`** — [src/TaskLauncher.cpp](../src/TaskLauncher.cpp), a ~50-line GUI-subsystem launcher that is what the scheduled task actually runs. It starts `RadTune.exe` with `CREATE_NO_WINDOW`, forwards its arguments verbatim and exits with RadTune's exit code (so Task Scheduler's *Last Run Result* still shows failures). Without it, Task Scheduler starting the console program directly flashed a cmd window at every logon (issue #5). `Scheduler::Install` points the task at it when it sits next to `RadTune.exe`, and falls back to `RadTune.exe` with a warning otherwise. Exit codes of its own: `2` = `RadTune.exe` missing, `3` = could not start it.

Architectural rule (keep it): the **CLI is the only thing that talks to ADLX**; the GUI, the task launcher (and any future watchdog) are frontends that build command lines and run `RadTune.exe`.

## Repo layout

```
src/
  main.cpp          # CLI entry point + verb dispatch + tuning read/apply helpers
  ADLXHelper.{h,cpp}# ADLX SDK bootstrap wrapper (g_ADLX: Initialize / GetSystemServices / Terminate)
  WinAPIs.cpp       # Win32 -> ADLX platform abstraction (atomics, LoadLibrary) — from the AMD SDK sample
  ProfileParser.{h,cpp} # hand-rolled string-based parser for Adrenalin-exported XML profiles
  Scheduler.{h,cpp} # Windows Task Scheduler integration (the -schedule verb)
  TaskLauncher.cpp  # RadTuneTask.exe — windowless launcher the scheduled task runs
gui/
  RadTuneGui.cpp    # RadTuneGUI.exe — native Win32 form; builds -set/-load/-get/-schedule and runs RadTune.exe
  RadTuneGui.manifest # requireAdministrator + Common Controls v6
  RadTuneGui.rc     # embeds the manifest + app icon
  RadTune.ico       # multi-size app icon (generated)
adlx_sdk/           # ADLX SDK (NOT in repo — placeholder only)
CMakeLists.txt
build.bat           # one-click build (builds all three exes)
```

## GUI — [gui/RadTuneGui.cpp](../gui/RadTuneGui.cpp)

Plain Win32 + common controls (no ImGui, no .NET — zero extra dependencies). Chosen over Dear ImGui for this frontend precisely because it needs no vendored libraries or graphics backend and builds with the stock VS toolchain.

- Locates `RadTune.exe` next to itself (`GetModuleFileNameW` → same dir).
- Builds a command line from the form (`-set` / `-load` / `-get`, optionally wrapped in `-schedule <trigger>`), runs it via `CreateProcessW` with a redirected stdout pipe (`CREATE_NO_WINDOW`), strips ANSI escapes, and shows the result in a read-only edit box.
- Runs the CLI on a **worker thread** (`StartRun`/`RunWorker`), posting the captured output back to the UI thread via `WM_APP_RESULT` (`OnRunResult`). ADLX init makes `-get`/`-set` take ~1s; running it inline froze the window. While a run is in flight the action buttons are disabled and the output shows "Running…".
- Ships an **elevation manifest** (`requireAdministrator`); the child `RadTune.exe` inherits admin rights, which tuning and highest-privileges scheduling both need.
- Buttons: **Apply now** (`-set`/`-load`), **Read from GPU** (`-get`, prefills the form), **Create schedule** (`-schedule <logon|startup|daily=HH:MM> …`), **Show status**, **Remove schedule**.
- **Remembers the form** between runs in the registry (`HKCU\Software\RadTune`), loaded on open / saved on each action and on close.
- **Three tabs (Tuning | Fan | Live), built from container windows.** `g_pageTuning` / `g_pageFan` share the settings rect; below them `g_actions` is a **shared action bar** (Apply now, Automation, status line) visible on both, so Apply and a schedule always carry Tuning **and** Fan settings — a fan curve left out of the logon task would not survive a reboot. `g_pageLive` spans settings + action bar and is shown alone. Exactly one settings page is visible at a time; the first attempt overlaid pages on their siblings, and overlapping sibling windows repaint over each other (the tuning form showed through Live). Controls live on containers, so `PageProc` forwards `WM_COMMAND`/`WM_DRAWITEM` up to `WndProc`, which keeps every handler in one place (forget to add a case there and the click is silently dropped — that is exactly how Refresh once did nothing).
- **The window is sized from the layout**: `CONTENT_TOP` + `SETTINGS_H` + `ACTIONS_H` give the client height and `AdjustWindowRect` derives the frame. Change a page's content → change those constants, never a hand-guessed frame height.
- **Fields the card may not have are driven by `-get`**: `coremin`, `fanmin`, `fantarget`, `acoustic`, `fancurve` appear only when supported; a missing key clears and disables the field (`g_has*` flags, honoured by `UpdateSourceState`). The fan curve editor is fixed at `CURVE_POINTS` (5) columns and is sent all-or-nothing.
- **The Live tab streams.** Entering it starts one `-monitor watch=1000` child and reads its pipe on a worker thread; leaving it (or closing the window) terminates the child. Samples are split on a blank line — strip `\r` first, since the child's CRT writes text mode and the separator arrives as `\r\n\r\n`.
- **Dropdowns are populated from the CLI, never hardcoded**: GPUs from `-gpus`, memory-timing presets from `-get`'s `memtimingsupported=`. A hardcoded preset list offers values the card refuses.
- **Startup auto-reads** the card: `-gpus` → (chained) `-get`. Deliberately overwrites the registry-restored form, because showing the GPU's real state is the honest default and `Apply now` stays explicit.
- **Results go to a message box**, with the icon chosen from the CLI's exit code; only failures interrupt a read. There is no Output panel — a one-line status label replaces it.
- Look: painted header strip with the app icon + Segoe UI, `GroupBox` sections, Consolas in the telemetry readout. Push buttons are **owner-drawn** (`BS_OWNERDRAW` + `WM_DRAWITEM` → `DrawButton()`) with explicit colours — stock Win11 buttons could render grey-on-grey under dark mode; owner-draw makes the label contrast theme-independent (primary "Apply now" is red). Icon is `gui/RadTune.ico` (multi-size), embedded via `RadTuneGui.rc` (resource id 101) and set as the window/taskbar icon.

## Entry point & dispatch — [main.cpp](../src/main.cpp)

1. Print banner.
2. **`HandleScheduleVerb(argc, argv, exitCode)`** ([main.cpp:26](../src/main.cpp:26)) — intercepts `-schedule` **before** ADLX init and returns early. Registering a Task Scheduler task doesn't need the GPU, so ADLX is never touched for this path. Details in [automation.md](automation.md).
3. `g_ADLX.Initialize()` → get `IADLXGPUTuningServices` + `IADLXGPUList`.
4. Verb dispatch on `argv[1]`:
   - `-list` → `ShowGPUSettings()` per GPU (reads GFX/VRAM/Fan/Power via ADLX).
   - `-get [gpu=N]` → `PrintGpuValues()` — emits current tuning as machine-readable `key=value` lines (`core=`, `coremin=`, `volt=`, `vram=`, `memtiming=`, `power=`, `zerorpm=`).
   - `-monitor [gpu=N] [watch=ms]` → `PrintGpuMetrics()` — emits **live telemetry** as `key=value` (`gpuclock=`, `vramclock=`, `temp=`, `hotspot=`, `fan=`, `power=` (watts), `boardpower=`, `voltage=`, `usage=`, `vramused=`) via `IADLXPerformanceMonitoringServices::GetCurrentGPUMetrics`. Only metrics the driver reports are emitted. Reads only, no elevation needed. With `watch=`, ADLX is initialised **once** and a sample is streamed every N ms, each terminated by a **blank line** — ADLX init costs ~600 ms, so a process-per-sample poll would be both wasteful and permanently that stale.
   - `-gpus` → machine-readable device list (`gpu0=NAME`) for the GUI's GPU dropdown (`-list` is formatted for humans).
- The **ASCII banner is printed only when `stdout` is a terminal** (`_isatty`). Piped output feeds the GUI, scripts and the machine-readable verbs; the banner was noise there and leaked into the GUI's dialogs. Consumed by the GUI's "Read from GPU". **Unlike `-list`, it does NOT gate on `IsSupportedManual*Tuning`** (those can return false on drivers/GPUs where `GetManual*Tuning` still works — same reason `ApplySettings`/`-set` skips the check). Gating it was the bug that made "Read from GPU" print nothing.
   - `-set ...` → parse `key=value` args → `ApplySettings()`.
   - `-load <xml> [gpu=N]` → `ProfileParser::Parse()` → `LoadProfileOnGpu()` → `ApplySettings()`.
5. `g_ADLX.Terminate()`.

## Module responsibilities

- **ADLXHelper** — owns the SDK lifecycle via the global `g_ADLX`. Everything GPU-related goes through the services it exposes.
- **ProfileParser** — parses AMD Adrenalin `*.xml` performance profiles with a manual string scanner (no XML lib). Reads `<GPU DevID RevID>` then repeated `<FEATURE ID Enabled>` blocks each containing `<STATES>/<STATE ID Value Enabled/>`. Result is a `GPUProfile` with `features` keyed by feature ID. See [ProfileParser.cpp](../src/ProfileParser.cpp).
- **Scheduler** — pure Win32 + STL, no ADLX. Generates a Task Scheduler XML and registers it with `schtasks`. See [automation.md](automation.md).

## Tuning value conventions (important, non-obvious)

These live in `ApplySettings()` / the `-set` parser in [main.cpp](../src/main.cpp):

- **Sentinels for "unset"**: frequency fields and `zerorpm` use `-1`; voltage and power use `-999`. A field is only applied when it differs from its sentinel — so partial `-set` commands are safe.
- **Voltage is an OFFSET in mV** (undervolt), not an absolute voltage. Out-of-range offsets make `SetGPUVoltage` return an ADLX error (surfaced to stderr).
- **Power limit is a percentage** (e.g. `power=15` → +15%).
- **VRAM memory timing (`memtiming=`)** is a *preset*, not manual sub-timings — the `ADLX_MEMORYTIMING_DESCRIPTION` enum (Adrenalin's "Memory Timing Control"). CLI accepts names `default|fast|fast2|auto|level1|level2` (or `0`-`5`). Gated by its own `IsSupportedMemoryTiming` (separate from `IsSupportedManualVRAMTuning` — a card can do manual VRAM freq but not timing presets), and read/written on `IADLXManualVRAMTuning2` with a fallback to `...Tuning1`. Not carried by Adrenalin XML profiles, so `-load` never sets it. The enum is a **superset across architectures**: a given GPU exposes only a subset, queried via `GetSupportedMemoryTimingDescriptionList`. `-list` prints that subset (`Supported: …`) and `ApplySettings` rejects a preset not in it with a clear message rather than a raw ADLX error.
- **Core max / voltage: offsets on RDNA4, absolute values on RDNA2/3.** ADLX: *"Start from Navi4+, the maximum frequency is an offset from the base frequency"* (same for voltage). So on an RX 9000 `SetGPUMaxFrequency` takes a signed MHz **offset** (0 = stock) and `SetGPUVoltage` a signed mV offset, while on RX 6000/7000 they take absolute MHz / mV (v1.0's examples: `core=2500 volt=1050`). ADLX has no flag for this; `IsOffsetRange()` reads it off the range — an offset range contains 0, an absolute clock/voltage range never does. `CoreMaxLabel()`/`VoltageLabel()` use it in `-list` and in `-set` messages, and `-get` emits `coremode=`/`voltmode=` (`offset`|`absolute`) so the GUI can label its fields. (1.3.0 hardcoded "offset" everywhere, which was wrong for RDNA2/3.) `SetGPUMinFrequency` and `SetMaxVRAMFrequency` are always absolute MHz. The absolute *base* clock is NOT exposed by the manual-tuning interface (`GetGPUMaxFrequencyDefault` returns 0 on offset-model cards), so the effective absolute clock can only come from live telemetry (`IPerformanceMonitoring`), not from tuning defaults.
- **Tuning params are `std::optional<int>`** end to end (parser → `ApplySettings`): empty = "leave unchanged", present = "apply this exact value". This keeps negative offsets and 0 legal (no magic-sentinel collision). `-load` passes `std::nullopt` for everything it doesn't map.
- **ADLX silently ignores out-of-range values** — `SetGPUMaxFrequency(99999)` returns `ADLX_OK` and changes nothing. Checking the return value is therefore NOT enough to know a setting was applied. `ApplySettings` validates every value against the card's range (`GetGPUMaxFrequencyRange`, `GetGPUMinFrequencyRange`, `GetGPUVoltageRange`, `GetMaxVRAMFrequencyRange`, `GetPowerLimitRange`) via `InRange()` **before** calling `Set*`. When a range can't be read we let ADLX decide rather than block.
- **…and ADLX can apply something other than what it accepted.** On an RX 9060 XT a `core=-500` offset — inside the range the card itself advertises — lands as −400 while `Set` returns OK (issue #11). So every write goes through `ReportVerified()`: after a successful `Set*` the value is **read back**, and a mismatch is reported as `requested X, but the driver applied Y` and counted as a failure (exit 1). If the read-back itself fails we trust the write. The fan curve is verified the same way (`ApplyFanCurve`).
- **`coremin` doesn't exist on RDNA4.** There is no `IsSupported` gate for the minimum clock; RX 9060/9070 XT simply return an unreadable `GetGPUMinFrequencyRange` (and Adrenalin shows no min clock either). `CoreMinSupported()` treats that as unsupported: `-list` shows `n/a`, `-get` omits the key, `-set coremin=` is rejected with an explicit message (issue #10).
- **Fan tuning** lives on the same `IADLXManualFanTuning` as Zero RPM. `ProbeFan()` returns a `FanCaps` from each feature's own gate: `IsSupportedMinFanSpeed` (RPM), `IsSupportedTargetFanSpeed` (RPM), `IsSupportedMinAcousticLimit` (MHz — a GFX clock threshold, not a speed) and the **fan curve** (no gate: usable when `GetFanTuningRanges` and the current states read back). The curve is a fixed number of `(°C, %)` points — 5 on RDNA4, which exposes **only** the curve + Zero RPM. To write it, fill `GetEmptyFanTuningStates`, then check `IsValidFanTuningStates`: it returns **ADLX_OK even for an invalid curve**, the verdict is `errorIndex` (−1 = valid).
- **`-set` validates the command line before touching the GPU.** Unknown keys (a typo used to be silently ignored), non-numeric values (`std::stoi` used to throw and kill the run) and malformed `memtiming`/`fancurve` all print a message and apply **nothing**, exit 1.
- **Tuning writes do NOT need elevation** on current drivers (verified: a non-elevated `-set core=-10` applies and reads back). Only `-schedule` needs an elevated console, because registering a task with `RunLevel=HighestAvailable` does.
- **Exit-code contract**: `main` returns 0 **only** when the requested work succeeded. `ApplySettings`/`LoadProfileOnGpu` return an `ApplyResult{applied, failed}`; any `failed > 0` (driver rejection, unsupported feature/preset, unreadable profile) sets exit 1, as do a bad argument, an out-of-range GPU index and an unknown verb. This exists because the primary consumer is an **unattended** scheduled task — a run that reports success while applying nothing is the worst outcome. Every `Set*` call goes through `Report()`, so no ADLX return value is ignored.
- **Version** lives in `CMakeLists.txt` (`project(RadTune VERSION x.y.z)`) and reaches the code as the `RADTUNE_VERSION` compile definition. Never hardcode it in `main.cpp` again — the banner used to drift from the released tag.
- **Profile feature IDs** used by `-load` ([main.cpp](../src/main.cpp), `LoadProfileOnGpu`): **ID 12 = undervolt** (→ voltage), **ID 3 = power limit** (→ power). Only these two are mapped from XML today; other feature IDs in the profile are ignored.
- **API versioning**: ADLX exposes `ManualGraphicsTuning1` (discrete states) vs `2` (min/max/voltage). The code prefers `...Tuning2` and falls back — reflects RDNA2 vs RDNA3 differences.

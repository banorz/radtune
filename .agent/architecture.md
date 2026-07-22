# Architecture

## Stack

- **Language**: C++17.
- **Build**: CMake 3.10+ (Visual Studio 17 2022 generator, x64) via [build.bat](../build.bat) / [CMakeLists.txt](../CMakeLists.txt).
- **GPU control**: AMD **ADLX SDK** (headers under `adlx_sdk/SDK/Include`, not committed — see [deploy.md](deploy.md)).
- **Platform**: Win32 (`User32` linked; ADLX is loaded dynamically from the installed driver).

## Two executables

The build produces **two independent exes** from one CMake project:

- **`RadTune.exe`** — the CLI engine (needs the ADLX SDK).
- **`RadTuneGUI.exe`** — a native Win32 frontend that *shells out* to `RadTune.exe`. It does **not** include or link ADLX, so it builds even without the SDK. See the GUI section below.

Architectural rule (keep it): the **CLI is the only thing that talks to ADLX**; the GUI (and any future watchdog) are frontends that build command lines and run `RadTune.exe`.

## Repo layout

```
src/
  main.cpp          # CLI entry point + verb dispatch + tuning read/apply helpers
  ADLXHelper.{h,cpp}# ADLX SDK bootstrap wrapper (g_ADLX: Initialize / GetSystemServices / Terminate)
  WinAPIs.cpp       # Win32 -> ADLX platform abstraction (atomics, LoadLibrary) — from the AMD SDK sample
  ProfileParser.{h,cpp} # hand-rolled string-based parser for Adrenalin-exported XML profiles
  Scheduler.{h,cpp} # Windows Task Scheduler integration (the -schedule verb)
gui/
  RadTuneGui.cpp    # RadTuneGUI.exe — native Win32 form; builds -set/-load/-get/-schedule and runs RadTune.exe
  RadTuneGui.manifest # requireAdministrator + Common Controls v6
  RadTuneGui.rc     # embeds the manifest + app icon
  RadTune.ico       # multi-size app icon (generated)
adlx_sdk/           # ADLX SDK (NOT in repo — placeholder only)
CMakeLists.txt
build.bat           # one-click build (builds both exes)
```

## GUI — [gui/RadTuneGui.cpp](../gui/RadTuneGui.cpp)

Plain Win32 + common controls (no ImGui, no .NET — zero extra dependencies). Chosen over Dear ImGui for this frontend precisely because it needs no vendored libraries or graphics backend and builds with the stock VS toolchain.

- Locates `RadTune.exe` next to itself (`GetModuleFileNameW` → same dir).
- Builds a command line from the form (`-set` / `-load` / `-get`, optionally wrapped in `-schedule <trigger>`), runs it via `CreateProcessW` with a redirected stdout pipe (`CREATE_NO_WINDOW`), strips ANSI escapes, and shows the result in a read-only edit box.
- Runs the CLI on a **worker thread** (`StartRun`/`RunWorker`), posting the captured output back to the UI thread via `WM_APP_RESULT` (`OnRunResult`). ADLX init makes `-get`/`-set` take ~1s; running it inline froze the window. While a run is in flight the action buttons are disabled and the output shows "Running…".
- Ships an **elevation manifest** (`requireAdministrator`); the child `RadTune.exe` inherits admin rights, which tuning and highest-privileges scheduling both need.
- Buttons: **Apply now** (`-set`/`-load`), **Read from GPU** (`-get`, prefills the form), **Create schedule** (`-schedule <logon|startup|daily=HH:MM> …`), **Show status**, **Remove schedule**.
- **Remembers the form** between runs in the registry (`HKCU\Software\RadTune`), loaded on open / saved on each action and on close.
- Look: painted header strip with the app icon + Segoe UI, `GroupBox` sections (Tuning / Automation / Output), Consolas in the output box. Push buttons are **owner-drawn** (`BS_OWNERDRAW` + `WM_DRAWITEM` → `DrawButton()`) with explicit colours — stock Win11 buttons could render grey-on-grey under dark mode; owner-draw makes the label contrast theme-independent (primary "Apply now" is red). Icon is `gui/RadTune.ico` (multi-size), embedded via `RadTuneGui.rc` (resource id 101) and set as the window/taskbar icon.

## Entry point & dispatch — [main.cpp](../src/main.cpp)

1. Print banner.
2. **`HandleScheduleVerb(argc, argv, exitCode)`** ([main.cpp:26](../src/main.cpp:26)) — intercepts `-schedule` **before** ADLX init and returns early. Registering a Task Scheduler task doesn't need the GPU, so ADLX is never touched for this path. Details in [automation.md](automation.md).
3. `g_ADLX.Initialize()` → get `IADLXGPUTuningServices` + `IADLXGPUList`.
4. Verb dispatch on `argv[1]`:
   - `-list` → `ShowGPUSettings()` per GPU (reads GFX/VRAM/Fan/Power via ADLX).
   - `-get [gpu=N]` → `PrintGpuValues()` — emits current tuning as machine-readable `key=value` lines (`core=`, `coremin=`, `volt=`, `vram=`, `memtiming=`, `power=`, `zerorpm=`). Consumed by the GUI's "Read from GPU". **Unlike `-list`, it does NOT gate on `IsSupportedManual*Tuning`** (those can return false on drivers/GPUs where `GetManual*Tuning` still works — same reason `ApplySettings`/`-set` skips the check). Gating it was the bug that made "Read from GPU" print nothing.
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
- **Core/voltage tuning are offsets, not absolutes (RDNA4).** `SetGPUMaxFrequency` is a signed MHz **offset** (0 = stock; ADLX doc: "maximum frequency (offset)"), `SetGPUVoltage` a signed mV offset. `SetGPUMinFrequency` and `SetMaxVRAMFrequency` are absolute MHz values. `-list`/GUI label the offset fields accordingly ("Core max offset", "Voltage offset"). The absolute *base* clock is NOT exposed by the manual-tuning interface (`GetGPUMaxFrequencyDefault` returns 0 on offset-model cards), so the effective absolute clock can only come from live telemetry (`IPerformanceMonitoring`), not from tuning defaults.
- **Tuning params are `std::optional<int>`** end to end (parser → `ApplySettings`): empty = "leave unchanged", present = "apply this exact value". This keeps negative offsets and 0 legal (no magic-sentinel collision). `-load` passes `std::nullopt` for everything it doesn't map.
- **Profile feature IDs** used by `-load` ([main.cpp](../src/main.cpp), `LoadProfileOnGpu`): **ID 12 = undervolt** (→ voltage), **ID 3 = power limit** (→ power). Only these two are mapped from XML today; other feature IDs in the profile are ignored.
- **API versioning**: ADLX exposes `ManualGraphicsTuning1` (discrete states) vs `2` (min/max/voltage). The code prefers `...Tuning2` and falls back — reflects RDNA2 vs RDNA3 differences.

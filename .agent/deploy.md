# Build & distribution

This is a locally-built desktop tool — there is no server deploy. "Deploy" here means: build the exe, and the non-obvious things around getting it to run.

## Prerequisites

- **Windows 10/11**.
- **AMD Radeon GPU** with Adrenalin driver installed (Navi 2x / Navi 3x recommended for full manual-tuning support). ADLX ships *with the driver* — there is no separate runtime to install or bundle.
- **Visual Studio 2022** with the "Desktop development with C++" workload.
- **CMake 3.10+**.
- **AMD ADLX SDK** — see the gotcha below.

## Gotcha #1 — the ADLX SDK is a git submodule

`adlx_sdk/` is a **git submodule** pinned to the official GPUOpen ADLX repo
(`https://github.com/GPUOpen-LibrariesAndSDKs/ADLX.git`) at **V1.4** (commit
`36d37dab`). It is NOT copied into this repo's history — the build resolves
`ADLX_INCLUDE_DIR = adlx_sdk/SDK/Include` from the checked-out submodule.

Fetch it before building:

```bash
git clone --recurse-submodules https://github.com/banorz/radtune.git   # fresh clone
# or, in an existing clone:
git submodule update --init
```

If the build fails on `#include "IGPUManual*.h"` or `ADLXHelper.h` dependencies,
the submodule isn't checked out — run the `submodule update` above.

**Why a submodule** (not vendored): the ADLX SDK is under AMD's own licence
agreement (`adlx_sdk/ADLX SDK License Agreement.pdf`), not a permissive one, so
we don't redistribute its files here. The submodule pins the exact version for
reproducible builds while users fetch the SDK from AMD directly. To bump it:
`cd adlx_sdk && git checkout <new-tag> && cd .. && git add adlx_sdk && git commit`.

## Build

One-click:

```bash
build.bat
```

By default it lets CMake **auto-detect the newest Visual Studio installed** (works with VS 2022, 2026, …). To force a specific one, pass the generator name:

```bash
build.bat "Visual Studio 18 2026"
build.bat "Visual Studio 17 2022"
```

It runs (from [build.bat](../build.bat)):

```bash
cmake .. -A x64            REM no -G -> newest VS; or -G "<generator>" when overridden
cmake --build . --config Release
```

Output: **`build/Release/RadTune.exe`** and **`RadTuneGUI.exe`**. Switching VS versions on an existing `build/` fails on the cached generator — delete `build/` and rerun.

Manual equivalent:

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Gotcha #2 — elevation is required at runtime

ADLX **write** operations (`-set`, `-load`) and creating the scheduled task (`-schedule`) need **Administrator** rights. Read-only `-list` works unelevated. When automating, the `-schedule` verb creates the task with `RunLevel=HighestAvailable` so the scheduled run is elevated automatically — but you must run the `-schedule` command itself once from an elevated console. See [automation.md](automation.md).

## Gotcha #3 — ANSI color output

`main.cpp` prints ANSI escape codes (`\033[1;3xm`) for the banner and status. These render correctly in Windows Terminal and modern conhost (VT processing on by default). In a very old console host the raw codes may show as garbage — not a bug, a terminal capability issue.

## Distribution

Ship just `RadTune.exe`. No DLLs to bundle: ADLX is resolved from the installed AMD driver at runtime (`WinAPIs.cpp` does the dynamic load). There is currently no CI, code signing, or installer.

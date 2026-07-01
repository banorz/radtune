# RadTune — agent entry point

C++17 CLI tool built on the **AMD ADLX SDK** that tunes Radeon GPUs (overclock / undervolt / power limit / fan) and **forces those settings to persist** against AMD Adrenalin resets. Windows-only.

## Index

- [architecture.md](architecture.md) — stack, repo layout, CLI dispatch flow, module responsibilities, tuning value conventions.
- [deploy.md](deploy.md) — build & distribution, prerequisites, the ADLX SDK gotcha, runtime/elevation gotchas.
- [automation.md](automation.md) — the domain: why RadTune exists (the reset problem), the `-schedule` verb internals, and the automation roadmap (GUI, anti-reset watchdog).

## Quick facts

- **Branch strategy**: `main` is the default/release branch; work happens on feature branches (e.g. `claude/*`), merged via PR to `origin` (github.com/banorz/radtune).
- **No env/secrets**: it's a local desktop tool. Nothing to configure, no `.env`, no credentials.
- **The ADLX SDK is NOT in the repo** — `adlx_sdk/` only holds a placeholder. You must download the SDK and place its headers under `adlx_sdk/SDK/Include` before the build resolves. See [deploy.md](deploy.md).
- **Tuning requires elevation**: ADLX write calls (and creating the scheduled task) need Administrator rights.
- **Single dimension of value**: persistence. The tuning itself is a thin wrapper over ADLX; the differentiator is forcing settings to survive reboots/crashes/driver TDR.

## Most frequent commands

```bash
build.bat                                            # configure + build (VS 2022, x64) -> build/Release/{RadTune,RadTuneGUI}.exe

RadTuneGUI.exe                                       # optional GUI frontend (shells out to RadTune.exe next to it)
RadTune -list                                        # list GPUs + current tuning
RadTune -set gpu=0 core=2500 coremin=2100 volt=1050 vram=2100 power=15 zerorpm=1
RadTune -load "C:\profile.xml" [gpu=N]               # apply an Adrenalin-exported XML profile
RadTune -schedule logon -set core=2500 volt=1050     # auto-apply via Task Scheduler (run elevated once)
RadTune -schedule status | remove
```

## Troubleshooting flowchart

When the user reports X, check in this order:

- **"ADLX Initialization failed"** → 1) not an AMD GPU / driver too old (needs Adrenalin installed — ADLX ships with the driver); 2) not running elevated; 3) Navi 2x/3x recommended, older archs may not support manual tuning.
- **"Failed to set Voltage / Power Limit" (non-zero ADLX error)** → the requested value is out of the card's allowed range. Voltage is an **offset in mV**, not absolute. See conventions in [architecture.md](architecture.md).
- **Build fails on `#include "IGPUManual*.h"`** → `adlx_sdk/SDK/Include` is empty; the SDK wasn't downloaded. See [deploy.md](deploy.md).
- **`-schedule` fails ("schtasks failed / access denied")** → not run from an elevated console; creating a highest-privileges task needs admin. See [automation.md](automation.md).
- **Settings reset after a reboot / mid-game crash** → this is the core problem RadTune addresses; point the user to `-schedule` (logon/startup) and the planned watchdog in [automation.md](automation.md).

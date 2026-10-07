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
build.bat                                            # configure + build (VS, x64) -> build/Release/{RadTune,RadTuneGUI,RadTuneTask}.exe

RadTuneGUI.exe                                       # optional GUI frontend (shells out to RadTune.exe next to it)
RadTune -list                                        # GPUs, current tuning, valid RANGES and what the card supports
RadTune -set gpu=0 core=-100 volt=-50 vram=2600 memtiming=fast power=10 zerorpm=1
RadTune -set fancurve=30:15,50:32,62:49,74:66,83:100 # fan curve, temp C : speed % (RDNA4: 5 points)
RadTune -monitor gpu=0 [watch=1000]                  # live telemetry (real clocks/temps/fan/power)
RadTune -load "C:\profile.xml" [gpu=N]               # apply an Adrenalin-exported XML profile
RadTune -schedule logon -set core=-100 volt=-50      # auto-apply via Task Scheduler (run elevated once)
RadTune -schedule status | remove
```

## Troubleshooting flowchart

When the user reports X, check in this order:

- **"ADLX Initialization failed"** → 1) not an AMD GPU / driver too old (needs Adrenalin installed — ADLX ships with the driver); 2) not running elevated; 3) Navi 2x/3x recommended, older archs may not support manual tuning.
- **"… is out of range [a .. b] for this GPU"** → the value is outside what the card advertises; `RadTune -list` prints every range. `core` and `volt` are **signed offsets** on RDNA4, not absolute clocks/voltages. See conventions in [architecture.md](architecture.md).
- **"requested X, but the driver applied Y"** → the driver accepted the write but clamped it (known: RX 9060 XT core offset −500 → −400, issue #11). Not a RadTune bug; the value the card runs is Y.
- **"Core min is not supported on this GPU"** / a disabled Core min field → RDNA4 has no minimum core clock (issue #10). Remove `coremin=` from old commands and scheduled tasks.
- **Fan settings missing or disabled** → the card doesn't expose them. RDNA4 offers only the **fan curve** + Zero RPM. Min/target speed and acoustic limit are enabled only where ADLX reports them as supported (which generations do is not documented by the SDK and has not been verified). `-list` shows what's available.
- **A console window flashes at logon** → the task was created before 1.4 or `RadTuneTask.exe` isn't next to `RadTune.exe`. Recreate the task with `-schedule` from the full release folder.
- **"Nothing applied - fix the arguments above"** → a typo or bad value in `-set`; RadTune now refuses the whole command instead of silently skipping the unknown part.
- **Build fails on `#include "IGPUManual*.h"`** → `adlx_sdk/SDK/Include` is empty; the SDK wasn't downloaded. See [deploy.md](deploy.md).
- **`-schedule` fails ("schtasks failed / access denied")** → not run from an elevated console; creating a highest-privileges task needs admin. See [automation.md](automation.md).
- **Settings reset after a reboot / mid-game crash** → this is the core problem RadTune addresses; point the user to `-schedule` (logon/startup) and the planned watchdog in [automation.md](automation.md).

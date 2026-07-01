# Automation & persistence (the domain)

This is the least-obvious dimension of the project and its active line of work: not *how* to tune the GPU, but how to make tuning **stick**.

## The problem RadTune exists to solve

AMD Adrenalin frequently loses manual tuning after a reboot, a system crash, or a **driver timeout (TDR)**. RadTune re-applies settings deterministically from the CLI, independent of the Adrenalin UI state. Persistence is the product; the ADLX tuning is just the mechanism.

Key insight for prioritisation: a *logon/startup* task only fixes the **reboot** case. The **mid-session driver TDR** — settings vanish while you're gaming, hours after login — is NOT covered by a one-shot scheduled task. That case is what the planned watchdog targets (see Roadmap).

## The `-schedule` verb — [Scheduler.cpp](../src/Scheduler.cpp)

Registers a Windows Task Scheduler task named `RadTune` that re-runs a chosen apply command automatically.

```bash
RadTune -schedule logon      -set gpu=0 core=2500 volt=1050
RadTune -schedule startup    -load "C:\profile.xml"
RadTune -schedule daily=09:00 -set core=2500 power=15
RadTune -schedule status     # schtasks /query
RadTune -schedule remove     # schtasks /delete
```

How it works:

1. Handled in `HandleScheduleVerb()` ([main.cpp:26](../src/main.cpp:26)) **before ADLX init** — no GPU needed to register a task.
2. `Scheduler::Install()` builds a **Task Scheduler XML** (trigger + `Principal RunLevel=HighestAvailable` + the exe path from `GetModuleFileNameA` + the re-serialized payload args), writes it to `%TEMP%\radtune_task.xml`, then runs `schtasks /create /tn "RadTune" /xml <file> /f`.
3. Payload must start with `-set` or `-load`; args containing spaces are re-quoted so they survive as one token when the task fires.

**Design choice — XML import over inline `schtasks` args**: passing the command inline forces fragile double-quoting for paths like `C:\Program Files\...`. Generating the XML sidesteps the quoting entirely and gives clean control over `RunLevel=HighestAvailable` (the "Run with highest privileges" checkbox the README used to require by hand).

**Gotcha**: creating a highest-privileges task needs an **elevated** console. On `Access Denied`, `Install()` returns an error telling the user to run as Administrator. Once created, the task fires with no further prompts.

Supported triggers today: `logon`, `startup` (BootTrigger), `daily=HH:MM`. Only static triggers — dynamic ones (game launch, temperature) were explicitly out of scope for this iteration.

## Roadmap

The agreed direction — **the CLI stays the engine; everything else is a frontend that shells out to `RadTune.exe`.** Do not fuse ADLX logic into the GUI or watchdog.

1. **`-schedule` CLI verb** — done (this file).
2. **GUI** — builds the tuning config (replacing hand-typed `core=... volt=...`, ideally persisted to a `radtune.json`) and drives `-schedule`. Recommended stack: Dear ImGui (native, single-exe, matches the "lightweight" identity). C# WinForms is faster to build and has a great Task Scheduler API but splits the codebase across two languages.
3. **Anti-reset watchdog** (highest differentiation) — a resident process launched at logon *via the same `-schedule`*. Loop: read current ADLX values (same calls as `ShowGPUSettings()`), compare against the desired `radtune.json`, and re-apply when a **reset signature** is detected (all values back to default/0) — not on every deviation, otherwise it fights the user's intentional Adrenalin changes. Optionally react instantly via ADLX tuning-changed events (`IADLXGPUTuningChangedListener`) or by watching the `amdkmdag` TDR entry in the System event log, instead of waiting for the poll tick.

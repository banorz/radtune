# Changelog

All notable changes to RadTune are documented here.
This project adheres to [Semantic Versioning](https://semver.org/).

## [1.3.0] - 2026-09-04

Tuning correctness & visibility release. RadTune gains VRAM memory timing
control and live telemetry, stops silently swallowing failures, and shows the
valid range for every tunable so values no longer have to be guessed.

### Added
- **VRAM memory timing control (`memtiming=`)** — set Adrenalin's "Memory Timing
  Control" preset (`default` / `fast` / `fast2` / `auto` / `level1` / `level2`)
  via `-set`, read it back with `-get` and `-list`, and pick it from the GUI's
  "VRAM mem timing" dropdown. Gated per-GPU by ADLX `IsSupportedMemoryTiming`.
- **`-monitor` verb** — live GPU telemetry (real clock, VRAM clock, temperature,
  hotspot, fan RPM, power, voltage, usage, VRAM used) via ADLX
  `GetCurrentGPUMetrics`, as machine-readable `key=value` lines. Only metrics the
  driver reports are emitted. This is where the *real* boost clock comes from,
  since the manual-tuning interface exposes only the offset, not the base clock.
- **GUI "Live" tab** — a second tab next to "Tuning" with a Refresh button that
  reads current telemetry on demand (`-monitor`) and shows it formatted. No
  polling: it complements external monitoring overlays rather than replacing them.

- **`-list` shows the allowed range for each tunable** (`core`, `volt`, `vram`,
  `power`), read from ADLX. The core max is an offset, so its span was not
  guessable — on an RX 9070 XT it reads `core=[-500 .. 1000]`,
  `volt=[-200 .. 0]`, `vram=[2518 .. 3000]`, `power=[-30 .. 10]`.

### Changed
- **Meaningful exit codes.** RadTune now exits non-zero when the requested work
  did not succeed: a failed or rejected setting, an out-of-range GPU index, an
  unreadable profile, a bad argument, or an unknown verb. Previously every run
  returned 0 — a scheduled task could report success while applying nothing.
- **Honest apply summary.** The final line reports what actually happened
  ("applied N", "N failed", or "nothing to apply") instead of unconditionally
  printing "Successfully applied!" even when every setting had been rejected.
- **Every `Set*` result is checked.** Core max/min, VRAM frequency and Zero RPM
  previously ignored the ADLX return value and were reported as successful even
  when the driver rejected them.
- **`-list` reports supported memory-timing presets per GPU** (via ADLX
  `GetSupportedMemoryTimingDescriptionList`), and `-set` now rejects a preset the
  card doesn't expose with a clear "Supported: …" message instead of a raw ADLX
  error. The `memtiming=` names are ADLX enum values (a superset); a given card
  exposes only a subset (e.g. Adrenalin's Standard/Accelerated).
- **Clearer `-list` / GUI labels**: the GPU max clock is an offset, now shown as
  "Core max offset" (CLI) and "Core max offset (MHz)" (GUI); voltage is likewise
  labelled "Voltage offset".

### Fixed
- **`-schedule` no longer blames elevation for every failure.** `schtasks`
  returns a generic code 1 for very different causes, and the message
  unconditionally told users to run as Administrator — which sent them chasing
  an elevated console while the real cause (a malformed task XML) was printed
  right above. The message now points at the schtasks error and only mentions
  elevation when the process actually lacks it.
- **Negative core/voltage offsets are now applied.** Tuning params moved from
  magic sentinels to `std::optional`, so a valid `core=-500` (RDNA4 offset) is no
  longer indistinguishable from "unset" and silently dropped.
- **Access violation on exit.** ADLX interfaces are now released before
  `g_ADLX.Terminate()`; releasing them afterwards dereferenced freed vtables and
  crashed the process on teardown (non-zero exit code, harmless but ugly for
  scripts and the scheduler).

## [1.2.0] - 2026-07-01

Automation & GUI release. RadTune can now schedule itself and ships an optional
native GUI. The CLI remains the engine — the GUI is just a frontend that runs it.

### Added
- **`-schedule` verb** — register a Windows Task Scheduler task (`logon` /
  `startup` / `daily=HH:MM`) that re-applies your tuning automatically, created
  with highest privileges. Manage it with `-schedule status` / `-schedule remove`.
- **`-get` verb** — print the GPU's current tuning as machine-readable
  `key=value` lines.
- **RadTuneGUI.exe** — a native Win32 frontend for the CLI:
  - Build and apply `-set` / `-load` tunings from a form.
  - **Read from GPU** to prefill the fields with the card's current values.
  - Create, inspect, and remove the scheduled task.
  - Remembers your settings between runs (`HKCU\Software\RadTune`).
  - Runs the CLI on a worker thread, so the window never freezes.
  - Modern look (header, group boxes, app icon); runs elevated so tuning has
    the rights ADLX requires.
- **`.agent/` documentation** so any coding agent can get oriented on the project.

### Fixed
- **"Read from GPU" returned nothing under the GUI** — `-get` no longer gates on
  `IsSupportedManual*Tuning` (which can report false on GPUs where tuning still
  works), and it flushes stdout so output isn't lost when piped.
- **"Show status" did nothing under the GUI** — scheduler commands now capture
  `schtasks` output through inherited handles instead of `std::system`.
- **Unreadable buttons** — the GUI's buttons are owner-drawn with explicit
  colours, so labels stay legible regardless of the Windows 11 theme / dark mode.

## [1.0.0] - 2025-12-23

Initial release: ADLX-based CLI for AMD GPU tuning (`-list`, `-set`, `-load`).

[1.2.0]: https://github.com/banorz/radtune/compare/v1.0.0...v1.2.0
[1.0.0]: https://github.com/banorz/radtune/releases/tag/v1.0.0

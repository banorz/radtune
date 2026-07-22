# Changelog

All notable changes to RadTune are documented here.
This project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **VRAM memory timing control (`memtiming=`)** — set Adrenalin's "Memory Timing
  Control" preset (`default` / `fast` / `fast2` / `auto` / `level1` / `level2`)
  via `-set`, read it back with `-get` and `-list`, and pick it from the GUI's
  "VRAM mem timing" dropdown. Gated per-GPU by ADLX `IsSupportedMemoryTiming`.

### Fixed
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

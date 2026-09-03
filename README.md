# VRAM Headroom Bell

A tiny Windows utility that records the WDDM local video-memory budget and warns
when a game approaches it. It is the telemetry-only first prototype recommended by
the accompanying [research report](WINDOWS_GAMING_MICRO_UTILITIES_RESEARCH.md#first-vram-headroom-bell-telemetry-only-mvp).

The program does **not** clear VRAM, inject into a game, change drivers, or terminate
background applications. Version 0.1 observes one DXGI signal and writes it to CSV
for correlation with a PresentMon capture.

## Build

Use Visual Studio 2022 or a recent Windows SDK:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Run

Start monitoring an existing process:

```powershell
build\Release\vram-headroom-bell.exe --pid 1234 --output session.csv
```

Or wait for a named game process:

```powershell
build\Release\vram-headroom-bell.exe --process game.exe --threshold 10 --hysteresis 3
```

It samples once per second and immediately after a DXGI budget-change notification.
Monitoring ends when the game exits. The CSV records UTC time, process ID, adapter,
current usage, current budget, calculated headroom, pressure state, and whether the
sample was triggered by a budget event.

Use `--help` for all options. Run PresentMon separately during repeatable A/B runs;
join its frame data to this CSV by timestamp. A low-headroom event is evidence to
investigate, not proof that VRAM pressure caused a hitch.

## Current constraints

* The collector targets Windows 10 or later and requires a WDDM adapter exposing
  `IDXGIAdapter3`.
* Version 0.1 selects the hardware adapter with the most dedicated video memory. A
  later version should map the game process to its actual adapter using GPU ETW or
  D3DKMT data—especially important on hybrid-graphics laptops.
* DXGI reports an adapter-wide budget. Per-process attribution and any allow-listed
  quiescence action are intentionally deferred until the telemetry MVP demonstrates
  predictive value.

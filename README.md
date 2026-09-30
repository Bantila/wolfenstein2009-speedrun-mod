# Wolfenstein (2009) Speedrun Mod

In-game speedrun overlay and launcher for Wolfenstein (2009, Raven Software), PC version 1.21. The launcher UI is available in English and Russian.

## Features

- In-game overlay drawn inside the D3D9 frame, so it works in exclusive fullscreen and shows up in recordings.
- Player coordinates and a speedometer (current and max horizontal speed per map).
- Timers: total RTA, total IGT, map RTA, map IGT. IGT excludes loads, in-engine cutscenes, pre-rendered (Bink) videos and the mission complete screen.
- LiveSplit-style splits per map: segment time, delta against your PB run (green = ahead, red = behind, gold = best segment) and split time. The PB run is saved when you finish a faster run without teleports.
- Per-map PBs for each category, with a live delta.
- Load and death counters.
- Categories: Any% and Cheat%. Cheat% keeps god mode on and runs configurable console commands after every map load (default: `give all;giveAllPowerUpgrades;momoney`).
- Hotkeys: show/hide overlay, start/finish, reset, switch category, save position, teleport. Teleporting marks the run as PRACTICE.
- Auto start and reset when the chosen start mission loads.
- Launcher: installs the mod, starts the game, and has a drag-and-drop overlay layout editor with per-widget label, size, color and anchor. Also sets the font, time format (0–3 decimals) and hotkeys. Changes apply to a running game in about 0.5 s.

## Install

1. Copy `SrmodLauncher.exe` and `srmod.dll` into the game's `SP` folder, next to `Wolf2.exe`.
2. Run `SrmodLauncher.exe` and press **Play**.

The launcher renames the original `binkw32.dll` to `binkw32_orig.dll` and installs the mod in its place. **Uninstall mod** restores the original.

Requirements: DirectX End-User Runtime (for `d3dx9_43.dll`) and .NET Framework 4.x. Windows 10 and 11 include .NET Framework 4.x.

## Default hotkeys

| Key | Action |
|-----|--------|
| F6 | Show/hide overlay |
| F7 | Start/finish run |
| F8 | Reset |
| F10 | Switch category |
| Num7 | Save position |
| Num9 | Teleport to saved position |

## How it works

- `mod.cpp` is a `binkw32.dll` proxy. Its exports are forwarded to `binkw32_orig.dll` through `binkw32.def`.
- It hot-patches `IDirect3DDevice9::Present` and `Reset` to draw the overlay with `ID3DXFont`.
- A separate thread polls game memory about every 1 ms and drives the timers (`timer.h`), so timing doesn't depend on the frame rate.
- Loads are detected with `idSessionLocal::insideExecuteMapChange`. The flag covers both map loads and save loads.
- Game structure offsets are listed at the top of `mod.cpp`. The mod checks a few code signatures and refuses to read memory on an unknown `Gamex86.dll`.
- `re/` has the Python scripts used to find the offsets (pefile + capstone).

## Build

Requires MSVC (x86) and the .NET Framework `csc.exe`, which ships with Windows.

```
build.bat          # runs timer_test, builds out\srmod.dll and out\SrmodLauncher.exe
build.bat deploy   # also copies them to ..\SP
```

The path to `vcvars32.bat` at the top of `build.bat` may need adjusting for your machine.

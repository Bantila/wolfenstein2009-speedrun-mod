# Wolfenstein (2009) Speedrun Mod

In-game speedrun overlay, full in-game settings menu (Dear ImGui), and standalone launcher for Wolfenstein (2009, Raven Software), PC version 1.21. Available in English and Russian.

## Features

- **In-Game Settings Menu (`F1`)**: Built directly into the DirectX 9 renderer using Dear ImGui. Modify any setting live in-game without alt-tabbing:
  - **Game & Visuals**: Real-time FOV slider (60°–120°) with quick presets, View/Head Bobbing toggle (`g_noViewBob` to eliminate motion sickness), and game language selector (`sys_lang`: English, Russian, French, German, Italian, Spanish, Polish).
  - **Quick Save / Load Binds**: Dedicated hotkeys for quick saving (`F5`), quick loading (`F9`), and restarting from checkpoint (`F11`).
  - **HUD Widgets Editor**: Toggle any widget, adjust position (X/Y), size, anchor, labels, and per-element colors (values, labels, backgrounds, and split deltas).
  - **Keybinds Manager**: Rebind any hotkey directly with a single click.
  - **Categories & Rules**: Any% vs Cheat%, campaign cutscene auto-start, IL practice auto-start, god mode lock, post-load commands.
  - **Records & Splits**: View current PB runs, per-map records, and reset PB data.
- **D3D9 Speedrun Overlay**: Drawn inside the Direct3D 9 frame, working seamlessly in exclusive fullscreen and video recordings.
- **Accurate Timers**: Total RTA, total IGT, map RTA, and map IGT. IGT automatically pauses during loads, in-engine cutscenes, pre-rendered (Bink) videos, and the mission complete screen.
- **Campaign Auto-Start**: The timer automatically resets and begins when the campaign intro video (`wolf_campaign_start_vo_english.bik`) starts. Visiting or reloading Train Station (`trainyard`) will **not** wipe your run.
- **LiveSplit-Style Splits**: Per-map segment times, live deltas against your PB run (green = ahead, red = behind, gold = best segment), and split times.
- **Player Metrics**: Real-time coordinates (X, Y, Z), speedometer (current and max per map), loads counter with total load duration, and death counter.
- **Categories**: Any% and Cheat%. Cheat% locks god mode and automatically executes console commands after each load (`give all;giveAllPowerUpgrades;momoney`).
- **Practice Tools**: Save player position and teleport back to saved coordinates. Teleporting marks the run as PRACTICE.
- **Standalone Launcher**: Optional external GUI to install/uninstall the mod, launch the game, and pre-configure layout or settings.

## Install

1. Copy `SrmodLauncher.exe` and `srmod.dll` into the game's `SP` folder, next to `Wolf2.exe`.
2. Run `SrmodLauncher.exe` and press **Play** (or start `Wolf2.exe` directly).

The mod proxies `binkw32.dll`. The launcher automatically renames the original `binkw32.dll` to `binkw32_orig.dll` and installs the mod in its place. **Uninstall mod** restores the original.

Requirements: DirectX End-User Runtime (for `d3dx9_43.dll`) and .NET Framework 4.x (for the external launcher; the DLL itself is pure C++).

## Default hotkeys

| Key | Action |
|-----|--------|
| **F1** | **Open / close In-Game Settings Menu** |
| **F5** | **Quick Save** (`contentManagerCommand save full`) |
| **F9** | **Quick Load** (`contentManagerCommand loadLast`) |
| **F11** | **Restart Checkpoint** (`ScaleformDiedRestart`) |
| **F6** | Show / hide overlay |
| **F7** | Start / finish run |
| **F8** | Reset timer |
| **F10** | Switch category (Any% / Cheat%) |
| **Num 7** | Save position (practice) |
| **Num 9** | Teleport to saved position (practice) |

All hotkeys can be rebound at any time via the in-game menu (`F1`) or in `SrmodLauncher.exe`.

## How it works

- `mod.cpp` is a `binkw32.dll` proxy forwarding original functions to `binkw32_orig.dll` via `binkw32.def`.
- Hot-patches `IDirect3DDevice9::Present` and `Reset` to render both the D3DX font overlay and the Dear ImGui in-game configurator.
- Subclasses the game window's `WndProc` to handle mouse and keyboard navigation inside the in-game settings menu.
- A high-precision background polling thread (1 ms query frequency) monitors game state and drives timers (`timer.h`).
- Detects the start of the campaign via wrapped `SrBinkOpen` when `wolf_campaign_start` is loaded, resetting the timer.
- Executes console commands safely via `idCmdSystem::BufferCommandText`.
- Live-reloads settings from `srmod.ini` and syncs changes seamlessly between the game and the launcher.

## Build

Requires MSVC (x86) and .NET Framework `csc.exe` (shipped with Windows).

```
build.bat          # runs timer_test, builds out\srmod.dll and out\SrmodLauncher.exe
build.bat deploy   # also copies binaries to ..\SP
```

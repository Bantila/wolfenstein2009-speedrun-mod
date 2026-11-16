# Wolfenstein (2009) Speedrun Mod

In-game speedrun overlay, full in-game settings menu (Dear ImGui), and standalone launcher for Wolfenstein (2009, Raven Software), PC version 1.21. Available in English and Russian.

## Features

- **BXT-Style New Run Setup Wizard (`F2` / Auto on 'New Game')**: Interactive run configuration wizard modeled after Bunnymod XT for Half-Life:
  - **Auto-Trigger**: Automatically intercepts clicking "New Game" from the main menu, or opens anytime via `F2`.
  - **Category Selection**: Choose between **Any%**, **All Missions** (18 missions tracked), **100%** (all 148 Gold, 88 Intel, 16 Tomes of Power, and upgrades), **Individual Level (IL)**, **Segmented**, or **Cheat%** (practice). Displays clear rules and timing criteria.
  - **Individual Levels (IL) & New Game+**: Full support for all 15 official speedrun.com individual levels (`Train Station`, `Dig Site`, `Church`, `Farm`, `Hospital`, `Cannery`, `Castle`, `Airfield`, `Zeppelin`, `Black Sun Dimension`, `Warehouse`, `Officer's House`, `SS Headquarters`, `SS Paranormal Base`, `Radio Tower`). Features optional **New Game+ Loadout** that instantly outfits BJ upon spawning with all weapons, veil medallion powers, full upgrades, max gold, and ammo, starting the IL timer on player control.
  - **Ruleset Enforcement**:
    - **Restricted (Competitive Standard)**: Quicksaves (`F5`) and Quickloads (`F9`) are **physically blocked** by the mod proxy at the window hook level. Cheats and teleports are completely disabled. FOV, head bobbing, language, and checkpoint reloads remain allowed.
    - **Unrestricted**: All glitches, quicksaves, quickloads, geometry skips, and mission replay warps allowed.
  - **Difficulty Selection**: Set game difficulty (`Casual`, `Normal`, `Hard`, or community-standard `Über` via `g_skill 0..3`).
  - **Instant Run Start**: One click (`[ START OPERATION NOW ]`) applies the skill cvar, resets timers, and launches directly into the opening cutscene or IL level.
- **Warp Trick Timer Protection**: Speedrunners frequently use the "warp trick" (replaying a mission from the menu and aborting immediately to warp to its completion trigger, e.g., Train Station). While a run is active (`g_run.state == Run::Running`), **the timer is strictly protected**: visiting or reloading Train Station (`trainyard`) will **never** zero out your run.
- **In-Game Settings & Cheats Menu (`F1`)**: Built directly into the DirectX 9 renderer using Dear ImGui with authentic dark iron aesthetics. Modify any setting live in-game without alt-tabbing:
  - **Cheats & Spawner**: One-click unlock for **FlameDLC** (flamethrower in opening room), **Built-in Cheat Menu** (Esc -> Options -> Game -> Cheats), and **Developer Console** (`com_allowConsole 1`). Toggles for God Mode, NoTarget, Infinite Stamina, Give All Weapons, Give All Veil Powers & Upgrades, Max Gold, All Intel & Objectives, Ammo Dispensary, 19 spawnable entities (SS, Veil Heavy, Altered, Assassin, Elite, Geist, etc.), and Tactical Map Warps.
  - **Game & Visuals**: Real-time FOV slider (60°–120°), View/Head Bobbing toggle with sprint animation fix, and game language selector (`sys_lang`: English, Russian, French, German, Italian, Spanish, Polish).
  - **Sprint Animation Fix**: Setting `g_noViewBob 1` in vanilla breaks BJ's sprinting arm animation. Srmod zeroes out camera spring angles while keeping the bobbing state active, completely eliminating camera shake while keeping the sprint animation 100% fluid.
  - **Smart Mouse Cursor Control**: Frees and shows the mouse cursor inside the ImGui menu and suppresses in-game shooting or camera rotation while the menu or wizard is open.
  - **Quick Save / Load Binds**: Dedicated hotkeys for quick saving (`F5`), quick loading (`F9`), and restarting from checkpoint (`F11`). In Restricted mode, quick save and load are cleanly disabled with an informative banner.
  - **HUD Widgets Editor**: Toggle any widget, adjust position (X/Y), size, anchor, labels, and per-element colors.
  - **Keybinds Manager**: Rebind any hotkey directly with a single click.
  - **Records & Splits**: View current PB runs, per-map records for the active category and ruleset, and reset PB data.
- **Authentic Military Telemetry HUD**: Restyled to match the dark industrial 1944 Wolfenstein aesthetic: beveled gunmetal frames, corner technical brackets `[ ]`, dirt-white gauge typography, and tactical dossier splits. Original game HUD elements are left completely untouched.
- **D3D9 Speedrun Overlay**: Drawn inside the Direct3D 9 frame, working seamlessly in exclusive fullscreen and video recordings. Category widget displays full status (e.g. `Any% (Restricted) | Über`).
- **Accurate Timers**: Total RTA, total IGT, map RTA, and map IGT. IGT automatically pauses during loads, in-engine cutscenes, pre-rendered (Bink) videos, and the mission complete screen.
- **Campaign Auto-Start**: The timer automatically resets and begins when the campaign intro video (`wolf_campaign_start_vo_english.bik`) starts.
- **LiveSplit-Style Splits**: Per-map segment times, live deltas against your PB run (green = ahead, red = behind, gold = best segment), and split times. Separate PBs maintained per category and ruleset (`any_restricted`, `any_unrestricted`, etc.).
- **Player Metrics**: Real-time coordinates (X, Y, Z), speedometer (current and max per map), loads counter with total load duration, and death counter.
- **Practice Tools**: Save player position and teleport back to saved coordinates. Teleporting marks the run as PRACTICE.
- **Standalone Launcher**: External GUI to install/uninstall the mod, launch the game, and pre-configure layout or settings.

## Install

1. Copy `SrmodLauncher.exe` and `srmod.dll` into the game's `SP` folder, next to `Wolf2.exe`.
2. Run `SrmodLauncher.exe` and press **Play** (or start `Wolf2.exe` directly).

The mod proxies `binkw32.dll`. The launcher automatically renames the original `binkw32.dll` to `binkw32_orig.dll` and installs the mod in its place. **Uninstall mod** restores the original.

Requirements: DirectX End-User Runtime (for `d3dx9_43.dll`) and .NET Framework 4.x (for the external launcher; the DLL itself is pure C++).

## Default hotkeys

| Key | Action |
|-----|--------|
| **F1** | **Open / close In-Game Settings Menu** |
| **F2** | **Start New Run Wizard (BXT Setup: Category, Ruleset, Difficulty)** |
| **F5** | **Quick Save** (`contentManagerCommand save full` — blocked in Restricted) |
| **F9** | **Quick Load** (`contentManagerCommand loadLast` — blocked in Restricted) |
| **F11** | **Restart Checkpoint** (`ScaleformDiedRestart`) |
| **F6** | Show / hide overlay |
| **F7** | Start / finish run |
| **F8** | Reset timer |
| **F10** | Cycle category (Any%, 100%, All Bosses, IL, Cheat%) |
| **Num 7** | Save position (practice) |
| **Num 9** | Teleport to saved position (practice — blocked in Restricted) |

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

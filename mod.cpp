// srmod.dll - Wolfenstein (2009) Speedrun Mod
// Authentic Wolfenstein Military Instrumentation UI, In-Game Wizard, Cheats & Entity Spawner,
// Speedrun.com Ruleset Enforcement, All Missions Tracker, and Individual Levels (New Game+).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include "timer.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- Game memory offsets (Wolfenstein 2009 single-player 1.2 / 1.1.1) ------------------------
const DWORD WOLF_SESSION_VT     = 0x1030F538; // idSessionLocal in Wolf2.exe (.data)
const DWORD WOLF_SESSION_OFF    = 0x0070F538;
const DWORD RVA_TIME_INGAME     = 0x005E3280; // uint32 in Gamex86.dll: in-game time in ms
const DWORD RVA_GAMELOCAL       = 0x005E26A8; // pointer to idGameLocal in Gamex86.dll
const DWORD RVA_CMDSYSTEM       = 0x0054F10C; // pointer to idCmdSystem in Gamex86.dll
const DWORD RVA_PHYS_GETORIGIN  = 0x00130FA0; // idPhysics_Player::GetOrigin (vtable slot 38)
const DWORD RVA_LOCALPLAYER_VT  = 0x004A8124; // idPlayer vtable in Gamex86.dll

const int   GL_LOCALPLAYER      = 0x0000;    // idGameLocal::localPlayer (entity pointer)
const int   GL_MAPNAME          = 0x0034;    // idGameLocal::mapFileName (char*)
const int   GL_CINEMATIC        = 0x0064;    // bool
const int   GL_MISSIONCOMPLETE  = 0x0094;    // pointer to mission complete controller
const int   MC_ACTIVE           = 0x0008;    // bool: mission complete screen is up
const int   PL_PHYSICS          = 0x0114;    // pointer to idPhysics in idPlayer
const int   PH_ORIGIN           = 0x0048;    // float[3]
const int   PH_VELOCITY         = 0x0054;    // float[3]
const int   PL_HEALTH           = 0x17E4;    // float
const int   PL_GODMODE          = 0x1818;    // bool

const int   CMD_BUFFERTEXT_IDX  = 8;         // idCmdSystem::BufferCommandText vtable slot
const int   CMD_EXEC_APPEND     = 2;         // cmdExecution_t: append to buffer

// ---- Minimal D3DX font declarations ---------------------------------------------------------
struct ID3DXFont : IUnknown {
    virtual void __stdcall p0() = 0; virtual void __stdcall p1() = 0; virtual void __stdcall p2() = 0;
    virtual void __stdcall p3() = 0; virtual void __stdcall p4() = 0; virtual void __stdcall p5() = 0;
    virtual void __stdcall p6() = 0; virtual void __stdcall p7() = 0; virtual void __stdcall p8() = 0;
    virtual void __stdcall p9() = 0; virtual void __stdcall p10() = 0;
    virtual INT __stdcall DrawTextA(void* sprite, LPCSTR s, INT n, LPRECT r, DWORD fmt, D3DCOLOR c) = 0;
    virtual INT __stdcall DrawTextW(void* sprite, LPCWSTR s, INT n, LPRECT r, DWORD fmt, D3DCOLOR c) = 0;
    virtual HRESULT __stdcall OnLostDevice() = 0;
    virtual HRESULT __stdcall OnResetDevice() = 0;
};
typedef HRESULT(WINAPI* D3DXCreateFontA_t)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD,
                                           DWORD, DWORD, DWORD, LPCSTR, ID3DXFont**);

// ---- Widgets (Authentic Military Instrumentation HUD) ---------------------------------------
enum WidgetId {
    W_COORDS, W_SPEED, W_RTA, W_IGT, W_MAP_RTA, W_MAP_IGT,
    W_PB, W_LOADS, W_DEATHS, W_CATEGORY, W_MAP, W_SPLITS, W_FPS,
    W_COUNT
};
const char* kWidgetNames[W_COUNT] = {
    "coords", "speed", "total_rta", "total_igt", "map_rta", "map_igt",
    "pb", "loads", "deaths", "category", "map", "splits", "fps"
};
const char* kWidgetLabels[W_COUNT] = {
    "POS: ", "SPEED: ", "RTA: ", "IGT: ", "MAP RTA: ", "MAP IGT: ",
    "PB: ", "LOADS: ", "DEATHS: ", "OP: ", "MAP: ", "", "FPS: "
};
const char* kWidgetDisplayNames[W_COUNT] = {
    "Coordinates (X, Y, Z)", "Speedometer (Telemetry)", "Total RTA", "Total IGT",
    "Map RTA", "Map IGT", "Map PB & Live Delta", "Loads Counter", "Deaths Counter",
    "Category Name", "Map Name", "Splits Dossier", "FPS Counter"
};

struct Widget {
    bool on; float x, y; int anchor, size; std::string label;
    D3DCOLOR color, labelColor, bg;
    D3DCOLOR nameColor, segColor, timeColor, aheadColor, behindColor, goldColor;
};

enum Key {
    K_TOGGLE, K_STARTSTOP, K_RESET, K_CATEGORY, K_SAVEPOS, K_TELEPORT,
    K_QUICKSAVE, K_QUICKLOAD, K_RESTART_CP, K_MENU, K_NEWRUN,
    K_COUNT
};
const char* kKeyNames[K_COUNT] = {
    "toggle", "start_stop", "reset", "category", "save_pos", "teleport",
    "quick_save", "quick_load", "restart_cp", "menu", "new_run"
};
const int kKeyDefaults[K_COUNT] = {
    VK_F6, VK_F7, VK_F8, VK_F10, VK_NUMPAD7, VK_NUMPAD9,
    VK_F5, VK_F9, VK_F11, VK_F1, VK_F2
};
const char* kKeyTitles[K_COUNT] = {
    "Show/hide overlay", "Start/finish run", "Reset timer", "Switch category",
    "Save position (practice)", "Teleport to position",
    "Quick Save", "Quick Load", "Restart Checkpoint", "Open In-Game Menu (F1)",
    "Start New Run Wizard (F2)"
};

// ---- Speedrun Categories (exact speedrun.com setup) ------------------------------------------
enum CategoryId {
    CAT_ANY,
    CAT_ALL_MISSIONS,
    CAT_100,
    CAT_SEGMENTED,
    CAT_IL,
    CAT_CHEAT,
    CAT_COUNT
};
const char* kCategoryKeys[CAT_COUNT] = { "any", "all_missions", "100", "segmented", "il", "cheat" };
const char* kCategoryNames[CAT_COUNT] = { "Any%", "All Missions", "100%", "Segmented", "Individual Level (IL)", "Cheat% (Practice)" };
const char* kCategorySummaries[CAT_COUNT] = {
    "Complete the game as fast as possible from campaign start to the mission complete dossier at Black Sun end.",
    "Complete all 18 story and side missions on speedrun.com checklist. (Currently 0 runs submitted on leaderboard!)",
    "Complete all missions, finding all 148 Golds, all 88 Intelligence, and all 16 Tomes of Power.",
    "Multi-segment speedrun using saves and reloads to achieve the best segmented run.",
    "Individual level time attack with full New Game+ loadout (All Weapons & Veil Powers).",
    "Practice category with developer cheats, god mode, and position teleportation."
};
const char* kCategoryTimingRules[CAT_COUNT] = {
    "Timing starts on starting a new game (campaign cutscene) and ends when the 'mission complete' dossier appears on screen at the end of Black Sun.",
    "Timing starts on campaign cutscene and ends upon completing all 18 story and side missions.",
    "Timing starts on campaign cutscene and ends on final Black Sun dossier with 100% collectibles in journal.",
    "Segmented timing across saves and level segments.",
    "Timing begins on first frame of player gaining control of BJ, and ends on last frame of level gameplay (mission complete screen).",
    "Practice / unrestricted timing."
};

// ---- Speedrun Rulesets (speedrun.com 'Edits in config' variable) -----------------------------
enum RulesetId {
    RULESET_RESTRICTED,    // 0: speedrun.com Restricted: Quicksaves/loads BLOCKED, no cheats, warps allowed
    RULESET_UNRESTRICTED,  // 1: speedrun.com Unrestricted: Quicksaves/loads allowed, warp glitches allowed
    RULESET_COUNT
};
const char* kRulesetKeys[RULESET_COUNT] = { "restricted", "unrestricted" };
const char* kRulesetNames[RULESET_COUNT] = { "Restricted", "Unrestricted" };
const char* kRulesetSummaries[RULESET_COUNT] = {
    "Speedrun.com Restricted: Quicksaves (F5) and Quickloads (F9) are STRICTLY FORBIDDEN and blocked by mod proxy. Console commands forbidden. ALLOWED: Warps (mission replay trick), skips without saves, FOV, View Bobbing toggle, Language, Checkpoint Reload.",
    "Speedrun.com Unrestricted: Quicksaves (F5), quickloads (F9), mission replay warps, and geometry skips permitted. Prohibited: infinite stamina (g_infiniteStamina 1) and godmode/cheats."
};

// Difficulties
enum DifficultyId {
    DIFF_CASUAL,   // 0: Easy
    DIFF_NORMAL,   // 1: Normal
    DIFF_HARD,     // 2: Hard
    DIFF_UBER,     // 3: Uber
    DIFF_COUNT
};
const char* kDifficultyNames[DIFF_COUNT] = { "Easy (Casual)", "Normal (Medium)", "Hard", "Uber (Competitive Standard)" };
const char* kDifficultyShortNames[DIFF_COUNT] = { "Easy", "Normal", "Hard", "Uber" };

// 18 official story and side missions from speedrun.com All Missions checklist
const int kAllMissionsCount = 18;
const char* kAllMissionsIds[kAllMissionsCount] = {
    "trainyard", "digsite", "church", "farm", "caverns", "hospital",
    "cannery", "castle", "castle_top", "airfield_west", "airfield_east", "zeppelin",
    "blacksun", "mtw_ware", "mtw_off", "mte_ss_hq", "mte_para_hq", "downtown_radio"
};
const char* kAllMissionsNames[kAllMissionsCount] = {
    "Train Station (Вокзал)",
    "Dig Site (Раскопки)",
    "Church (Церковь)",
    "Farm (Ферма)",
    "Caverns (Пещеры)",
    "Hospital (Больница)",
    "Cannery (Консервный завод)",
    "Castle (Замок)",
    "Castle Top (Вершина замка)",
    "Airfield West (Аэродром — запад)",
    "Airfield East (Аэродром — восток)",
    "Zeppelin (Дирижабль)",
    "Black Sun Dimension (Чёрное Солнце)",
    "Warehouse (Склад)",
    "Officer's House (Квартира офицера)",
    "SS Headquarters (Штаб-квартира СС)",
    "SS Paranormal Base (База исследований СС)",
    "Radio Tower (Радиовышка)"
};
static bool g_allMissionsDone[kAllMissionsCount] = { false };

// 15 official speedrun.com Individual Levels (IL)
const int kIlLevelsCount = 15;
const char* kIlLevelIds[kIlLevelsCount] = {
    "trainyard", "digsite", "church", "farm", "hospital",
    "cannery", "castle", "airfield_west", "zeppelin", "blacksun",
    "mtw_ware", "mtw_off", "mte_ss_hq", "mte_para_hq", "downtown_radio"
};
const char* kIlLevelNames[kIlLevelsCount] = {
    "Train Station", "Dig Site", "Church", "Farm", "Hospital",
    "Cannery", "Castle", "Airfield", "Zeppelin", "Black Sun Dimension",
    "Warehouse", "Officer's House", "SS Headquarters", "SS Paranormal Base", "Radio Tower"
};
const char* kIlLevelPaths[kIlLevelsCount] = {
    "game/trainyard/trainyard",
    "game/digsite/digsite",
    "game/church/church",
    "game/farm/farm",
    "game/hospital/hospital",
    "game/cannery/cannery",
    "game/castle/castle",
    "game/airfield/airfield_west",
    "game/zeppelin/zeppelin",
    "game/blacksun/blacksun",
    "game/mtw/mtw_ware",
    "game/mtw/mtw_off",
    "game/mte/mte_ss_hq",
    "game/mte/mte_para_hq",
    "game/downtown/downtown_radio"
};

// Spawnable entities for cheat menu
const int kSpawnEntitiesCount = 19;
const char* kSpawnEntityIds[kSpawnEntitiesCount] = {
    "enemies_elite_guard", "enemies_elite_guard_02", "enemies_ak_flame_trooper",
    "enemies_despoiled_green", "enemies_despoiled_red", "enemies_ak_heavy_trooper",
    "enemies_ak_rocket_trooper", "enemies_scribe", "enemies_sniffer",
    "enemies_wehrmacht_officer", "enemies_wehrmacht_sniper", "enemies_wehrmacht_infantry",
    "enemies_wehrmacht_infantry_mp40", "enemies_altered", "enemies_ak_assassin",
    "enemies_SS_officer", "enemies_SS_sentry",
    "allies_kreisau_soldier_01", "allies_kreisau_erik"
};
const char* kSpawnEntityNames[kSpawnEntitiesCount] = {
    "Enemy: Elite Guard (Knife)", "Enemy: Elite Guard (Whip)", "Enemy: Flame Trooper",
    "Enemy: Green Skeleton (Despoiled)", "Enemy: Red Skeleton (Despoiled)", "Enemy: Heavy Trooper",
    "Enemy: Rocket Trooper (Flying)", "Enemy: Scribe (Veil Mage)", "Enemy: Sniffer (Mutant)",
    "Enemy: Wehrmacht Officer", "Enemy: Sniper", "Enemy: Infantry (Kar98)",
    "Enemy: Infantry (MP40)", "Enemy: Altered (Invisible Mutant)", "Enemy: Assassin",
    "Enemy: SS Officer", "Enemy: SS Sentry",
    "Ally: Kreisau Soldier (Friendly)", "Ally: Erik Engel (Friendly)"
};

struct Settings {
    Widget w[W_COUNT];
    int keys[K_COUNT];
    int decimals = 2, splitLines = 10;
    bool hoursAlways = false, shadow = true;
    bool autoStartCutscene = true; // Auto-reset and start run on wolf_campaign_start cutscene
    bool autoStartMap = false;      // Auto-reset when entering start_map (IL speedruns only)
    bool cheatGod = true;
    int fov = 80;
    bool headBob = true;
    std::string language = "english";
    std::string font = "Consolas", category = "any", startMap = "trainyard";
    int ruleset = RULESET_RESTRICTED; // 0 = restricted, 1 = unrestricted
    int difficulty = DIFF_UBER;       // 0..3 (default 3 = Uber)
    bool ilNewGamePlus = true;        // New Game+ full loadout for IL runs
    int ilLevelIdx = 0;               // 0..14
    std::string cheatCmds, teleportCmd;
};

const char* kLanguages[] = { "english", "russian", "french", "german", "italian", "spanish", "polish" };
const int kLangCount = 7;

static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_pbIni[MAX_PATH], g_log[MAX_PATH];
static std::mutex g_mx;
static Settings g_set;
static Run g_run;
static bool g_fontsDirty = true, g_visible = true, g_supported = false;
static float g_pos[3], g_speed, g_maxSpeed;
static bool g_havePlayer;
static double g_pb = -1;
static std::vector<Split> g_pbSplits;
static volatile LONG g_binkOpen;
static std::string g_msg; static DWORD g_msgUntil;
static float g_fps = 0.0f; static int g_frameCount = 0; static DWORD g_lastFpsTime = 0;
static bool g_ilPendingLoadout = false;

// In-Game Menu & Wizard state
static bool g_imguiInit = false;
static HWND g_gameHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool g_menuOpen = false;
static bool g_wizardOpen = false;
static bool g_wasMenuOpen = false;
static int g_rebindKeyIndex = -1;
static int g_selectedWidget = 0;
static bool g_confirmResetPb = false;
static int g_selectedSpawnEntity = 0;
static int g_selectedWarpMap = 0;

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_log, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static std::string IniS(const char* sec, const char* key, const char* def, const char* file = g_ini) {
    char b[1024]; GetPrivateProfileStringA(sec, key, def, b, sizeof b, file); return b;
}
static float IniF(const char* sec, const char* key, float def) {
    char d[32]; snprintf(d, sizeof d, "%g", def); return (float)atof(IniS(sec, key, d).c_str());
}
static int IniI(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_ini); }

static void Message(const std::string& m) { g_msg = m; g_msgUntil = GetTickCount() + 2500; }

static int GetAllMissionsCompletedCount() {
    int count = 0;
    for (int i = 0; i < kAllMissionsCount; i++) if (g_allMissionsDone[i]) count++;
    return count;
}

static void CheckAllMissionComplete(const std::string& mapName) {
    for (int i = 0; i < kAllMissionsCount; i++) {
        if (mapName == kAllMissionsIds[i] || mapName.find(kAllMissionsIds[i]) != std::string::npos) {
            if (!g_allMissionsDone[i]) {
                g_allMissionsDone[i] = true;
                int count = GetAllMissionsCompletedCount();
                Log("All Missions progress: %d/%d (%s)", count, kAllMissionsCount, kAllMissionsNames[i]);
                Message(std::string(kAllMissionsNames[i]) + " (" + std::to_string(count) + "/" + std::to_string(kAllMissionsCount) + ")");
            }
            break;
        }
    }
}

static Settings LoadSettings() {
    Settings s;
    for (int i = 0; i < W_COUNT; i++) {
        std::string sec = std::string("widget.") + kWidgetNames[i];
        Widget& w = s.w[i];
        w.on = IniI(sec.c_str(), "enabled", i != W_MAP) != 0;
        bool splits = i == W_SPLITS;
        w.x = IniF(sec.c_str(), "x", splits ? 0.99f : 0.01f);
        w.y = IniF(sec.c_str(), "y", splits ? 0.02f : 0.02f + 0.03f * i);
        w.anchor = IniI(sec.c_str(), "anchor", splits ? 1 : 0);
        w.size = IniI(sec.c_str(), "size", 20);
        auto C = [&](const char* key, const char* def) { return (D3DCOLOR)strtoul(IniS(sec.c_str(), key, def).c_str(), nullptr, 16); };
        w.color = C("color", "FFEDEAE2");          // Authentic dirty white/bone
        char hex[16]; snprintf(hex, sizeof hex, "%08X", w.color);
        w.labelColor = C("label_color", "FFA09C92"); // Weathered military technical gray
        w.bg = C("bg", "EB121315");                  // Authentic dark iron plate
        w.nameColor = C("name_color", hex); w.segColor = C("seg_color", hex); w.timeColor = C("time_color", hex);
        w.aheadColor = C("ahead_color", "FF40A850"); // Tactical restrained green
        w.behindColor = C("behind_color", "FFC83838"); // Muted warning red
        w.goldColor = C("gold_color", "FFE0B030");     // Aged brass/gold
        w.label = IniS(sec.c_str(), "label", kWidgetLabels[i]);
    }
    for (int i = 0; i < K_COUNT; i++) s.keys[i] = IniI("keys", kKeyNames[i], kKeyDefaults[i]);
    s.decimals = IniI("general", "decimals", 2);
    s.splitLines = IniI("general", "split_lines", 10);
    s.hoursAlways = IniI("general", "hours_always", 0) != 0;
    s.autoStartCutscene = IniI("general", "auto_start_cutscene", 1) != 0;
    s.autoStartMap = IniI("general", "auto_start_map", 0) != 0;
    s.shadow = IniI("general", "shadow", 1) != 0;
    s.font = IniS("general", "font", "Consolas");
    s.category = IniS("general", "category", "any");
    s.ruleset = IniI("general", "ruleset", RULESET_RESTRICTED);
    if (s.ruleset < 0 || s.ruleset >= RULESET_COUNT) s.ruleset = RULESET_RESTRICTED;
    s.difficulty = IniI("general", "difficulty", DIFF_UBER);
    if (s.difficulty < 0 || s.difficulty >= DIFF_COUNT) s.difficulty = DIFF_UBER;
    s.startMap = IniS("general", "start_map", "trainyard");
    s.fov = IniI("game", "fov", 80);
    s.headBob = IniI("game", "head_bob", 1) != 0;
    s.language = IniS("launcher", "language", "english");
    s.ilNewGamePlus = IniI("game", "il_newgame_plus", 1) != 0;
    s.ilLevelIdx = IniI("game", "il_level_idx", 0);
    if (s.ilLevelIdx < 0 || s.ilLevelIdx >= kIlLevelsCount) s.ilLevelIdx = 0;
    s.cheatGod = IniI("cheat", "god", 1) != 0;
    s.cheatCmds = IniS("cheat", "commands", "give all;giveAllPowerUpgrades;momoney");
    s.teleportCmd = IniS("cheat", "teleport_cmd", "script $player1.setOrigin('%.3f %.3f %.3f');");
    return s;
}

static void SaveSettings(const Settings& s) {
    for (int i = 0; i < W_COUNT; i++) {
        std::string sec = std::string("widget.") + kWidgetNames[i];
        const Widget& w = s.w[i];
        WritePrivateProfileStringA(sec.c_str(), "enabled", w.on ? "1" : "0", g_ini);
        char buf[64];
        snprintf(buf, sizeof buf, "%.4f", w.x); WritePrivateProfileStringA(sec.c_str(), "x", buf, g_ini);
        snprintf(buf, sizeof buf, "%.4f", w.y); WritePrivateProfileStringA(sec.c_str(), "y", buf, g_ini);
        WritePrivateProfileStringA(sec.c_str(), "anchor", std::to_string(w.anchor).c_str(), g_ini);
        WritePrivateProfileStringA(sec.c_str(), "size", std::to_string(w.size).c_str(), g_ini);
        snprintf(buf, sizeof buf, "%08X", w.color); WritePrivateProfileStringA(sec.c_str(), "color", buf, g_ini);
        snprintf(buf, sizeof buf, "%08X", w.labelColor); WritePrivateProfileStringA(sec.c_str(), "label_color", buf, g_ini);
        snprintf(buf, sizeof buf, "%08X", w.bg); WritePrivateProfileStringA(sec.c_str(), "bg", buf, g_ini);
        if (i == W_SPLITS) {
            snprintf(buf, sizeof buf, "%08X", w.nameColor); WritePrivateProfileStringA(sec.c_str(), "name_color", buf, g_ini);
            snprintf(buf, sizeof buf, "%08X", w.segColor); WritePrivateProfileStringA(sec.c_str(), "seg_color", buf, g_ini);
            snprintf(buf, sizeof buf, "%08X", w.timeColor); WritePrivateProfileStringA(sec.c_str(), "time_color", buf, g_ini);
            snprintf(buf, sizeof buf, "%08X", w.aheadColor); WritePrivateProfileStringA(sec.c_str(), "ahead_color", buf, g_ini);
            snprintf(buf, sizeof buf, "%08X", w.behindColor); WritePrivateProfileStringA(sec.c_str(), "behind_color", buf, g_ini);
            snprintf(buf, sizeof buf, "%08X", w.goldColor); WritePrivateProfileStringA(sec.c_str(), "gold_color", buf, g_ini);
        }
        std::string qlabel = "\"" + w.label + "\"";
        WritePrivateProfileStringA(sec.c_str(), "label", qlabel.c_str(), g_ini);
    }
    for (int i = 0; i < K_COUNT; i++) {
        WritePrivateProfileStringA("keys", kKeyNames[i], std::to_string(s.keys[i]).c_str(), g_ini);
    }
    WritePrivateProfileStringA("general", "decimals", std::to_string(s.decimals).c_str(), g_ini);
    WritePrivateProfileStringA("general", "split_lines", std::to_string(s.splitLines).c_str(), g_ini);
    WritePrivateProfileStringA("general", "hours_always", s.hoursAlways ? "1" : "0", g_ini);
    WritePrivateProfileStringA("general", "auto_start_cutscene", s.autoStartCutscene ? "1" : "0", g_ini);
    WritePrivateProfileStringA("general", "auto_start_map", s.autoStartMap ? "1" : "0", g_ini);
    WritePrivateProfileStringA("general", "shadow", s.shadow ? "1" : "0", g_ini);
    std::string qfont = "\"" + s.font + "\"";
    WritePrivateProfileStringA("general", "font", qfont.c_str(), g_ini);
    WritePrivateProfileStringA("general", "category", s.category.c_str(), g_ini);
    WritePrivateProfileStringA("general", "ruleset", std::to_string(s.ruleset).c_str(), g_ini);
    WritePrivateProfileStringA("general", "difficulty", std::to_string(s.difficulty).c_str(), g_ini);
    WritePrivateProfileStringA("general", "start_map", s.startMap.c_str(), g_ini);
    WritePrivateProfileStringA("game", "fov", std::to_string(s.fov).c_str(), g_ini);
    WritePrivateProfileStringA("game", "head_bob", s.headBob ? "1" : "0", g_ini);
    WritePrivateProfileStringA("game", "il_newgame_plus", s.ilNewGamePlus ? "1" : "0", g_ini);
    WritePrivateProfileStringA("game", "il_level_idx", std::to_string(s.ilLevelIdx).c_str(), g_ini);
    WritePrivateProfileStringA("launcher", "language", s.language.c_str(), g_ini);
    WritePrivateProfileStringA("cheat", "god", s.cheatGod ? "1" : "0", g_ini);
    WritePrivateProfileStringA("cheat", "commands", s.cheatCmds.c_str(), g_ini);
    WritePrivateProfileStringA("cheat", "teleport_cmd", s.teleportCmd.c_str(), g_ini);
}

static std::string GetPbCategoryKey(const Settings& s) {
    if (s.category == "cheat") return "cheat";
    std::string r = s.ruleset == RULESET_RESTRICTED ? "restricted" : "unrestricted";
    return s.category + "_" + r;
}

static std::string GetFullCategoryDisplay(const Settings& s) {
    if (s.category == "cheat") {
        return std::string("Cheat% | ") + kDifficultyShortNames[s.difficulty];
    }
    std::string catName = "Any%";
    for (int i = 0; i < CAT_COUNT; i++) {
        if (s.category == kCategoryKeys[i]) { catName = kCategoryNames[i]; break; }
    }
    if (s.category == "all_missions") {
        catName = "All Missions (" + std::to_string(GetAllMissionsCompletedCount()) + "/" + std::to_string(kAllMissionsCount) + ")";
    }
    if (s.category == "il") {
        std::string mName = kIlLevelNames[s.ilLevelIdx];
        std::string ng = s.ilNewGamePlus ? " [NG+]" : "";
        return "IL: " + mName + ng + " (" + kRulesetNames[s.ruleset] + ") | " + kDifficultyShortNames[s.difficulty];
    }
    return catName + " (" + kRulesetNames[s.ruleset] + ") | " + kDifficultyShortNames[s.difficulty];
}

static double ReadPb(const std::string& catKey, const std::string& map) {
    std::string v = IniS(catKey.c_str(), map.c_str(), "", g_pbIni);
    if (v.empty() && catKey.find('_') != std::string::npos) {
        std::string baseCat = catKey.substr(0, catKey.find('_'));
        v = IniS(baseCat.c_str(), map.c_str(), "", g_pbIni);
    }
    return v.empty() ? -1 : atof(v.c_str());
}

static std::vector<Split> ReadPbSplits(const std::string& catKey) {
    std::string sec = "splits." + catKey;
    std::vector<Split> v;
    int n = GetPrivateProfileIntA(sec.c_str(), "n", 0, g_pbIni);
    if (n == 0 && catKey.find('_') != std::string::npos) {
        std::string baseCat = catKey.substr(0, catKey.find('_'));
        sec = "splits." + baseCat;
        n = GetPrivateProfileIntA(sec.c_str(), "n", 0, g_pbIni);
    }
    for (int i = 0; i < n; i++) {
        std::string k = std::to_string(i);
        double t = atof(IniS(sec.c_str(), ("time" + k).c_str(), "0", g_pbIni).c_str());
        v.push_back(Split{IniS(sec.c_str(), ("map" + k).c_str(), "", g_pbIni), t - (v.empty() ? 0 : v.back().time), t, false});
    }
    return v;
}

static void WritePbSplits(const std::string& catKey, const std::vector<Split>& v) {
    std::string sec = "splits." + catKey;
    WritePrivateProfileStringA(sec.c_str(), nullptr, nullptr, g_pbIni);
    WritePrivateProfileStringA(sec.c_str(), "n", std::to_string(v.size()).c_str(), g_pbIni);
    for (size_t i = 0; i < v.size(); i++) {
        char t[32]; snprintf(t, sizeof t, "%.3f", v[i].time);
        WritePrivateProfileStringA(sec.c_str(), ("map" + std::to_string(i)).c_str(), v[i].map.c_str(), g_pbIni);
        WritePrivateProfileStringA(sec.c_str(), ("time" + std::to_string(i)).c_str(), t, g_pbIni);
    }
}

// ---- Game memory -----------------------------------------------------------------------------
struct Snapshot { bool loading, paused, mission, cinematic, player; float pos[3], vel[3], hp; char map[64]; };
static BYTE* g_session;

static BYTE* FindSession() {
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".data", 6)) continue;
        DWORD* p = (DWORD*)(exe + sec->VirtualAddress);
        DWORD n = sec->Misc.VirtualSize / 4;
        for (DWORD k = 0; k < n; k++) if (p[k] == WOLF_SESSION_VT) return (BYTE*)&p[k];
    }
    return nullptr;
}

static bool CheckGameDll(BYTE* gx) {
    __try {
        if (*(DWORD*)(gx + RVA_TIME_INGAME) == 0 && *(DWORD*)(gx + RVA_GAMELOCAL) == 0) return false;
        BYTE* gl = *(BYTE**)(gx + RVA_GAMELOCAL);
        if (gl && *(DWORD*)gl != (DWORD)(gx + RVA_LOCALPLAYER_VT)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static BYTE* LocalPlayer(BYTE* gx) {
    BYTE* gl = *(BYTE**)(gx + RVA_GAMELOCAL);
    if (!gl) return nullptr;
    BYTE* pl = *(BYTE**)(gl + GL_LOCALPLAYER);
    if (!pl || *(DWORD*)pl != (DWORD)(gx + RVA_LOCALPLAYER_VT)) return nullptr;
    return pl;
}

static void ReadGame(Snapshot* s, bool writeGod) {
    memset(s, 0, sizeof *s);
    __try {
        if (g_session) {
            BYTE* ssn = *(BYTE**)g_session;
            if (ssn) s->loading = ssn[0x20] != 0;
        }
        BYTE* gx = (BYTE*)GetModuleHandleA("Gamex86.dll");
        if (!gx || !g_supported) return;
        BYTE* gl = *(BYTE**)(gx + RVA_GAMELOCAL);
        if (!gl) return;
        s->cinematic = gl[GL_CINEMATIC] != 0;
        BYTE* mc = *(BYTE**)(gl + GL_MISSIONCOMPLETE);
        if (mc) s->mission = mc[MC_ACTIVE] != 0;
        s->paused |= s->cinematic || s->mission;
        const char* m = *(const char**)(gx + RVA_GAMELOCAL + GL_MAPNAME);
        if (m) {
            const char* base = m;
            for (const char* c = m; *c && c - m < 256; c++) if (*c == '/' || *c == '\\') base = c + 1;
            int n = 0;
            while (base[n] && base[n] != '.' && n < 63) { s->map[n] = base[n]; n++; }
        }
        BYTE* pl = LocalPlayer(gx);
        if (!pl) return;
        BYTE* ph = *(BYTE**)(pl + PL_PHYSICS);
        if (!ph || (*(DWORD**)ph)[38] != (DWORD)(gx + RVA_PHYS_GETORIGIN)) return;
        memcpy(s->pos, ph + PH_ORIGIN, 12);
        memcpy(s->vel, ph + PH_VELOCITY, 12);
        s->hp = *(float*)(pl + PL_HEALTH);
        s->player = true;
        if (writeGod && s->hp > 0) pl[PL_GODMODE] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->player = false;
    }
}

static void ExecCommand(const std::string& text) {
    __try {
        BYTE* gx = (BYTE*)GetModuleHandleA("Gamex86.dll");
        if (!gx || !g_supported) return;
        void* cs = *(void**)(gx + RVA_CMDSYSTEM);
        if (!cs) return;
        typedef void(__thiscall * Buf_t)(void*, int, const char*);
        ((Buf_t)(*(void***)cs)[CMD_BUFFERTEXT_IDX])(cs, CMD_EXEC_APPEND, text.c_str());
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ExecCommandList(const std::string& list) {
    std::string buf;
    for (char c : list) buf += (c == ';') ? '\n' : c;
    if (!buf.empty()) ExecCommand(buf + "\n");
}

static bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// SPRINT ANIMATION FIX:
// When head bob is disabled, setting g_noViewBob 1 breaks the sprint animation.
// By keeping g_noViewBob 0 and setting camera bob angles (pm_bobpitch, pm_bobroll, pm_bobup) to 0,
// the sprint arm animation continues to play completely naturally without camera shake!
static void ApplyFovAndBob() {
    char cmd[256];
    if (g_set.headBob) {
        snprintf(cmd, sizeof cmd,
            "set g_fov %d\n"
            "set g_noViewBob 0\n"
            "set pm_bobpitch 0.002\n"
            "set pm_bobroll 0.002\n"
            "set pm_bobup 0.005\n",
            g_set.fov);
    } else {
        snprintf(cmd, sizeof cmd,
            "set g_fov %d\n"
            "set g_noViewBob 0\n"
            "set pm_bobpitch 0\n"
            "set pm_bobroll 0\n"
            "set pm_bobup 0\n",
            g_set.fov);
    }
    ExecCommandList(cmd);
}

static void FinishRun() {
    g_run.Finish();
    std::string catKey = GetPbCategoryKey(g_set);
    bool better = false;
    if (g_pbSplits.empty() || g_run.igt < g_pbSplits.back().time) better = true;
    if (!g_run.practice && better) {
        WritePbSplits(catKey, g_run.splits);
        Message("NEW RECORD! " + FormatTime(g_run.igt, 3, false));
    } else Message("Run finished: " + FormatTime(g_run.igt, 2, false));
}

static void StartNewRunFromWizard() {
    g_run.Reset();
    memset(g_allMissionsDone, 0, sizeof(g_allMissionsDone));
    g_run.practice = false;
    g_maxSpeed = 0;
    std::string catKey = GetPbCategoryKey(g_set);
    g_pb = ReadPb(catKey, g_set.startMap);
    g_pbSplits = ReadPbSplits(catKey);

    char buf[512];
    if (g_set.category == "il") {
        g_ilPendingLoadout = g_set.ilNewGamePlus;
        snprintf(buf, sizeof buf,
            "set g_skill %d\n"
            "map %s\n",
            g_set.difficulty, kIlLevelPaths[g_set.ilLevelIdx]);
        ExecCommandList(buf);
        Message("IL RUN: " + std::string(kIlLevelNames[g_set.ilLevelIdx]) + (g_set.ilNewGamePlus ? " (NG+)" : ""));
    } else {
        snprintf(buf, sizeof buf,
            "set g_skill %d\n"
            "NewCampaignHasStarted\n"
            "set g_postVideoCommand \"map game/trainyard/trainyard\"\n"
            "PlayVideo videos/wolf_campaign_start_vo_%s\n",
            g_set.difficulty, g_set.language.c_str());
        ExecCommandList(buf);
        Message("SPEEDRUN LAUNCH: " + GetFullCategoryDisplay(g_set));
    }

    if (g_set.ruleset == RULESET_RESTRICTED) {
        ExecCommand("unbind F5\nunbind F9\n");
    }

    ApplyFovAndBob();

    Log("New Run Initialized: category=%s, difficulty=%d, ruleset=%d",
        g_set.category.c_str(), g_set.difficulty, g_set.ruleset);
}

static const char* GetKeyDisplayName(int vk) {
    static char buf[32];
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, sizeof buf, "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(buf, sizeof buf, "Num %d", vk - VK_NUMPAD0); return buf; }
    if (vk >= '0' && vk <= '9') { snprintf(buf, sizeof buf, "%c", (char)vk); return buf; }
    if (vk >= 'A' && vk <= 'Z') { snprintf(buf, sizeof buf, "%c", (char)vk); return buf; }
    switch (vk) {
    case VK_TAB: return "Tab"; case VK_RETURN: return "Enter"; case VK_ESCAPE: return "Esc";
    case VK_SPACE: return "Space"; case VK_LEFT: return "Left"; case VK_UP: return "Up";
    case VK_RIGHT: return "Right"; case VK_DOWN: return "Down"; case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End";
    case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down";
    case VK_OEM_3: return "~ (Tilde)"; case VK_OEM_MINUS: return "-"; case VK_OEM_PLUS: return "+";
    case VK_LBUTTON: return "Mouse 1"; case VK_RBUTTON: return "Mouse 2"; case VK_MBUTTON: return "Mouse 3";
    default: snprintf(buf, sizeof buf, "Key 0x%02X", vk); return buf;
    }
}

// Window procedure hook for in-game menu and wizard input capture
static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_rebindKeyIndex >= 0) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (wParam != VK_ESCAPE) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_set.keys[g_rebindKeyIndex] = (int)wParam;
                SaveSettings(g_set);
            }
            g_rebindKeyIndex = -1;
            return 0;
        }
    }

    if (msg == WM_KEYDOWN) {
        if ((int)wParam == g_set.keys[K_MENU]) {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) g_wizardOpen = false;
            return 0;
        }
        if ((int)wParam == g_set.keys[K_NEWRUN]) {
            g_wizardOpen = !g_wizardOpen;
            if (g_wizardOpen) g_menuOpen = false;
            return 0;
        }
    }

    // RESTRICTED MODE ENFORCEMENT:
    // Drop Quicksave & Quickload keystrokes physically before game engine can process them
    if (g_set.ruleset == RULESET_RESTRICTED && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if ((int)wParam == g_set.keys[K_QUICKSAVE] || (int)wParam == g_set.keys[K_QUICKLOAD] ||
            wParam == VK_F5 || wParam == VK_F9) {
            Message("[RESTRICTED] Quicksaves & Quickloads are FORBIDDEN!");
            return 0;
        }
    }

    if (g_menuOpen || g_wizardOpen) {
        if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
            g_menuOpen = false;
            g_wizardOpen = false;
            return 0;
        }
        if (g_imguiInit && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return 1;
        // Suppress gameplay input when menu or wizard is active
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)) {
            return 0;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

// ---- Poll thread: timers, hotkeys, ini reload ------------------------------------------------
static void Poll() {
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    bool keyDown[K_COUNT] = {};
    FILETIME iniTime = {};
    DWORD lastIniCheck = 0, loadEndTick = 0;
    bool cheatsPending = false, wasLoading = false, wasMission = false, wasPaused = false;
    float savedPos[3] = {}; bool haveSaved = false;

    for (;;) {
        Sleep(1);
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        last = now;

        DWORD tick = GetTickCount();
        if (tick - lastIniCheck > 500) {
            lastIniCheck = tick;
            WIN32_FILE_ATTRIBUTE_DATA ad;
            if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &ad)) {
                if (CompareFileTime(&iniTime, &ad.ftLastWriteTime)) {
                    iniTime = ad.ftLastWriteTime;
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_set = LoadSettings();
                    g_fontsDirty = true;
                }
            }
        }

        std::string cmds;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            bool cheat = (g_set.category == "cheat" && g_set.ruleset != RULESET_RESTRICTED);
            Snapshot s; ReadGame(&s, cheat && g_set.cheatGod);

            g_run.SetLoading(s.loading);
            g_run.paused = s.paused;
            if (s.mission && !wasMission) {
                CheckAllMissionComplete(g_run.map);
                bool isFinalLevel = (g_run.map == "blacksun" || g_run.map.find("blacksun") != std::string::npos);
                bool allMissionsFinished = (g_set.category == "all_missions" && GetAllMissionsCompletedCount() >= kAllMissionsCount);
                if (g_run.state == Run::Running && (isFinalLevel || allMissionsFinished || g_set.category == "il")) {
                    FinishRun();
                    if (allMissionsFinished) Message("ALL 18 MISSIONS COMPLETE! Run finished!");
                } else {
                    g_run.MissionComplete();
                }
            }
            if (s.paused != wasPaused)
                Log("pause=%d (cinematic=%d mission=%d bink=%ld) map=%s", s.paused, s.cinematic, s.mission, g_binkOpen, s.map);
            wasMission = s.mission; wasPaused = s.paused;
            g_run.Tick(dt);
            if (wasLoading && !s.loading) loadEndTick = GetTickCount();
            wasLoading = s.loading;

            // Map change is decided once the load has finished and the name is readable.
            bool menu = !strcmp(s.map, "menu") && !g_run.map.empty();
            if (!s.loading && s.map[0] && !menu && g_run.map != s.map) {
                std::string done; double t;
                std::string catKey = GetPbCategoryKey(g_set);
                if (g_run.MapLoaded(s.map, &done, &t)) {
                    CheckAllMissionComplete(done);
                    double pb = ReadPb(catKey, done);
                    if (pb < 0 || t < pb) {
                        char v[32]; snprintf(v, sizeof v, "%.3f", t);
                        WritePrivateProfileStringA(catKey.c_str(), done.c_str(), v, g_pbIni);
                        Message("New PB on " + done + ": " + FormatTime(t, 3, false));
                        if (!g_run.splits.empty() && g_run.splits.back().map == done) g_run.splits.back().gold = true;
                    }
                }
                // Auto-start on entering start_map (only if explicitly enabled for IL practice).
                // WARP PROTECTION: If a run is ALREADY RUNNING, map changes NEVER restart or reset the run!
                if (g_set.autoStartMap && g_run.state != Run::Running && !g_set.startMap.empty() &&
                    _stricmp(g_run.map.c_str(), g_set.startMap.c_str()) == 0) {
                    g_run.Start(); g_run.mapClean = true;
                    g_pbSplits = ReadPbSplits(catKey);
                    Message("Run started (map load: " + g_set.startMap + ")");
                }
                g_pb = ReadPb(catKey, g_run.map);
                g_maxSpeed = 0;
                cheatsPending = true;

                // Re-apply FOV and Head Bobbing on map transition
                ApplyFovAndBob();
            }

            g_havePlayer = s.player;
            if (s.player) {
                memcpy(g_pos, s.pos, sizeof g_pos);
                g_speed = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
                if (g_speed > g_maxSpeed) g_maxSpeed = g_speed;
                g_run.SetHealth(s.hp);

                // Auto New Game+ Loadout for IL runs
                if (g_ilPendingLoadout && s.hp > 0 && GetTickCount() - loadEndTick > 600) {
                    cmds += "give all\n"
                            "givePowers\n"
                            "giveALLPowerUpgrades\n"
                            "giveAllIntel\n"
                            "giveGold 100000\n"
                            "give ammocomponent\n";
                    g_ilPendingLoadout = false;
                    g_run.Start();
                    Message("IL LOADOUT DEPLOYED (New Game+)");
                }

                if (cheat && cheatsPending && s.hp > 0 && GetTickCount() - loadEndTick > 1000) {
                    cmds += g_set.cheatCmds + "\n"; cheatsPending = false;
                }
            }

            if (GameFocused() && !g_menuOpen && !g_wizardOpen) {
                for (int k = 0; k < K_COUNT; k++) {
                    if (k == K_MENU || k == K_NEWRUN) continue; // Handled by hkWndProc
                    bool down = (GetAsyncKeyState(g_set.keys[k]) & 0x8000) != 0;
                    bool pressed = down && !keyDown[k];
                    keyDown[k] = down;
                    if (!pressed) continue;
                    switch (k) {
                    case K_TOGGLE: g_visible = !g_visible; break;
                    case K_STARTSTOP:
                        if (g_run.state == Run::Running) FinishRun();
                        else {
                            g_run.Start();
                            memset(g_allMissionsDone, 0, sizeof(g_allMissionsDone));
                            g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
                            Message("Run started (" + GetFullCategoryDisplay(g_set) + ")");
                        }
                        break;
                    case K_RESET: g_run.Reset(); memset(g_allMissionsDone, 0, sizeof(g_allMissionsDone)); g_maxSpeed = 0; Message("Reset"); break;
                    case K_CATEGORY: {
                        int curIdx = 0;
                        for (int i = 0; i < CAT_COUNT; i++) {
                            if (g_set.category == kCategoryKeys[i]) { curIdx = i; break; }
                        }
                        g_set.category = kCategoryKeys[(curIdx + 1) % CAT_COUNT];
                        SaveSettings(g_set);
                        std::string catKey = GetPbCategoryKey(g_set);
                        g_pb = ReadPb(catKey, g_run.map);
                        g_pbSplits = ReadPbSplits(catKey);
                        if (g_set.category == "cheat") cheatsPending = true;
                        Message("Category: " + GetFullCategoryDisplay(g_set));
                        break;
                    }
                    case K_SAVEPOS:
                        if (s.player) { memcpy(savedPos, s.pos, sizeof savedPos); haveSaved = true; Message("Position saved"); }
                        break;
                    case K_TELEPORT:
                        if (haveSaved) {
                            char cmd[128];
                            snprintf(cmd, sizeof cmd, g_set.teleportCmd.c_str(), savedPos[0], savedPos[1], savedPos[2]);
                            cmds += std::string(cmd) + "\n";
                            g_run.Teleported();
                            Message("Teleported (Practice)");
                        }
                        break;
                    case K_QUICKSAVE:
                        if (g_set.ruleset == RULESET_RESTRICTED) {
                            Message("[RESTRICTED] Quicksave is FORBIDDEN!");
                        } else {
                            cmds += "contentManagerCommand save full\n";
                            Message("Quick Save");
                        }
                        break;
                    case K_QUICKLOAD:
                        if (g_set.ruleset == RULESET_RESTRICTED) {
                            Message("[RESTRICTED] Quick Load is FORBIDDEN!");
                        } else {
                            cmds += "contentManagerCommand loadLast\n";
                            Message("Quick Load");
                        }
                        break;
                    case K_RESTART_CP:
                        cmds += "ScaleformDiedRestart\n";
                        Message("Restart Checkpoint");
                        break;
                    }
                }
            }
        }
        if (!cmds.empty()) ExecCommandList(cmds);
    }
}

// ---- Rendering: Authentic Wolfenstein Military Instrumentation HUD --------------------------
static D3DXCreateFontA_t g_createFont;
static std::map<int, ID3DXFont*> g_fonts;

static void ReleaseFonts() { for (auto& f : g_fonts) f.second->Release(); g_fonts.clear(); }

static ID3DXFont* Font(IDirect3DDevice9* dev, int size, const std::string& face) {
    auto it = g_fonts.find(size);
    if (it != g_fonts.end()) return it->second;
    ID3DXFont* f = nullptr;
    if (g_createFont) g_createFont(dev, size, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str(), &f);
    if (f) g_fonts[size] = f;
    return f;
}

static std::string WidgetText(int id, const Settings& st) {
    char b[160];
    auto T = [&](double t) { return FormatTime(t, st.decimals, st.hoursAlways); };
    switch (id) {
    case W_COORDS:
        if (!g_havePlayer) return "";
        snprintf(b, sizeof b, "X:%.1f Y:%.1f Z:%.1f", g_pos[0], g_pos[1], g_pos[2]); return b;
    case W_SPEED: snprintf(b, sizeof b, "%.0f u/s (max %.0f)", g_speed, g_maxSpeed); return b;
    case W_RTA: return T(g_run.rta);
    case W_IGT: return T(g_run.igt) + (g_run.loading ? " [LOAD]" : g_run.paused ? " [PAUSED]" : "");
    case W_MAP_RTA: return T(g_run.mapRta);
    case W_MAP_IGT: return T(g_run.mapIgt);
    case W_PB:
        if (g_pb < 0) return "--";
        return T(g_pb) + "  " + FormatDelta(g_run.mapIgt - g_pb, st.decimals);
    case W_LOADS: snprintf(b, sizeof b, "%d (", g_run.loads); return b + T(g_run.loadTime) + ")";
    case W_DEATHS: return std::to_string(g_run.deaths);
    case W_CATEGORY: return GetFullCategoryDisplay(st) + (g_run.practice ? "  [PRACTICE]" : "");
    case W_MAP: return g_run.map;
    case W_FPS: snprintf(b, sizeof b, "%.0f FPS", g_fps); return b;
    }
    return "";
}

static void FillRect(IDirect3DDevice9* dev, int x0, int y0, int x1, int y1, D3DCOLOR color) {
    if (!(color >> 24)) return;
    struct V { float x, y, z, rhw; D3DCOLOR c; } v[4] = {
        {(float)x0, (float)y0, 0, 1, color}, {(float)x1, (float)y0, 0, 1, color},
        {(float)x0, (float)y1, 0, 1, color}, {(float)x1, (float)y1, 0, 1, color}};
    dev->SetPixelShader(nullptr); dev->SetVertexShader(nullptr); dev->SetTexture(0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

// Authentic military instrumentation plate frame with beveled edges and corner brackets
static void DrawMilitaryFrame(IDirect3DDevice9* dev, int x0, int y0, int x1, int y1, D3DCOLOR bg, D3DCOLOR border, D3DCOLOR bracket) {
    FillRect(dev, x0, y0, x1, y1, bg);
    FillRect(dev, x0, y0, x1, y0 + 1, border);
    FillRect(dev, x0, y0, x0 + 1, y1, border);
    FillRect(dev, x0, y1 - 1, x1, y1, 0x90000000);
    FillRect(dev, x1 - 1, y0, x1, y1, 0x90000000);
    if (bracket >> 24) {
        int bLen = 5;
        FillRect(dev, x0, y0, x0 + bLen, y0 + 2, bracket);
        FillRect(dev, x0, y0, x0 + 2, y0 + bLen, bracket);
        FillRect(dev, x1 - bLen, y0, x1, y0 + 2, bracket);
        FillRect(dev, x1 - 2, y0, x1, y0 + bLen, bracket);
        FillRect(dev, x0, y1 - 2, x0 + bLen, y1, bracket);
        FillRect(dev, x0, y1 - bLen, x0 + 2, y1, bracket);
        FillRect(dev, x1 - bLen, y1 - 2, x1, y1, bracket);
        FillRect(dev, x1 - 2, y1 - bLen, x1, y1, bracket);
    }
}

static void Text(ID3DXFont* f, const std::string& s, RECT r, DWORD fmt, D3DCOLOR color, bool shadow) {
    if (s.empty()) return;
    if (shadow) {
        RECT sh = {r.left + 2, r.top + 2, r.right + 2, r.bottom + 2};
        f->DrawTextA(nullptr, s.c_str(), -1, &sh, fmt | DT_NOCLIP, D3DCOLOR_ARGB((color >> 24) & 0xFF, 0, 0, 0));
    }
    f->DrawTextA(nullptr, s.c_str(), -1, &r, fmt | DT_NOCLIP, color);
}

static SIZE Measure(ID3DXFont* f, const std::string& s) {
    RECT r = {0, 0, 0, 0};
    if (!s.empty()) f->DrawTextA(nullptr, s.c_str(), -1, &r, DT_CALCRECT | DT_NOCLIP, 0);
    return SIZE{r.right, r.bottom};
}

static POINT Place(float fx, float fy, int anchor, int w, int h, int W, int H) {
    POINT p = {int(fx * W), int(fy * H)};
    if (anchor == 1 || anchor == 3) p.x -= w;
    if (anchor == 2 || anchor == 3) p.y -= h;
    if (anchor == 4) { p.x -= w / 2; p.y -= h / 2; }
    return p;
}

static void DrawLine(IDirect3DDevice9* dev, const std::string& face, int size, bool shadow,
                     const std::string& label, D3DCOLOR labelColor, const std::string& value, D3DCOLOR valueColor,
                     D3DCOLOR bg, float fx, float fy, int anchor, int W, int H) {
    ID3DXFont* f = Font(dev, size, face);
    if (!f || (label.empty() && value.empty())) return;
    SIZE a = Measure(f, label), b = Measure(f, value);
    int w = a.cx + b.cx, h = a.cy > b.cy ? a.cy : b.cy;
    int padX = 8, padY = 4;
    POINT p = Place(fx, fy, anchor, w, h, W, H);
    
    D3DCOLOR plateBg = (bg >> 24) ? bg : 0xDC121315;
    D3DCOLOR borderCol = 0x80545048;
    D3DCOLOR bracketCol = 0xFFA09A8E;
    
    DrawMilitaryFrame(dev, p.x - padX, p.y - padY, p.x + w + padX, p.y + h + padY, plateBg, borderCol, bracketCol);
    Text(f, label, RECT{p.x, p.y, p.x + a.cx, p.y + h}, DT_LEFT, labelColor, shadow);
    Text(f, value, RECT{p.x + a.cx, p.y, p.x + w, p.y + h}, DT_LEFT, valueColor, shadow);
}

static void DrawSplits(IDirect3DDevice9* dev, const Settings& st, const Widget& w, int W, int H) {
    ID3DXFont* f = Font(dev, w.size, st.font);
    if (!f || g_run.state == Run::Idle) return;
    auto T = [&](double t) { return FormatTime(t, st.decimals, st.hoursAlways); };
    struct Row { std::string col[4]; D3DCOLOR deltaColor; };
    std::vector<Row> rows;
    const auto& sp = g_run.splits;
    size_t first = sp.size() + 1 > (size_t)st.splitLines ? sp.size() + 1 - st.splitLines : 0;
    for (size_t i = first; i <= sp.size(); i++) {
        bool live = i == sp.size();
        if (live && g_run.state != Run::Running) break;
        std::string map = live ? g_run.map : sp[i].map;
        double time = live ? g_run.igt : sp[i].time;
        double seg = time - (i ? sp[i - 1].time : 0);
        Row r; r.col[0] = map; r.col[1] = T(seg); r.col[3] = T(time); r.deltaColor = w.timeColor;
        if (i < g_pbSplits.size() && g_pbSplits[i].map == map) {
            double d = time - g_pbSplits[i].time;
            r.col[2] = FormatDelta(d, st.decimals);
            r.deltaColor = d < 0 ? w.aheadColor : w.behindColor;
        }
        if (!live && sp[i].gold) r.deltaColor = w.goldColor;
        rows.push_back(r);
    }
    if (rows.empty()) return;

    int colW[4] = {}, lineH = 0, gap = w.size, pad = w.size / 4;
    for (auto& r : rows)
        for (int c = 0; c < 4; c++) {
            SIZE m = Measure(f, r.col[c].empty() ? " " : r.col[c]);
            if (m.cx > colW[c]) colW[c] = m.cx;
            if (m.cy > lineH) lineH = m.cy;
        }
    int totalW = colW[0] + colW[1] + colW[2] + colW[3] + gap * 3;
    int headerH = lineH + 4;
    int totalH = int(rows.size()) * lineH + headerH;
    POINT p = Place(w.x, w.y, w.anchor, totalW, totalH, W, H);

    DrawMilitaryFrame(dev, p.x - pad - 6, p.y - pad - 6, p.x + totalW + pad + 6, p.y + totalH + pad + 6, 0xEA121315, 0x80545048, 0xFFA09A8E);

    // Military dossier header
    Text(f, "// MISSION TELEMETRY //", RECT{p.x, p.y, p.x + totalW, p.y + lineH}, DT_LEFT, 0xFFA09A8E, st.shadow);
    FillRect(dev, p.x, p.y + lineH + 2, p.x + totalW, p.y + lineH + 3, 0x60545048);

    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        int y = p.y + headerH + int(i) * lineH, x = p.x;
        Text(f, r.col[0], RECT{x, y, x + colW[0], y + lineH}, DT_LEFT, w.nameColor, st.shadow);
        x += colW[0] + gap;
        Text(f, r.col[1], RECT{x, y, x + colW[1], y + lineH}, DT_RIGHT, w.segColor, st.shadow);
        x += colW[1] + gap;
        Text(f, r.col[2], RECT{x, y, x + colW[2], y + lineH}, DT_RIGHT, r.deltaColor, st.shadow);
        x += colW[2] + gap;
        Text(f, r.col[3], RECT{x, y, x + colW[3], y + lineH}, DT_RIGHT, w.timeColor, st.shadow);
    }
}

static void SetupWolfensteinTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 1.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowPadding = ImVec2(12, 10);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.09f, 0.08f, 0.08f, 0.96f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.06f, 0.06f, 0.06f, 0.70f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.09f, 0.09f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.35f, 0.32f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.14f, 0.12f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.24f, 0.18f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.09f, 0.08f, 1.00f);
    colors[ImGuiCol_Text]                 = ImVec4(0.90f, 0.88f, 0.84f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.52f, 0.48f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.20f, 0.18f, 0.16f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.38f, 0.18f, 0.16f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.48f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.14f, 0.13f, 0.12f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.20f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.24f, 0.22f, 1.00f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.14f, 0.13f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.32f, 0.22f, 0.18f, 1.00f);
    colors[ImGuiCol_TabSelected]            = ImVec4(0.28f, 0.18f, 0.16f, 1.00f);
    colors[ImGuiCol_TabDimmed]         = ImVec4(0.12f, 0.11f, 0.10f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]   = ImVec4(0.18f, 0.16f, 0.14f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.85f, 0.25f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.65f, 0.55f, 0.35f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.85f, 0.70f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.22f, 0.18f, 0.16f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.32f, 0.24f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.40f, 0.22f, 0.20f, 1.00f);
    colors[ImGuiCol_Separator]            = ImVec4(0.30f, 0.28f, 0.25f, 0.80f);
}

// BXT-Style New Run Setup Wizard
static void DrawNewRunWizard(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(800, 580), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 400.0f, io.DisplaySize.y * 0.5f - 290.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Wolfenstein (2009) Speedrun Setup - Mission Briefing", &g_wizardOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1.0f), "// SPEEDRUN CAMPAIGN SETUP WIZARD //");
        ImGui::TextDisabled("Select speedrun category, ruleset and target parameters before mission launch.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 1: CATEGORY SELECTION
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "STEP 1: Speedrun Category");
        int currentCatIdx = 0;
        for (int i = 0; i < CAT_COUNT; i++) {
            if (g_set.category == kCategoryKeys[i]) { currentCatIdx = i; break; }
        }
        for (int i = 0; i < CAT_COUNT; i++) {
            if (ImGui::RadioButton(kCategoryNames[i], currentCatIdx == i)) {
                currentCatIdx = i;
                g_set.category = kCategoryKeys[i];
                SaveSettings(g_set);
                std::string catKey = GetPbCategoryKey(g_set);
                g_pb = ReadPb(catKey, g_run.map);
                g_pbSplits = ReadPbSplits(catKey);
            }
            if (i < CAT_COUNT - 1) ImGui::SameLine();
        }

        ImGui::BeginChild("CategoryDesc", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Goal: %s", kCategorySummaries[currentCatIdx]);
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Timing: %s", kCategoryTimingRules[currentCatIdx]);
        if (g_set.category == "all_missions") {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[!] All Missions has 0 runs on speedrun.com! All 18 missions tracked automatically.");
        }
        ImGui::EndChild();

        // If IL is selected: Level chooser & New Game+ loadout
        if (g_set.category == "il") {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Individual Level (IL) Selection:");
            ImGui::Combo("Mission", &g_set.ilLevelIdx, kIlLevelNames, kIlLevelsCount);
            if (ImGui::Checkbox("New Game+ Loadout (Spawn with All Weapons, Ammo & Medallion Powers)", &g_set.ilNewGamePlus))
                SaveSettings(g_set);
            ImGui::TextDisabled("Leaderboard IL timing: starts upon player control, ends on mission complete dossier.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 2: RULESET SELECTION
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "STEP 2: Ruleset (speedrun.com 'Edits in config')");
        if (ImGui::RadioButton("Restricted (No wolf.cfg edits / Quicksaves blocked)", g_set.ruleset == RULESET_RESTRICTED)) {
            g_set.ruleset = RULESET_RESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map);
            g_pbSplits = ReadPbSplits(catKey);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Unrestricted (Quicksaves & Skips Allowed)", g_set.ruleset == RULESET_UNRESTRICTED)) {
            g_set.ruleset = RULESET_UNRESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map);
            g_pbSplits = ReadPbSplits(catKey);
        }

        ImGui::BeginChild("RulesetDesc", ImVec2(0, 70), true);
        if (g_set.ruleset == RULESET_RESTRICTED) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[!] RESTRICTED (speedrun.com): Quicksaves & Quickloads are BLOCKED.");
            ImGui::TextWrapped("Forbidden: console commands, wolf.cfg edits, infinite stamina cheat, in-game FPS glitch binds.");
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "ALLOWED: Warps (mission replay trick), skips without saves, FOV, Show FPS, Unlock FPS.");
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[*] UNRESTRICTED (speedrun.com):");
            ImGui::TextWrapped("Allowed: quicksaves (F5), quickloads (F9), mission replay warps, geometry clips and skips.");
            ImGui::TextDisabled("Forbidden: g_infiniteStamina 1 and console cheats (send run to Cheat%), in-game FPS binds.");
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 3: DIFFICULTY SELECTION
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "STEP 3: Difficulty (Сложность g_skill)");
        if (g_set.category == "100") {
            if (g_set.difficulty != DIFF_CASUAL && g_set.difficulty != DIFF_COUNT) {
                g_set.difficulty = DIFF_CASUAL;
            }
            if (ImGui::RadioButton("Easy (speedrun.com Leaderboard)", g_set.difficulty == DIFF_CASUAL)) {
                g_set.difficulty = DIFF_CASUAL; SaveSettings(g_set);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Cheat (Special Cheat Mod)", g_set.difficulty == DIFF_COUNT)) {
                g_set.difficulty = DIFF_COUNT; SaveSettings(g_set);
            }
            ImGui::TextDisabled("Note: speedrun.com 100%% leaderboard only features Easy and Cheat subcategories (no Uber).");
        } else {
            for (int i = 0; i < DIFF_COUNT; i++) {
                if (ImGui::RadioButton(kDifficultyNames[i], g_set.difficulty == i)) {
                    g_set.difficulty = i; SaveSettings(g_set);
                }
                if (i < DIFF_COUNT - 1) ImGui::SameLine();
            }
            ImGui::TextDisabled("Competitive community standard on speedrun.com is Easy or Uber difficulty.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // LAUNCH BUTTONS
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.2f, 1.0f), "Operation: %s", GetFullCategoryDisplay(g_set).c_str());
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.50f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.40f, 0.16f, 1.0f));
        if (ImGui::Button("START OPERATION NOW (Launch Run)", ImVec2(340, 36))) {
            StartNewRunFromWizard();
            g_wizardOpen = false;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)", ImVec2(120, 36))) {
            g_wizardOpen = false;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("Hotkey: [F2] opens this wizard anytime");
    }
    ImGui::End();
}

// In-Game Configuration & Cheats Menu
static void DrawInGameMenu(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(840, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 420.0f, io.DisplaySize.y * 0.5f - 300.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Wolfenstein (2009) Speedrun Mod - Tactical Operations & Cheats", &g_menuOpen, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("SettingsTabs")) {
            // TAB 1: Game & Visuals
            if (ImGui::BeginTabItem("Game & Visuals")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Field of View (FOV)");
                if (ImGui::SliderInt("##FOV", &g_set.fov, 60, 120, "%d deg")) {
                    SaveSettings(g_set); ApplyFovAndBob();
                }
                ImGui::TextDisabled("Allowed in speedrun.com Restricted & Unrestricted categories.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Head & Weapon Bobbing (Sprint Animation Preserved)");
                if (ImGui::Checkbox("Enable Head Bobbing", &g_set.headBob)) {
                    SaveSettings(g_set); ApplyFovAndBob();
                }
                ImGui::TextDisabled("Fixed: Turning off head bobbing eliminates camera shake without breaking the sprint arm animation!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Game Language (sys_lang)");
                int curLang = 0;
                for (int i = 0; i < kLangCount; i++) {
                    if (g_set.language == kLanguages[i]) { curLang = i; break; }
                }
                if (ImGui::Combo("Language", &curLang, kLanguages, kLangCount)) {
                    g_set.language = kLanguages[curLang];
                    SaveSettings(g_set);
                    ExecCommand(std::string("set sys_lang \"") + g_set.language + "\"\n");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Save & Checkpoint Controls");
                if (g_set.ruleset == RULESET_RESTRICTED) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[RESTRICTED ACTIVE]: Quicksaves are prohibited.");
                } else {
                    if (ImGui::Button("Quick Save (F5)", ImVec2(160, 26))) {
                        ExecCommand("contentManagerCommand save full\n"); Message("Quick Save");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Quick Load (F9)", ImVec2(160, 26))) {
                        ExecCommand("contentManagerCommand loadLast\n"); Message("Quick Load");
                    }
                }
                if (ImGui::Button("Restart Checkpoint", ImVec2(180, 26))) {
                    ExecCommand("ScaleformDiedRestart\n"); Message("Restart Checkpoint");
                }
                ImGui::EndTabItem();
            }

            // TAB 2: Cheats & Entity Spawner
            if (ImGui::BeginTabItem("Cheats & Spawner")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "Game Unlocks & DLC");
                if (ImGui::Button("Unlock FlameDLC (Flamethrower in First Room)")) {
                    ExecCommand("seta g_unlockedFlameDLC 1\n");
                    Message("FlameDLC Unlocked!");
                }
                ImGui::SameLine();
                if (ImGui::Button("Unlock Built-in Cheat Menu")) {
                    ExecCommand("seta g_showCheatMenuMsg 1\nseta g_gameFinished 1\n");
                    Message("Built-in Cheats Unlocked in Options!");
                }
                ImGui::SameLine();
                if (ImGui::Button("Unlock Dev Console (com_allowConsole)")) {
                    ExecCommand("set com_allowConsole 1\nseta g_cheatsAreOn 1\n");
                    Message("Console Unlocked! [R-Alt]+[L-Alt]+~");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "Player & Combat Cheats");
                if (ImGui::Button("Toggle God Mode (Invulnerability)")) {
                    ExecCommand("god\n"); Message("God Mode Toggled");
                }
                ImGui::SameLine();
                if (ImGui::Button("Toggle No Target (Enemies Ignore)")) {
                    ExecCommand("notarget\n"); Message("NoTarget Toggled");
                }
                ImGui::SameLine();
                if (ImGui::Button("Infinite Stamina")) {
                    ExecCommand("seta g_infiniteStamina 1\n"); Message("Infinite Stamina Enabled");
                }

                if (ImGui::Button("Give All Weapons & Ammo")) {
                    ExecCommand("give all\ngive ammocomponent\n"); Message("All Weapons & Ammo Given");
                }
                ImGui::SameLine();
                if (ImGui::Button("Give All Veil Powers & Medallion Upgrades")) {
                    ExecCommand("givePowers\ngiveALLPowerUpgrades\n"); Message("All Powers & Upgrades Given");
                }
                ImGui::SameLine();
                if (ImGui::Button("Give Max Gold ($100,000)")) {
                    ExecCommand("giveGold 100000\nmomoney\n"); Message("Max Gold Given");
                }
                if (ImGui::Button("Unlock All Intel & Objectives")) {
                    ExecCommand("giveAllIntel\ngiveObjectives\n"); Message("All Intel & Objectives Unlocked");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "Ammo Supply Dispensary");
                if (ImGui::Button("MP40")) ExecCommand("give ammo_mp40\n"); ImGui::SameLine();
                if (ImGui::Button("MP43")) ExecCommand("give ammo_mp43\n"); ImGui::SameLine();
                if (ImGui::Button("Kar98")) ExecCommand("give ammo_mauser\n"); ImGui::SameLine();
                if (ImGui::Button("Panzer")) ExecCommand("give ammo_panzer\n"); ImGui::SameLine();
                if (ImGui::Button("Particle Cannon")) ExecCommand("give ammo_particle_cannon\n"); ImGui::SameLine();
                if (ImGui::Button("Tesla")) ExecCommand("give ammo_tesla\n"); ImGui::SameLine();
                if (ImGui::Button("MDL24 Grenades")) ExecCommand("give ammo_mdl24\n");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "Entity Spawner (Enemies & Allies)");
                ImGui::Combo("Entity", &g_selectedSpawnEntity, kSpawnEntityNames, kSpawnEntitiesCount);
                if (ImGui::Button("SPAWN ENTITY NOW", ImVec2(220, 28))) {
                    std::string sp = std::string("spawn ") + kSpawnEntityIds[g_selectedSpawnEntity] + "\n";
                    ExecCommand(sp);
                    Message("Spawned: " + std::string(kSpawnEntityNames[g_selectedSpawnEntity]));
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "Tactical Map Warp");
                ImGui::Combo("Map", &g_selectedWarpMap, kIlLevelNames, kIlLevelsCount);
                if (ImGui::Button("WARP TO MAP", ImVec2(180, 28))) {
                    std::string wp = std::string("map ") + kIlLevelPaths[g_selectedWarpMap] + "\n";
                    ExecCommand(wp);
                    Message("Warping to: " + std::string(kIlLevelNames[g_selectedWarpMap]));
                }
                ImGui::EndTabItem();
            }

            // TAB 3: HUD Widgets & Styling
            if (ImGui::BeginTabItem("HUD & Telemetry")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Authentic Wolfenstein Instrumentation Gauges");
                ImGui::Checkbox("Master Overlay Visible", &g_visible);
                ImGui::Spacing();

                ImGui::Combo("Select Widget", &g_selectedWidget, kWidgetDisplayNames, W_COUNT);
                Widget& w = g_set.w[g_selectedWidget];
                if (ImGui::Checkbox("Gauge Enabled", &w.on)) SaveSettings(g_set);
                if (ImGui::SliderFloat("Position X", &w.x, 0.0f, 1.0f, "%.3f")) SaveSettings(g_set);
                if (ImGui::SliderFloat("Position Y", &w.y, 0.0f, 1.0f, "%.3f")) SaveSettings(g_set);
                if (ImGui::SliderInt("Size / Gauge Height", &w.size, 12, 48)) SaveSettings(g_set);

                char lblBuf[64]; snprintf(lblBuf, sizeof lblBuf, "%s", w.label.c_str());
                if (ImGui::InputText("Gauge Label", lblBuf, sizeof lblBuf)) {
                    w.label = lblBuf; SaveSettings(g_set);
                }
                ImGui::EndTabItem();
            }

            // TAB 4: Categories & Rules
            if (ImGui::BeginTabItem("Categories & Rules")) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.50f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.28f, 1.0f));
                if (ImGui::Button("Open Start New Run Wizard (F2)", ImVec2(280, 30))) {
                    g_wizardOpen = true; g_menuOpen = false;
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::TextDisabled("(Interactive category, ruleset & difficulty setup)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Speedrun Category");
                int currentCatIdx = 0;
                for (int i = 0; i < CAT_COUNT; i++) {
                    if (g_set.category == kCategoryKeys[i]) { currentCatIdx = i; break; }
                }
                for (int i = 0; i < CAT_COUNT; i++) {
                    if (ImGui::RadioButton(kCategoryNames[i], currentCatIdx == i)) {
                        currentCatIdx = i;
                        g_set.category = kCategoryKeys[i];
                        SaveSettings(g_set);
                        std::string catKey = GetPbCategoryKey(g_set);
                        g_pb = ReadPb(catKey, g_run.map);
                        g_pbSplits = ReadPbSplits(catKey);
                    }
                    if (i < CAT_COUNT - 1) ImGui::SameLine();
                }
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Goal: %s", kCategorySummaries[currentCatIdx]);
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Timing: %s", kCategoryTimingRules[currentCatIdx]);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Ruleset Enforcement");
                if (ImGui::RadioButton("Restricted (No Quicksave / Glitchless)", g_set.ruleset == RULESET_RESTRICTED)) {
                    g_set.ruleset = RULESET_RESTRICTED; SaveSettings(g_set);
                    std::string catKey = GetPbCategoryKey(g_set);
                    g_pb = ReadPb(catKey, g_run.map); g_pbSplits = ReadPbSplits(catKey);
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Unrestricted (All Skips & Quicksaves Allowed)", g_set.ruleset == RULESET_UNRESTRICTED)) {
                    g_set.ruleset = RULESET_UNRESTRICTED; SaveSettings(g_set);
                    std::string catKey = GetPbCategoryKey(g_set);
                    g_pb = ReadPb(catKey, g_run.map); g_pbSplits = ReadPbSplits(catKey);
                }
                if (g_set.ruleset == RULESET_RESTRICTED) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[!] RESTRICTED: Quicksaves & Quickloads are BLOCKED. Warps (mission replay) are ALLOWED. Cheats forbidden.");
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[*] UNRESTRICTED: Quicksaves, quickloads, mission replay warps and skips allowed. Stamina cheat/cheats forbidden.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Game Difficulty (g_skill)");
                if (g_set.category == "100") {
                    if (g_set.difficulty != DIFF_CASUAL && g_set.difficulty != DIFF_COUNT) {
                        g_set.difficulty = DIFF_CASUAL;
                    }
                    if (ImGui::RadioButton("Easy (speedrun.com Leaderboard)", g_set.difficulty == DIFF_CASUAL)) {
                        g_set.difficulty = DIFF_CASUAL; SaveSettings(g_set);
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Cheat (Cheat Mod)", g_set.difficulty == DIFF_COUNT)) {
                        g_set.difficulty = DIFF_COUNT; SaveSettings(g_set);
                    }
                    ImGui::TextDisabled("speedrun.com 100%% leaderboard only features Easy and Cheat categories (no Uber).");
                } else {
                    for (int i = 0; i < DIFF_COUNT; i++) {
                        if (ImGui::RadioButton(kDifficultyNames[i], g_set.difficulty == i)) {
                            g_set.difficulty = i; SaveSettings(g_set);
                        }
                        if (i < DIFF_COUNT - 1) ImGui::SameLine();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Warp Protection & Auto-Start");
                if (ImGui::Checkbox("Auto-start & reset on campaign cutscene (wolf_campaign_start_vo_*.bik)", &g_set.autoStartCutscene))
                    SaveSettings(g_set);
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "WARP PROTECTION: Mid-run mission replays/warps will NEVER reset an active run.");
                ImGui::EndTabItem();
            }

            // TAB 5: Records & Splits
            if (ImGui::BeginTabItem("Records & Splits")) {
                ImGui::Spacing();
                std::string catKey = GetPbCategoryKey(g_set);
                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "Best Run Splits - %s", GetFullCategoryDisplay(g_set).c_str());

                if (g_set.category == "all_missions") {
                    ImGui::Spacing();
                    int doneCount = GetAllMissionsCompletedCount();
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "All Missions Checklist (speedrun.com - %d / %d completed):", doneCount, kAllMissionsCount);
                    ImGui::BeginChild("AllMissionsChecklist", ImVec2(0, 140), true);
                    ImGui::Columns(2, "MissionsCols");
                    for (int i = 0; i < kAllMissionsCount; i++) {
                        if (g_allMissionsDone[i]) {
                            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[X] %s", kAllMissionsNames[i]);
                        } else {
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[ ] %s", kAllMissionsNames[i]);
                        }
                        if (i == (kAllMissionsCount / 2) - 1) ImGui::NextColumn();
                    }
                    ImGui::Columns(1);
                    ImGui::EndChild();
                    ImGui::Spacing();
                }

                if (g_pbSplits.empty()) {
                    ImGui::TextDisabled("No completed run saved in srmod_pb.ini for this category/ruleset yet.");
                } else {
                    ImGui::BeginChild("SplitsList", ImVec2(0, 220), true);
                    ImGui::Columns(3, "PbCols");
                    ImGui::Text("Map"); ImGui::NextColumn();
                    ImGui::Text("Segment"); ImGui::NextColumn();
                    ImGui::Text("Total Split Time"); ImGui::NextColumn();
                    ImGui::Separator();
                    for (const auto& sp : g_pbSplits) {
                        ImGui::Text("%s", sp.map.c_str()); ImGui::NextColumn();
                        ImGui::Text("%s", FormatTime(sp.seg, 2, false).c_str()); ImGui::NextColumn();
                        ImGui::Text("%s", FormatTime(sp.time, 2, false).c_str()); ImGui::NextColumn();
                    }
                    ImGui::Columns(1);
                    ImGui::EndChild();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (!g_confirmResetPb) {
                    if (ImGui::Button("Reset All PB Data for this Category")) g_confirmResetPb = true;
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Are you sure you want to erase all PBs in this category?");
                    ImGui::SameLine();
                    if (ImGui::Button("YES, ERASE")) {
                        std::string catKey = GetPbCategoryKey(g_set);
                        WritePrivateProfileStringA(catKey.c_str(), nullptr, nullptr, g_pbIni);
                        std::string sec = "splits." + catKey;
                        WritePrivateProfileStringA(sec.c_str(), nullptr, nullptr, g_pbIni);
                        g_pb = -1; g_pbSplits.clear();
                        g_confirmResetPb = false;
                        Message("PB data erased");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) g_confirmResetPb = false;
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Save Settings", ImVec2(140, 28))) {
            SaveSettings(g_set); Message("Settings saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close Menu (Esc / F1)", ImVec2(160, 28))) {
            g_menuOpen = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Map: %s | State: %s | FOV: %d | Bob: %s",
            g_run.map.empty() ? "None" : g_run.map.c_str(),
            g_run.state == Run::Running ? "RUNNING" : g_run.state == Run::Finished ? "FINISHED" : "IDLE",
            g_set.fov, g_set.headBob ? "ON" : "OFF");
    }
    ImGui::End();
}

static void DrawOverlay(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_fontsDirty) { ReleaseFonts(); g_fontsDirty = false; }

    IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    D3DVIEWPORT9 vp; dev->GetViewport(&vp);
    dev->GetRenderTarget(0, &oldRt);
    dev->SetRenderTarget(0, bb);

    // Frame time & FPS calculation
    g_frameCount++;
    DWORD nowTick = GetTickCount();
    if (nowTick - g_lastFpsTime >= 500) {
        g_fps = g_frameCount * 1000.0f / (float)(nowTick - g_lastFpsTime);
        g_frameCount = 0;
        g_lastFpsTime = nowTick;
    }

    // Initialize ImGui on first valid frame
    if (!g_imguiInit) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
            g_gameHwnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();
            if (g_gameHwnd) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                SetupWolfensteinTheme();

                ImGui_ImplWin32_Init(g_gameHwnd);
                ImGui_ImplDX9_Init(dev);

                g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
                g_imguiInit = true;
                Log("ImGui initialized on HWND %p with Wolfenstein Military theme", g_gameHwnd);
            }
        }
    }

    // MOUSE CURSOR HANDLING:
    // When menu or wizard is open, free the cursor from game lock and show it.
    // When closed, restore game cursor.
    bool menuNow = (g_menuOpen || g_wizardOpen);
    if (menuNow != g_wasMenuOpen) {
        if (menuNow) {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 1) {}
        } else {
            while (ShowCursor(FALSE) >= 0) {}
        }
        g_wasMenuOpen = menuNow;
    }

    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);

    if (SUCCEEDED(dev->BeginScene())) {
        int W = desc.Width, H = desc.Height;
        const Settings& st = g_set;

        // Draw Speedrun Overlay widgets (Authentic Military Instrumentation)
        if (g_visible) {
            if (!g_supported)
                DrawLine(dev, st.font, 22, true, "", 0, "srmod: unsupported game version (see srmod.log)", 0xFFFF5050,
                         0, 0.5f, 0.05f, 4, W, H);
            for (int i = 0; i < W_COUNT; i++) {
                const Widget& w = st.w[i];
                if (!w.on) continue;
                if (i == W_SPLITS) DrawSplits(dev, st, w, W, H);
                else DrawLine(dev, st.font, w.size, st.shadow, w.label, w.labelColor, WidgetText(i, st), w.color,
                              w.bg, w.x, w.y, w.anchor, W, H);
            }

            // Radio teletype alert banner
            if (GetTickCount() < g_msgUntil) {
                ID3DXFont* f = Font(dev, 22, st.font);
                if (f) {
                    std::string badge = "[ MILITARY TELEMETRY ]  " + g_msg;
                    SIZE m = Measure(f, badge);
                    int bx = (W - m.cx) / 2, by = int(0.18f * H);
                    DrawMilitaryFrame(dev, bx - 16, by - 6, bx + m.cx + 16, by + m.cy + 6, 0xF0121315, 0xFF8A3030, 0xFFE04040);
                    Text(f, badge, RECT{bx, by, bx + m.cx, by + m.cy}, DT_CENTER, 0xFFEDEAE2, true);
                }
            }

            // Menu status bar prompt
            if (!g_menuOpen && !g_wizardOpen) {
                bool isMainMenu = (g_run.map == "menu" || g_run.map.empty());
                std::string prompt = isMainMenu
                    ? "[F2] START SPEEDRUN WIZARD  |  [F1] SETTINGS & CHEATS"
                    : "[F1] Tactical Menu  |  [F2] Run Setup";
                D3DCOLOR promptColor = isMainMenu ? 0xFFFFD040 : 0x80FFFFFF;
                DrawLine(dev, st.font, 14, false, "", 0, prompt, promptColor, 0x50000000, 0.005f, 0.995f, 2, W, H);
            }
        }

        // Draw In-Game Menu and Wizard
        if (g_imguiInit) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = (g_menuOpen || g_wizardOpen);
            if (g_menuOpen) DrawInGameMenu(dev);
            if (g_wizardOpen) DrawNewRunWizard(dev);
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }

        dev->EndScene();
    }
    if (sb) { sb->Apply(); sb->Release(); }

    dev->SetRenderTarget(0, oldRt);
    dev->SetViewport(&vp);
    if (oldRt) oldRt->Release();
    bb->Release();
}

// ---- D3D9 hooks ------------------------------------------------------------------------------
typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Present_t oPresent;
static Reset_t oReset;

static HRESULT __stdcall hkPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    DrawOverlay(dev);
    return oPresent(dev, a, b, c, d);
}

static HRESULT __stdcall hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto& f : g_fonts) f.second->OnLostDevice();
        if (g_imguiInit) ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    HRESULT hr = oReset(dev, pp);
    std::lock_guard<std::mutex> lk(g_mx);
    if (SUCCEEDED(hr)) {
        for (auto& f : g_fonts) f.second->OnResetDevice();
        if (g_imguiInit) ImGui_ImplDX9_CreateDeviceObjects();
    } else g_fontsDirty = true;
    return hr;
}

static bool Hook(void** vtable, int idx, void* hook, void** orig) {
    BYTE* fn = (BYTE*)vtable[idx];
    DWORD old;
    bool pad = true;
    for (int i = 1; i <= 5; i++) pad &= fn[-i] == 0x90 || fn[-i] == 0xCC;
    if (fn[0] == 0x8B && fn[1] == 0xFF && pad) {
        VirtualProtect(fn - 5, 7, PAGE_EXECUTE_READWRITE, &old);
        *orig = fn + 2;
        fn[-5] = 0xE9; *(int*)(fn - 4) = int((BYTE*)hook - fn);
        *(WORD*)fn = 0xF9EB;
        VirtualProtect(fn - 5, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn - 5, 7);
        Log("hook %d: hotpatch", idx);
    } else {
        VirtualProtect(&vtable[idx], 4, PAGE_READWRITE, &old);
        *orig = vtable[idx]; vtable[idx] = hook;
        VirtualProtect(&vtable[idx], 4, old, &old);
        Log("hook %d: vtable", idx);
    }
    return true;
}

static void InitD3D(IDirect3DDevice9* dev) {
    HMODULE d3dx = LoadLibraryA("d3dx9_43.dll");
    if (!d3dx) {
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\d3dx9_43.dll", sys);
        d3dx = LoadLibraryA(p);
    }
    if (d3dx) g_createFont = (D3DXCreateFontA_t)GetProcAddress(d3dx, "D3DXCreateFontA");
    void** vt = *(void***)dev;
    Hook(vt, 17, (void*)hkPresent, (void**)&oPresent);
    Hook(vt, 16, (void*)hkReset, (void**)&oReset);
}

static void HookD3D9() {
    WNDCLASSA wc = {0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), 0, 0, 0, 0, "srmod_tmp"};
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("srmod_tmp", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, wc.hInstance, 0);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { DestroyWindow(hwnd); return; }
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        InitD3D(dev);
        dev->Release();
    }
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassA("srmod_tmp", wc.hInstance);
}

// ---- binkw32 proxy ---------------------------------------------------------------------------
static HMODULE g_binkOrig;

static void* BinkOrig(const char* name) {
    if (!g_binkOrig) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\binkw32_orig.dll", g_dir);
        g_binkOrig = LoadLibraryA(p);
        if (!g_binkOrig) {
            char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
            snprintf(p, sizeof p, "%s\\binkw32.dll", sys);
            g_binkOrig = LoadLibraryA(p);
        }
        if (!g_binkOrig) Log("FATAL: failed to load real binkw32.dll");
    }
    return g_binkOrig ? (void*)GetProcAddress(g_binkOrig, name) : nullptr;
}

extern "C" void* __stdcall SrBinkOpen(const char* file, unsigned flags) {
    static auto fn = (void*(__stdcall*)(const char*, unsigned))BinkOrig("_BinkOpen@8");
    void* h = fn ? fn(file, flags) : nullptr;
    if (h) InterlockedIncrement(&g_binkOpen);

    // Reset and start timer when campaign intro cutscene starts
    if (file) {
        std::string fnLower = file;
        for (char& c : fnLower) c = (char)tolower((unsigned char)c);
        if (fnLower.find("wolf_campaign_start") != std::string::npos) {
            std::lock_guard<std::mutex> lk(g_mx);
            // WARP PROTECTION:
            // If the runner is currently in an active run (state == Running, player exists, map != menu),
            // a warp trick or mid-run cutscene MUST NEVER RESET the timer!
            bool isMidRun = (g_run.state == Run::Running && g_havePlayer && g_run.map != "menu");
            if (isMidRun) {
                Log("WARP PROTECTION: suppressed cutscene timer reset during active run (map=%s)", g_run.map.c_str());
            } else if (g_set.autoStartCutscene) {
                // Runner clicked New Game from vanilla menu!
                // Open the in-game Speedrun Wizard directly so they can choose category & rules!
                if (!g_wizardOpen && g_run.state != Run::Running) {
                    g_wizardOpen = true;
                    g_menuOpen = false;
                    Log("Campaign start cutscene intercepted -> opened in-game Speedrun Wizard");
                }
                g_run.Start();
                memset(g_allMissionsDone, 0, sizeof(g_allMissionsDone));
                g_run.mapClean = true;
                g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
                Message("Run started (" + GetFullCategoryDisplay(g_set) + ")");
                Log("Campaign start cutscene opened: %s -> timer reset and started", file);
            }
        }
    }
    return h;
}

extern "C" void __stdcall SrBinkClose(void* h) {
    static auto fn = (void(__stdcall*)(void*))BinkOrig("_BinkClose@4");
    if (fn) fn(h);
    if (h && InterlockedDecrement(&g_binkOpen) < 0) InterlockedExchange(&g_binkOpen, 0);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        *strrchr(g_dir, '\\') = 0;
        snprintf(g_ini, MAX_PATH, "%s\\srmod.ini", g_dir);
        snprintf(g_pbIni, MAX_PATH, "%s\\srmod_pb.ini", g_dir);
        snprintf(g_log, MAX_PATH, "%s\\srmod.log", g_dir);
        DeleteFileA(g_log);

        Log("srmod attached, base=%s", g_dir);
        g_session = FindSession();
        Log("session=%p", g_session);

        g_set = LoadSettings();
        SaveSettings(g_set); // Ensures initial srmod.ini is created with defaults

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (int i = 0; i < 200; i++) {
                HMODULE gx = GetModuleHandleA("Gamex86.dll");
                if (gx && CheckGameDll((BYTE*)gx)) {
                    g_supported = true;
                    Log("Gamex86 verified (%d ms)", i * 50);
                    ApplyFovAndBob();
                    break;
                }
                Sleep(50);
            }
            if (!g_supported) Log("Gamex86 not verified, running in generic mode");
            HookD3D9();
            Poll();
            return 0;
        }, nullptr, 0, nullptr);
    }
    return TRUE;
}

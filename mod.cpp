// srmod - Wolfenstein (2009) Speedrun Mod & Tactical In-Game Suite
// Features: ImGui Tactical Suite (Advanced Configurator Style), High-DPI JetBrains Mono HUD,
// Speedrun.com Ruleset Enforcement, All Missions Tracker, Individual Levels (New Game+),
// Cheats & Spawner dispensary, Sprint-Safe Head Bobbing, Warp Protection, and Campaign Auto-Start.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <map>
#include <cctype>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "timer.h"

#pragma comment(lib, "winmm.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- Addresses (Wolfenstein 1.21 SP, patched Gamex86.dll 6739216 bytes) -------------------
const DWORD RVA_GAMELOCAL       = 0x61f0b0;  // idGameLocal gameLocal
const DWORD RVA_CMDSYSTEM       = 0x61f050;  // idCmdSystem *cmdSystem
const DWORD RVA_PHYS_GETORIGIN  = 0x2f8760;  // idPhysics_Player::GetOrigin
const DWORD GL_LOCALCLIENT      = 0x63d4c;   // int localClientNum
const DWORD GL_ENTITIES         = 0x4d0;     // idEntity *entities[]
const DWORD GL_MAPNAME          = 0x63e28;   // idStr mapFileName data pointer
const DWORD GL_INCINEMATIC      = 0x63bdc;   // bool inCinematic (set by SetCamera)
const DWORD RVA_MISSIONCOMPLETE = 0x875e8c;  // mission complete menu object
const DWORD MC_ACTIVE           = 0x268;     // bool: mission complete screen shown
const DWORD PL_PHYSICS          = 0x2ec;     // idPhysics *physics
const DWORD PL_HEALTH           = 0xd8;      // float health
const DWORD PL_GODMODE          = 0x5fa;     // bool godmode
const DWORD PH_ORIGIN           = 0x9c;      // idVec3 current.origin
const DWORD PH_VELOCITY         = 0xa8;      // idVec3 current.velocity
const int   CMD_BUFFERTEXT_IDX  = 8;
const int   CMD_EXEC_APPEND     = 2;
const DWORD WOLF_SESSION_VT     = 0x105e78cc;
const DWORD SESS_INSIDE_MAPCHANGE = 0xd;

struct Sig { DWORD rva; const char* bytes; int len; };
const Sig kSigs[] = {
    {0x06e1f0, "\x8B\x81\x28\x3E\x06\x00\xC3", 7},             // GetMapName
    {0x011460, "\x8B\x81\xEC\x02\x00\x00\xC3", 7},             // idPlayer::GetPhysics
    {0x2f8760, "\x8D\x81\x9C\x00\x00\x00\xC2\x04\x00", 9},     // idPhysics_Player::GetOrigin
};

// ---- Minimal D3DX font declarations ---------------------------------------------------------
struct ID3DXFont : IUnknown {
    virtual void __stdcall p0() = 0; virtual void __stdcall p1() = 0; virtual void __stdcall p2() = 0;
    virtual void __stdcall p3() = 0; virtual void __stdcall p4() = 0; virtual void __stdcall p5() = 0;
    virtual void __stdcall p6() = 0; virtual void __stdcall p7() = 0; virtual void __stdcall p8() = 0;
    virtual void __stdcall p9() = 0; virtual void __stdcall p10() = 0;
    virtual int  __stdcall DrawTextA(void* s, const char* str, int count, RECT* rect, DWORD format, D3DCOLOR color) = 0;
    virtual void __stdcall p12() = 0; virtual void __stdcall p13() = 0;
    virtual HRESULT __stdcall OnLostDevice() = 0;
    virtual HRESULT __stdcall OnResetDevice() = 0;
};
typedef HRESULT(__stdcall* D3DXCreateFontA_t)(IDirect3DDevice9*, int, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, const char*, ID3DXFont**);
static D3DXCreateFontA_t g_createFont;

// ---- Settings, Keys, Categories & Rulesets --------------------------------------------------
enum Key {
    K_MENU, K_NEWRUN, K_OVERLAY, K_STARTFINISH, K_RESET,
    K_SAVE_POS, K_LOAD_POS, K_QUICKSAVE, K_QUICKLOAD, K_RESTART_CP,
    K_COUNT
};
const char* kKeyNames[K_COUNT] = {
    "Menu", "NewRunWizard", "Overlay", "StartFinish", "Reset",
    "SavePos", "LoadPos", "QuickSave", "QuickLoad", "RestartCheckpoint"
};
const int kDefaultKeys[K_COUNT] = {
    VK_F1, VK_F2, VK_F6, VK_F7, VK_F8,
    VK_NUMPAD1, VK_NUMPAD2, VK_F5, VK_F9, VK_F11
};

enum Ruleset {
    RULESET_RESTRICTED = 0,   // speedrun.com competitive standard: quicksave/quickload blocked, no wolf.cfg cheats
    RULESET_UNRESTRICTED = 1  // speedrun.com unrestricted: quicksaves, loads, warps, skips allowed
};
const char* kRulesetNames[2] = { "Restricted", "Unrestricted" };

enum Category {
    CAT_ANY,
    CAT_ALL_MISSIONS,
    CAT_100,
    CAT_SEGMENTED,
    CAT_IL,
    CAT_CHEAT,
    CAT_COUNT
};
const char* kCategoryNames[CAT_COUNT] = { "Any%", "All Missions", "100%", "Segmented", "Individual Level (IL)", "Cheat% (Practice)" };
const char* kCategoryKeys[CAT_COUNT] = { "any", "all_missions", "100", "segmented", "il", "cheat" };
const char* kCategoryDescs[CAT_COUNT] = {
    "Complete the game from start to Black Sun finale as fast as possible.",
    "Complete all 18 campaign story and district missions. Currently 0 runs on speedrun.com!",
    "100% all 148 Gold pieces, 88 Intel dossiers, 16 Tomes of Power, and full weapon/Veil upgrades.",
    "Segmented multi-segment record run using optimal segmented saves.",
    "Individual level time attack with full New Game+ loadout (All Weapons & Veil Powers).",
    "Practice category with cheats, teleports, and custom console cvars."
};

enum Difficulty { DIFF_CASUAL = 0, DIFF_NORMAL = 1, DIFF_HARD = 2, DIFF_UBER = 3, DIFF_COUNT = 4 };
const char* kDifficultyNames[DIFF_COUNT] = { "Casual (Easy)", "Normal", "Hard", "Uber" };
const char* kDifficultyKeys[DIFF_COUNT]  = { "easy", "normal", "hard", "uber" };

// All 18 Missions for All Missions category
const char* kAllMissionsList[18] = {
    "trainyard", "sewers", "tavern", "digsite", "church", "farm", "hospital",
    "cannery", "castle", "airfield_west", "airfield_east", "zeppelin", "blacksun",
    "mtw_ware", "mtw_off", "mte_ss_hq", "mte_para_hq", "downtown_radio"
};
const int kAllMissionsCount = 18;

// 15 official speedrun.com Individual Levels (IL)
struct IlLevelDef { const char* name; const char* path; const char* id; };
const IlLevelDef kIlLevels[15] = {
    { "Train Station",          "game/trainyard/trainyard",          "trainyard" },
    { "Dig Site",               "game/digsite/digsite",              "digsite" },
    { "Church",                 "game/church/church",                "church" },
    { "Farm",                   "game/farm/farm",                    "farm" },
    { "Hospital",               "game/hospital/hospital",            "hospital" },
    { "Cannery",                "game/cannery/cannery",              "cannery" },
    { "Castle",                 "game/castle/castle",                "castle" },
    { "Airfield",               "game/airfield_west/airfield_west",  "airfield_west" },
    { "Zeppelin",               "game/zeppelin/zeppelin",            "zeppelin" },
    { "Black Sun Dimension",    "game/blacksun/blacksun",            "blacksun" },
    { "Warehouse",              "game/midtown_west/mtw_ware",        "mtw_ware" },
    { "Officer's House",        "game/midtown_west/mtw_off",         "mtw_off" },
    { "SS Headquarters",        "game/midtown_east/mte_ss_hq",       "mte_ss_hq" },
    { "SS Paranormal Base",     "game/midtown_east/mte_para_hq",     "mte_para_hq" },
    { "Radio Tower",            "game/downtown/downtown_radio",      "downtown_radio" }
};
const int kIlLevelsCount = 15;
const char* kIlLevelNames[15] = {
    "Train Station", "Dig Site", "Church", "Farm", "Hospital",
    "Cannery", "Castle", "Airfield", "Zeppelin", "Black Sun Dimension",
    "Warehouse", "Officer's House", "SS Headquarters", "SS Paranormal Base", "Radio Tower"
};
const char* kIlLevelPaths[15] = {
    "game/trainyard/trainyard", "game/digsite/digsite", "game/church/church",
    "game/farm/farm", "game/hospital/hospital", "game/cannery/cannery",
    "game/castle/castle", "game/airfield_west/airfield_west", "game/zeppelin/zeppelin",
    "game/blacksun/blacksun", "game/midtown_west/mtw_ware", "game/midtown_west/mtw_off",
    "game/midtown_east/mte_ss_hq", "game/midtown_east/mte_para_hq", "game/downtown/downtown_radio"
};

const char* kLanguages[] = { "english", "russian", "french", "german", "italian", "spanish", "polish" };
const int kLangCount = 7;

// Entity spawner definitions
const char* kSpawnEntityNames[] = {
    "SS Soldier (monster_nazi_officer)",
    "Veil Heavy Trooper (monster_heavy_trooper)",
    "Altered Mutant (monster_altered)",
    "Veil Assassin (monster_veil_assassin)",
    "Elite Guard (monster_elite_guard)",
    "Geist Creature (monster_geist)",
    "Scribe (monster_scribe)",
    "General Zetta (monster_boss_deathhead)",
    "Queen Geist (monster_boss_geistsmith)",
    "Hans Grosse (monster_boss_hansgrosse)",
    "Golden Dawn Ally (monster_golden_dawn)",
    "Kreisau Circle Ally (monster_kreisau)",
    "Rocket Trooper (monster_rocket_trooper)",
    "Flame Trooper (monster_flame_trooper)",
    "Despoiled (monster_despoiled)",
    "Sniffer (monster_sniffer)",
    "Particle Weapon Crate (weapon_particle_cannon)",
    "Tesla Weapon Crate (weapon_tesla)",
    "Panzerschreck Crate (weapon_panzer)"
};
const char* kSpawnEntityIds[] = {
    "monster_nazi_officer", "monster_heavy_trooper", "monster_altered",
    "monster_veil_assassin", "monster_elite_guard", "monster_geist",
    "monster_scribe", "monster_boss_deathhead", "monster_boss_geistsmith",
    "monster_boss_hansgrosse", "monster_golden_dawn", "monster_kreisau",
    "monster_rocket_trooper", "monster_flame_trooper", "monster_despoiled",
    "monster_sniffer", "weapon_particle_cannon", "weapon_tesla", "weapon_panzer"
};
const int kSpawnEntitiesCount = sizeof(kSpawnEntityNames) / sizeof(kSpawnEntityNames[0]);

enum WidgetId {
    W_TIMER, W_MAP_TIMER, W_TOTAL_IGT, W_MAP_IGT, W_SPLITS,
    W_MAP, W_CATEGORY, W_POS, W_SPEED, W_MAX_SPEED, W_LOADS, W_DEATHS,
    W_COUNT
};
const char* kWidgetNames[W_COUNT] = {
    "timer", "map_timer", "total_igt", "map_igt", "splits",
    "map", "category", "pos", "speed", "max_speed", "loads", "deaths"
};
const char* kWidgetDisplayNames[W_COUNT] = {
    "Timer (RTA)", "Map Timer (RTA)", "Total Timer (IGT)", "Map Timer (IGT)", "PB Splits Dossier",
    "Current Map", "Category & Ruleset", "Coordinates (X Y Z)", "Speedometer", "Peak Map Speed", "Load Counter", "Deaths Counter"
};

struct Widget {
    bool on; float x, y; int anchor, size;
    D3DCOLOR color, bg, labelColor;
    std::string label;
};

struct Settings {
    std::string font = "JetBrains Mono";
    bool shadow = true;
    Widget w[W_COUNT];
    int keys[K_COUNT];
    int fov = 100;
    bool headBob = false;
    std::string language = "english";
    std::string category = "any";
    int ruleset = RULESET_RESTRICTED;
    int difficulty = DIFF_CASUAL;
    std::string startMap = "trainyard";
    bool autoStartCutscene = true;
    bool autoStartIl = true;
    bool lockGod = false;
    std::string postLoadCmds = "";
    std::string cheatCmds = "god; give all; seta g_infiniteStamina 1; seta g_showCheatMenuMsg 1; seta g_gameFinished 1; set com_allowConsole 1";
    int splitsCount = 6;
    bool ilNewGamePlus = true;
    int ilLevelIdx = 0;
};

// Forward declarations
static void Log(const char* fmt, ...);
static void Message(const std::string& s);
static void SaveSettings(const Settings& s);
static Settings LoadSettings();
static void ExecCommand(const std::string& text);
static void ExecCommandList(const std::string& list);
static void ApplyFovAndBob();
static void StartNewRunFromWizard();
static void DrawInGameMenu(IDirect3DDevice9* dev);
static void DrawNewRunWizard(IDirect3DDevice9* dev);
static const char* GetKeyDisplayName(int vk);
static bool ToggleSwitch(const char* label, bool* v, const char* hint = nullptr);

// ---- Globals ---------------------------------------------------------------------------------
static char g_dir[MAX_PATH];
static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];
static char g_pbIni[MAX_PATH];
static Settings g_set;
static Run g_run;
static volatile long g_binkOpen;
static bool g_supported;
static bool g_visible = true;
static bool g_menuOpen = false;
static bool g_wizardOpen = false;
static bool g_wasMenuOpen = false;
static int  g_rebindKeyIndex = -1;
static int  g_selectedWidget = 0;
static int  g_selectedSpawnEntity = 0;
static int  g_selectedWarpMap = 0;
static std::mutex g_mx;
static std::string g_msg;
static DWORD g_msgUntil;
static float g_pos[3], g_speed, g_maxSpeed;
static float g_savePos[3];
static bool  g_hasSavedPos = false;
static bool  g_warpProtection = true;
static bool  g_havePlayer = false;
static bool  g_allMissionsDone[18] = { false };
static double g_pb = 0;
static std::vector<std::pair<std::string, double>> g_pbSplits;
static bool  g_imguiInit = false;
static HWND  g_gameHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool  g_ilPendingLoadout = false;

static std::map<int, ID3DXFont*> g_fonts;
static bool g_fontsDirty = false;

// ---- Utilities -------------------------------------------------------------------------------
static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_log, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
}

static void Message(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_msg = s;
    g_msgUntil = GetTickCount() + 3500;
}

static int GetAllMissionsCompletedCount() {
    int count = 0;
    for (int i = 0; i < kAllMissionsCount; i++) {
        if (g_allMissionsDone[i]) count++;
    }
    return count;
}

static std::string FormatTime(double sec, bool milli = true) {
    if (sec < 0) sec = 0;
    int isec = int(sec);
    int h = isec / 3600, m = (isec % 3600) / 60, s = isec % 60;
    int ms = int((sec - isec) * 1000 + 0.5);
    char b[64];
    if (milli) {
        if (h > 0) snprintf(b, sizeof b, "%d:%02d:%02d.%03d", h, m, s, ms);
        else       snprintf(b, sizeof b, "%02d:%02d.%03d", m, s, ms);
    } else {
        if (h > 0) snprintf(b, sizeof b, "%d:%02d:%02d", h, m, s);
        else       snprintf(b, sizeof b, "%02d:%02d", m, s);
    }
    return b;
}

static std::string FormatDelta(double diff) {
    char b[64];
    double a = fabs(diff);
    int isec = int(a);
    int m = isec / 60, s = isec % 60;
    int ms = int((a - isec) * 10);
    snprintf(b, sizeof b, "%c%d:%02d.%d", diff < 0 ? '-' : '+', m, s, ms);
    return b;
}

static std::string GetPbCategoryKey(const Settings& s) {
    std::string key = s.category;
    if (s.category == "any" || s.category == "100") {
        key += (s.ruleset == RULESET_RESTRICTED) ? "_restricted" : "_unrestricted";
        key += "_";
        key += kDifficultyKeys[s.difficulty];
    } else if (s.category == "il") {
        key += "_";
        key += kIlLevels[s.ilLevelIdx].id;
    }
    return key;
}

static std::string GetFullCategoryDisplay(const Settings& s) {
    std::string out;
    if (s.category == "any") out = "Any%";
    else if (s.category == "all_missions") out = "All Missions (18)";
    else if (s.category == "100") out = "100%";
    else if (s.category == "segmented") out = "Segmented";
    else if (s.category == "il") out = std::string("IL: ") + kIlLevelNames[s.ilLevelIdx];
    else if (s.category == "cheat") out = "Cheat% (Practice)";
    else out = s.category;

    if (s.category == "any" || s.category == "100") {
        out += (s.ruleset == RULESET_RESTRICTED) ? " (Restricted)" : " (Unrestricted)";
        out += " | ";
        out += kDifficultyNames[s.difficulty];
    }
    return out;
}

static double ReadPb(const std::string& catKey, const std::string& map) {
    if (map.empty() || map == "menu") return 0;
    char b[64];
    std::string section = "PB_" + catKey;
    GetPrivateProfileStringA(section.c_str(), map.c_str(), "0", b, sizeof b, g_pbIni);
    return atof(b);
}

static void WritePb(const std::string& catKey, const std::string& map, double t) {
    if (map.empty() || map == "menu") return;
    char b[64]; snprintf(b, sizeof b, "%.3f", t);
    std::string section = "PB_" + catKey;
    WritePrivateProfileStringA(section.c_str(), map.c_str(), b, g_pbIni);
}

static std::vector<std::pair<std::string, double>> ReadPbSplits(const std::string& catKey) {
    std::vector<std::pair<std::string, double>> res;
    char buf[2048] = { 0 };
    std::string section = "Splits_" + catKey;
    GetPrivateProfileSectionA(section.c_str(), buf, sizeof buf, g_pbIni);
    char* p = buf;
    while (*p) {
        char* eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            res.emplace_back(p, atof(eq + 1));
        }
        p += strlen(p) + 1;
    }
    return res;
}

static void WritePbSplits(const std::string& catKey, const std::vector<std::pair<std::string, double>>& splits) {
    std::string section = "Splits_" + catKey;
    WritePrivateProfileSectionA(section.c_str(), "", g_pbIni);
    for (auto& s : splits) {
        char b[64]; snprintf(b, sizeof b, "%.3f", s.second);
        WritePrivateProfileStringA(section.c_str(), s.first.c_str(), b, g_pbIni);
    }
}

// ---- Game Memory & Hooking -------------------------------------------------------------------
struct Snapshot { bool loading, paused, mission, cinematic, player; float pos[3], vel[3], hp; char map[64]; };
static BYTE* g_session = nullptr;

static BYTE* FindSession() {
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    if (!exe) return nullptr;
    __try {
        auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            if (memcmp(sec->Name, ".data", 5) != 0) continue;
            DWORD* p = (DWORD*)(exe + sec->VirtualAddress);
            DWORD n = sec->Misc.VirtualSize / 4;
            for (DWORD k = 0; k < n; k++) {
                if (p[k] == WOLF_SESSION_VT) return (BYTE*)&p[k];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static bool CheckGameDll(BYTE* gx) {
    for (auto& s : kSigs) {
        if (memcmp(gx + s.rva, s.bytes, s.len) != 0) return false;
    }
    return true;
}

static BYTE* LocalPlayer(BYTE* gx) {
    BYTE* gl = gx + RVA_GAMELOCAL;
    int idx = *(int*)(gl + GL_LOCALCLIENT);
    if (idx < 0 || idx >= 4096) return nullptr;
    return *(BYTE**)(gl + GL_ENTITIES + idx * 4);
}

static void ReadGame(Snapshot* s, bool writeGod) {
    memset(s, 0, sizeof *s);
    __try {
        if (g_session) s->loading = g_session[SESS_INSIDE_MAPCHANGE] != 0;
        BYTE* gx = (BYTE*)GetModuleHandleA("Gamex86.dll");
        s->paused = g_binkOpen > 0;
        if (!gx || !g_supported) return;
        s->cinematic = gx[RVA_GAMELOCAL + GL_INCINEMATIC] != 0;
        BYTE* mc = *(BYTE**)(gx + RVA_MISSIONCOMPLETE);
        if (mc) s->mission = mc[MC_ACTIVE] != 0;
        s->paused |= s->cinematic || s->mission;
        const char* m = *(const char**)(gx + RVA_GAMELOCAL + GL_MAPNAME);
        if (m) {
            const char* base = m;
            for (const char* c = m; *c && c - m < 256; c++) if (*c == '/' || *c == '\\') base = c + 1;
            int n = 0;
            while (base[n] && base[n] != '.' && n < (int)sizeof(s->map) - 1) { s->map[n] = base[n]; n++; }
            s->map[n] = 0;
        }
        BYTE* pl = LocalPlayer(gx);
        if (!pl) return;
        s->player = true;
        s->hp = *(float*)(pl + PL_HEALTH);
        if (writeGod) *(BYTE*)(pl + PL_GODMODE) = 1;
        BYTE* ph = *(BYTE**)(pl + PL_PHYSICS);
        if (!ph) return;
        typedef float*(__thiscall * Phys_t)(void*);
        void** vt = *(void***)ph;
        if ((DWORD)vt[1] == (DWORD)(gx + RVA_PHYS_GETORIGIN)) {
            memcpy(s->pos, (float*)vt[1], 12);
        } else {
            float* o = ((Phys_t)vt[1])(ph);
            if (o) memcpy(s->pos, o, 12);
        }
        float* v = *(float**)(ph + PH_VELOCITY);
        if (v) memcpy(s->vel, v, 12);
        else memcpy(s->vel, ph + PH_VELOCITY, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
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

static void ApplyFovAndBob() {
    char buf[256];
    snprintf(buf, sizeof buf, "seta g_fov %d\n", g_set.fov);
    ExecCommand(buf);

    if (g_set.headBob) {
        ExecCommand("seta g_noViewBob 0\nseta pm_bobpitch 0.002\nseta pm_bobroll 0.002\nseta pm_bobup 0.005\n");
    } else {
        ExecCommand("seta g_noViewBob 0\nseta pm_bobpitch 0\nseta pm_bobroll 0\nseta pm_bobup 0\n");
    }
}

static bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void FinishRun() {
    bool clean = g_run.mapClean;
    std::string map = g_run.map; double mapIgt = g_run.mapIgt;
    g_run.Finish();
    std::string catKey = GetPbCategoryKey(g_set);
    double pb = ReadPb(catKey, map);
    if (clean && (pb <= 0 || mapIgt < pb)) {
        WritePb(catKey, map, mapIgt);
        g_pb = mapIgt;
        Message("NEW PB FOR " + map + "! " + FormatTime(mapIgt));
    } else {
        Message("Map Finished: " + FormatTime(mapIgt));
    }
}

static void CheckAllMissionsProgress(const std::string& completedMap) {
    if (g_set.category != "all_missions") return;
    for (int i = 0; i < kAllMissionsCount; i++) {
        if (completedMap == kAllMissionsList[i]) {
            if (!g_allMissionsDone[i]) {
                g_allMissionsDone[i] = true;
                int done = GetAllMissionsCompletedCount();
                Message("MISSION COMPLETED: " + completedMap + " [" + std::to_string(done) + "/18]");
                if (done == 18) {
                    FinishRun();
                    Message("ALL 18 MISSIONS COMPLETE! TIME: " + FormatTime(g_run.igt));
                }
            }
            break;
        }
    }
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
        Message("OPERATION LAUNCH: " + GetFullCategoryDisplay(g_set));
    }

    if (g_set.ruleset == RULESET_RESTRICTED) {
        ExecCommand("unbind F5\nunbind F9\n");
    }

    ApplyFovAndBob();
    Log("New Run Initialized: category=%s, difficulty=%d, ruleset=%d",
        g_set.category.c_str(), g_set.difficulty, g_set.ruleset);
}

// Window message handler hook
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

    // Physical blocking of quicksaves/loads in Restricted Mode
    if (g_set.ruleset == RULESET_RESTRICTED && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if ((int)wParam == g_set.keys[K_QUICKSAVE] || (int)wParam == g_set.keys[K_QUICKLOAD] ||
            wParam == VK_F5 || wParam == VK_F9) {
            Message("[RESTRICTED ACTIVE] Quicksaves & Quickloads are FORBIDDEN!");
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
        // Suppress in-game player actions while menu/wizard is open
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)) {
            return 0;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

// Background poll loop
static void Poll() {
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    Snapshot prev = {};
    bool wasActive = false;
    DWORD lastLoadTick = 0, loadEndTick = 0;
    bool cheatsPending = false;

    while (true) {
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        last = now;

        if (!g_session) {
            g_session = FindSession();
            if (g_session) Log("Wolf2 Session object found at %p", g_session);
        }
        BYTE* gx = (BYTE*)GetModuleHandleA("Gamex86.dll");
        if (gx && !g_supported) {
            g_supported = CheckGameDll(gx);
            Log("Gamex86.dll at %p, supported=%d", gx, g_supported);
            if (g_supported) ApplyFovAndBob();
        }

        Snapshot s;
        bool cheat = (g_set.category == "cheat");
        bool lockGod = cheat && g_set.lockGod;
        ReadGame(&s, lockGod);

        std::string cmds;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_havePlayer = s.player;
            bool active = s.player && !s.loading && !s.paused;

            if (s.loading && !prev.loading) {
                lastLoadTick = GetTickCount();
                g_run.loads++;
            }
            if (!s.loading && prev.loading) {
                loadEndTick = GetTickCount();
                g_run.loadTime += (loadEndTick - lastLoadTick) / 1000.0;
                cheatsPending = true;
                if (!g_set.postLoadCmds.empty()) cmds += g_set.postLoadCmds + "\n";
            }

            if (s.map[0] && strcmp(s.map, g_run.map.c_str()) != 0) {
                std::string newMap = s.map;
                if (newMap == "menu") {
                    g_run.map = "menu";
                } else {
                    bool warpProtected = (g_run.state == Run::Running && (newMap == "trainyard" || newMap == "train_station"));
                    if (warpProtected) {
                        Log("WARP PROTECTION: Preserving run timer across map reload: %s", newMap.c_str());
                    } else if (g_set.autoStartIl && g_set.category == "il") {
                        g_run.Reset();
                        g_run.Start();
                        g_maxSpeed = 0;
                    }
                    g_run.map = newMap;
                    g_run.mapRta = 0; g_run.mapIgt = 0; g_run.mapClean = true;
                    g_maxSpeed = 0;
                    std::string catKey = GetPbCategoryKey(g_set);
                    g_pb = ReadPb(catKey, g_run.map);
                    g_pbSplits = ReadPbSplits(catKey);
                }
            }

            if (s.mission && !prev.mission) {
                FinishRun();
                CheckAllMissionsProgress(g_run.map);
            }

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
                    if (k == K_MENU || k == K_NEWRUN) continue;
                    int vk = g_set.keys[k];
                    if (vk && (GetAsyncKeyState(vk) & 1)) {
                        switch (k) {
                        case K_OVERLAY: g_visible = !g_visible; break;
                        case K_STARTFINISH:
                            if (g_run.state == Run::Running) g_run.Finish();
                            else {
                                g_run.Reset(); g_run.Start();
                                g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
                            }
                            break;
                        case K_RESET: g_run.Reset(); break;
                        case K_SAVE_POS:
                            if (s.player) {
                                memcpy(g_savePos, s.pos, sizeof g_savePos);
                                g_hasSavedPos = true;
                                Message("Position Saved: " + std::to_string((int)s.pos[0]) + " " + std::to_string((int)s.pos[1]));
                            }
                            break;
                        case K_LOAD_POS:
                            if (g_hasSavedPos && s.player) {
                                if (g_set.ruleset == RULESET_RESTRICTED) {
                                    Message("[RESTRICTED] Teleportation is blocked in Restricted mode!");
                                } else {
                                    char tpBuf[128];
                                    snprintf(tpBuf, sizeof tpBuf, "teleport %f %f %f\n", g_savePos[0], g_savePos[1], g_savePos[2]);
                                    cmds += tpBuf;
                                    g_run.practice = true;
                                    Message("Teleported to Saved Position [PRACTICE]");
                                }
                            }
                            break;
                        case K_QUICKSAVE:
                            if (g_set.ruleset == RULESET_RESTRICTED) Message("[RESTRICTED] Quicksaves are forbidden!");
                            else { cmds += "contentManagerCommand save full\n"; Message("Quick Save Created"); }
                            break;
                        case K_QUICKLOAD:
                            if (g_set.ruleset == RULESET_RESTRICTED) Message("[RESTRICTED] Quickloads are forbidden!");
                            else { cmds += "contentManagerCommand loadLast\n"; Message("Quick Load Loaded"); }
                            break;
                        case K_RESTART_CP:
                            cmds += "ScaleformDiedRestart\n"; Message("Restarting Checkpoint...");
                            break;
                        }
                    }
                }
            }

            g_run.SetLoading(s.loading); g_run.paused = s.paused; g_run.Tick(dt);
            prev = s;
            wasActive = active;
        }

        if (!cmds.empty()) ExecCommandList(cmds);
        Sleep(5);
    }
}

// ---- Settings Load & Save --------------------------------------------------------------------
static void SaveSettings(const Settings& s) {
    auto wstr = [](const char* k, const std::string& v) { WritePrivateProfileStringA("General", k, v.c_str(), g_ini); };
    auto wint = [](const char* k, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); WritePrivateProfileStringA("General", k, b, g_ini); };

    wstr("font", s.font);
    wint("shadow", s.shadow ? 1 : 0);
    wint("fov", s.fov);
    wint("head_bob", s.headBob ? 1 : 0);
    wstr("language", s.language);
    wstr("category", s.category);
    wint("ruleset", s.ruleset);
    wint("difficulty", s.difficulty);
    wstr("start_map", s.startMap);
    wint("auto_start_cutscene", s.autoStartCutscene ? 1 : 0);
    wint("auto_start_il", s.autoStartIl ? 1 : 0);
    wint("lock_god", s.lockGod ? 1 : 0);
    wstr("post_load_cmds", s.postLoadCmds);
    wstr("cheat_cmds", s.cheatCmds);
    wint("splits_count", s.splitsCount);
    wint("il_ng_plus", s.ilNewGamePlus ? 1 : 0);
    wint("il_level_idx", s.ilLevelIdx);

    for (int i = 0; i < W_COUNT; i++) {
        const Widget& w = s.w[i];
        char b[256];
        snprintf(b, sizeof b, "%d,%f,%f,%d,%d,%08X,%08X,%08X,%s",
                 w.on ? 1 : 0, w.x, w.y, w.anchor, w.size, w.color, w.bg, w.labelColor, w.label.c_str());
        WritePrivateProfileStringA("Widgets", kWidgetNames[i], b, g_ini);
    }

    for (int i = 0; i < K_COUNT; i++) {
        char b[32]; snprintf(b, sizeof b, "%d", s.keys[i]);
        WritePrivateProfileStringA("Keys", kKeyNames[i], b, g_ini);
    }
}

static Settings LoadSettings() {
    Settings s;
    char b[256];
    GetPrivateProfileStringA("General", "font", "JetBrains Mono", b, sizeof b, g_ini); s.font = b;
    s.shadow = GetPrivateProfileIntA("General", "shadow", 1, g_ini) != 0;
    s.fov = GetPrivateProfileIntA("General", "fov", 100, g_ini);
    s.headBob = GetPrivateProfileIntA("General", "head_bob", 0, g_ini) != 0;
    GetPrivateProfileStringA("General", "language", "english", b, sizeof b, g_ini); s.language = b;
    GetPrivateProfileStringA("General", "category", "any", b, sizeof b, g_ini); s.category = b;
    s.ruleset = GetPrivateProfileIntA("General", "ruleset", RULESET_RESTRICTED, g_ini);
    s.difficulty = GetPrivateProfileIntA("General", "difficulty", DIFF_CASUAL, g_ini);
    GetPrivateProfileStringA("General", "start_map", "trainyard", b, sizeof b, g_ini); s.startMap = b;
    s.autoStartCutscene = GetPrivateProfileIntA("General", "auto_start_cutscene", 1, g_ini) != 0;
    s.autoStartIl = GetPrivateProfileIntA("General", "auto_start_il", 1, g_ini) != 0;
    s.lockGod = GetPrivateProfileIntA("General", "lock_god", 0, g_ini) != 0;
    GetPrivateProfileStringA("General", "post_load_cmds", "", b, sizeof b, g_ini); s.postLoadCmds = b;
    GetPrivateProfileStringA("General", "cheat_cmds", "god; give all; seta g_infiniteStamina 1; seta g_showCheatMenuMsg 1; seta g_gameFinished 1; set com_allowConsole 1", b, sizeof b, g_ini); s.cheatCmds = b;
    s.splitsCount = GetPrivateProfileIntA("General", "splits_count", 6, g_ini);
    s.ilNewGamePlus = GetPrivateProfileIntA("General", "il_ng_plus", 1, g_ini) != 0;
    s.ilLevelIdx = GetPrivateProfileIntA("General", "il_level_idx", 0, g_ini);

    // Initial default positions (Wolfenstein Instrumentation HUD)
    struct DefW { const char* label; float x, y; int anchor, size; D3DCOLOR c, bg, lc; };
    DefW defs[W_COUNT] = {
        {"RTA",       0.015f, 0.020f, 0, 24, 0xFFFFFFFF, 0xD0101216, 0xFF8E929A},
        {"MAP RTA",   0.015f, 0.055f, 0, 18, 0xFFFFFFFF, 0xD0101216, 0xFF8E929A},
        {"IGT",       0.015f, 0.085f, 0, 32, 0xFFFFB834, 0xD0101216, 0xFF8E929A},
        {"MAP IGT",   0.015f, 0.130f, 0, 18, 0xFFFFB834, 0xD0101216, 0xFF8E929A},
        {"DOSSIER",   0.015f, 0.170f, 0, 16, 0xFFFFFFFF, 0xD0101216, 0xFF8E929A},
        {"MISSION",   0.985f, 0.020f, 1, 20, 0xFFEAECEE, 0xD0101216, 0xFFFFB834},
        {"OPERATION", 0.985f, 0.050f, 1, 16, 0xFFFFB834, 0xD0101216, 0xFF8E929A},
        {"COORD",     0.985f, 0.080f, 1, 15, 0xFF20C997, 0xD0101216, 0xFF8E929A},
        {"SPEED",     0.985f, 0.105f, 1, 18, 0xFF20C997, 0xD0101216, 0xFF8E929A},
        {"PEAK",      0.985f, 0.130f, 1, 15, 0xFF20C997, 0xD0101216, 0xFF8E929A},
        {"LOADS",     0.985f, 0.155f, 1, 15, 0xFFFA5252, 0xD0101216, 0xFF8E929A},
        {"CASUALTY",  0.985f, 0.180f, 1, 15, 0xFFFA5252, 0xD0101216, 0xFF8E929A}
    };

    for (int i = 0; i < W_COUNT; i++) {
        char buf[256];
        GetPrivateProfileStringA("Widgets", kWidgetNames[i], "", buf, sizeof buf, g_ini);
        Widget& w = s.w[i];
        if (buf[0]) {
            int on, anc, sz; unsigned col, bg, lc; char lbl[64] = { 0 };
            if (sscanf(buf, "%d,%f,%f,%d,%d,%x,%x,%x,%63[^\r\n]",
                       &on, &w.x, &w.y, &anc, &sz, &col, &bg, &lc, lbl) >= 8) {
                w.on = on != 0; w.anchor = anc; w.size = sz;
                w.color = col; w.bg = bg; w.labelColor = lc; w.label = lbl;
            }
        } else {
            w.on = true; w.x = defs[i].x; w.y = defs[i].y; w.anchor = defs[i].anchor;
            w.size = defs[i].size; w.color = defs[i].c; w.bg = defs[i].bg;
            w.labelColor = defs[i].lc; w.label = defs[i].label;
        }
    }

    for (int i = 0; i < K_COUNT; i++) {
        s.keys[i] = GetPrivateProfileIntA("Keys", kKeyNames[i], kDefaultKeys[i], g_ini);
    }
    return s;
}

// ---- Direct3D 9 Overlay & High-DPI Typography -----------------------------------------------
static ID3DXFont* Font(IDirect3DDevice9* dev, int size, const std::string& face) {
    if (g_fontsDirty) {
        for (auto& f : g_fonts) f.second->Release();
        g_fonts.clear();
        g_fontsDirty = false;
    }
    auto it = g_fonts.find(size);
    if (it != g_fonts.end()) return it->second;
    if (!g_createFont) return nullptr;

    ID3DXFont* f = nullptr;
    // Attempt high-DPI ClearType with JetBrains Mono, falling back cleanly
    const char* fontFace = face.empty() ? "JetBrains Mono" : face.c_str();
    HRESULT hr = g_createFont(dev, size, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              fontFace, &f);
    if (FAILED(hr)) {
        hr = g_createFont(dev, size, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          "Consolas", &f);
    }
    if (SUCCEEDED(hr)) g_fonts[size] = f;
    return f;
}

static SIZE Measure(ID3DXFont* f, const std::string& str) {
    RECT r = { 0, 0, 0, 0 };
    f->DrawTextA(nullptr, str.c_str(), (int)str.size(), &r, DT_CALCRECT, 0);
    return SIZE{ r.right - r.left, r.bottom - r.top };
}

static void Text(ID3DXFont* f, const std::string& str, RECT r, DWORD fmt, D3DCOLOR col, bool shadow) {
    if (shadow) {
        RECT sh = { r.left + 1, r.top + 1, r.right + 1, r.bottom + 1 };
        f->DrawTextA(nullptr, str.c_str(), (int)str.size(), &sh, fmt, 0xEE000000);
    }
    f->DrawTextA(nullptr, str.c_str(), (int)str.size(), &r, fmt, col);
}

// High-tech telemetry gauge panel with smoked glass & vertical accent bar
static void DrawTelemetryPanel(IDirect3DDevice9* dev, int x0, int y0, int x1, int y1, D3DCOLOR bg, D3DCOLOR accent) {
    struct V { float x, y, z, rhw; D3DCOLOR col; };
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    // Dark smoked glass background
    V quad[4] = {
        { (float)x0, (float)y0, 0.0f, 1.0f, bg },
        { (float)x1, (float)y0, 0.0f, 1.0f, bg },
        { (float)x0, (float)y1, 0.0f, 1.0f, bg },
        { (float)x1, (float)y1, 0.0f, 1.0f, bg }
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

    // 3px Left vertical accent bar
    V bar[4] = {
        { (float)x0, (float)y0, 0.0f, 1.0f, accent },
        { (float)x0 + 3.0f, (float)y0, 0.0f, 1.0f, accent },
        { (float)x0, (float)y1, 0.0f, 1.0f, accent },
        { (float)x0 + 3.0f, (float)y1, 0.0f, 1.0f, accent }
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(V));

    // Subtle 1px border
    D3DCOLOR border = 0x5530343D;
    V outline[5] = {
        { (float)x0, (float)y0, 0.0f, 1.0f, border },
        { (float)x1, (float)y0, 0.0f, 1.0f, border },
        { (float)x1, (float)y1, 0.0f, 1.0f, border },
        { (float)x0, (float)y1, 0.0f, 1.0f, border },
        { (float)x0, (float)y0, 0.0f, 1.0f, border }
    };
    dev->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, outline, sizeof(V));
}

static std::string WidgetText(int id, const Settings& st) {
    char b[128];
    switch (id) {
    case W_TIMER:     return FormatTime(g_run.rta);
    case W_MAP_TIMER: return FormatTime(g_run.mapRta);
    case W_TOTAL_IGT: return FormatTime(g_run.igt);
    case W_MAP_IGT:   return FormatTime(g_run.mapIgt);
    case W_MAP:       return g_run.map.empty() ? "--" : g_run.map;
    case W_CATEGORY:  return GetFullCategoryDisplay(st);
    case W_POS:
        snprintf(b, sizeof b, "X:%7.1f Y:%7.1f Z:%7.1f", g_pos[0], g_pos[1], g_pos[2]); return b;
    case W_SPEED:
        snprintf(b, sizeof b, "%.0f u/s", g_speed); return b;
    case W_MAX_SPEED:
        snprintf(b, sizeof b, "PEAK: %.0f u/s", g_maxSpeed); return b;
    case W_LOADS:
        snprintf(b, sizeof b, "%d (%s)", g_run.loads, FormatTime(g_run.loadTime, false).c_str()); return b;
    case W_DEATHS:
        snprintf(b, sizeof b, "%d", g_run.deaths); return b;
    default: return "";
    }
}

static void DrawLine(IDirect3DDevice9* dev, const std::string& fontName, int size, bool shadow,
                     const std::string& label, D3DCOLOR labelColor,
                     const std::string& value, D3DCOLOR valColor, D3DCOLOR bg,
                     float rx, float ry, int anchor, int W, int H) {
    ID3DXFont* f = Font(dev, size, fontName);
    if (!f) return;
    std::string text = label.empty() ? value : (label + ":  " + value);
    SIZE sz = Measure(f, text);

    int x = int(rx * W), y = int(ry * H);
    int padX = 12, padY = 5;

    switch (anchor) {
    case 1: x -= sz.cx + padX * 2; break; // Top-Right
    case 2: y -= sz.cy + padY * 2; break; // Bottom-Left
    case 3: x -= sz.cx + padX * 2; y -= sz.cy + padY * 2; break; // Bottom-Right
    case 4: x -= (sz.cx + padX * 2) / 2; break; // Top-Center
    case 5: x -= (sz.cx + padX * 2) / 2; y -= (sz.cy + padY * 2) / 2; break; // Center
    }

    DrawTelemetryPanel(dev, x, y, x + sz.cx + padX * 2, y + sz.cy + padY * 2, bg ? bg : 0xD0101216, 0xFFFFB834);

    RECT r = { x + padX, y + padY, x + sz.cx + padX, y + sz.cy + padY };
    if (!label.empty()) {
        std::string lbl = label + ":  ";
        SIZE lsz = Measure(f, lbl);
        RECT lr = { r.left, r.top, r.left + lsz.cx, r.bottom };
        Text(f, lbl, lr, DT_LEFT, labelColor ? labelColor : 0xFF8E929A, shadow);
        r.left += lsz.cx;
    }
    Text(f, value, r, DT_LEFT, valColor ? valColor : 0xFFFFFFFF, shadow);
}

static void DrawSplits(IDirect3DDevice9* dev, const Settings& st, const Widget& w, int W, int H) {
    ID3DXFont* f = Font(dev, w.size, st.font);
    if (!f) return;

    std::vector<std::pair<std::string, double>> splits = g_pbSplits;
    if (splits.empty()) {
        splits = { {"trainyard", 125.0}, {"digsite", 240.0}, {"church", 390.0}, {"hospital", 650.0} };
    }

    int maxCount = st.splitsCount;
    int count = (int)splits.size() > maxCount ? maxCount : (int)splits.size();
    if (count <= 0) return;

    int lineH = w.size + 6;
    int padX = 14, padY = 8;
    int boxW = 280;
    int boxH = padY * 2 + (count + 1) * lineH;

    int x = int(w.x * W), y = int(w.y * H);
    if (w.anchor == 1 || w.anchor == 3) x -= boxW;
    if (w.anchor == 2 || w.anchor == 3) y -= boxH;

    DrawTelemetryPanel(dev, x, y, x + boxW, y + boxH, w.bg ? w.bg : 0xD5101216, 0xFFFFB834);

    // Header
    RECT hr = { x + padX, y + padY, x + boxW - padX, y + padY + lineH };
    Text(f, "/// MISSION SPLITS DOSSIER", hr, DT_LEFT, 0xFFFFB834, st.shadow);
    Text(f, "DELTA", hr, DT_RIGHT, 0xFF8E929A, st.shadow);

    // Rows
    for (int i = 0; i < count; i++) {
        int ry = y + padY + (i + 1) * lineH;
        RECT rName = { x + padX, ry, x + boxW - 90, ry + lineH };
        RECT rDelta = { x + boxW - 85, ry, x + boxW - padX, ry + lineH };

        std::string name = splits[i].first;
        double pbTime = splits[i].second;
        double delta = (g_run.mapIgt > 0) ? (g_run.mapIgt - pbTime) : 0;

        Text(f, name, rName, DT_LEFT, 0xFFEAECEE, st.shadow);

        std::string deltaStr = FormatDelta(delta);
        D3DCOLOR deltaCol = 0xFF8E929A;
        if (delta < 0) deltaCol = 0xFF40C057; // Ahead (Green)
        else if (delta > 0) deltaCol = 0xFFFA5252; // Behind (Red)

        Text(f, deltaStr, rDelta, DT_RIGHT, deltaCol, st.shadow);
    }
}

// ---- ImGui Advanced Configurator Style & Theme ------------------------------------------------
static void SetupAdvancedConfiguratorTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowPadding     = ImVec2(16.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 5.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    // Wolfenstein Advanced Configurator Gunmetal / Amber Gold Theme
    colors[ImGuiCol_Text]                  = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.12f, 0.14f, 0.85f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.09f, 0.11f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.26f, 0.30f, 0.75f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.05f, 0.05f, 0.06f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.08f, 0.09f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.34f, 0.36f, 0.42f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.85f, 0.60f, 0.15f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.95f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.68f, 0.22f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.85f, 0.60f, 0.15f, 0.85f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.95f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.30f, 0.36f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.85f, 0.60f, 0.15f, 0.60f);
    colors[ImGuiCol_Separator]             = ImVec4(0.24f, 0.26f, 0.30f, 0.80f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.85f, 0.60f, 0.15f, 0.80f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.95f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.24f, 0.26f, 0.30f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.85f, 0.60f, 0.15f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.95f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.85f, 0.60f, 0.15f, 0.70f);
    colors[ImGuiCol_TabSelected]           = ImVec4(0.85f, 0.60f, 0.15f, 0.90f);
    colors[ImGuiCol_TabDimmed]             = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]     = ImVec4(0.25f, 0.22f, 0.18f, 1.00f);
}

// Toggle switch pill inspired by Advanced Configurator
static bool ToggleSwitch(const char* label, bool* v, const char* hint) {
    bool clicked = false;
    ImGui::PushID(label);

    ImVec4 activeBg = ImVec4(0.18f, 0.55f, 0.25f, 1.0f); // Vibrant emerald
    ImVec4 inactiveBg = ImVec4(0.18f, 0.20f, 0.23f, 1.0f);

    if (*v) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.20f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, inactiveBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.28f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.15f, 0.18f, 1.0f));
    }

    std::string btnText = *v ? "  ON  " : "  OFF  ";
    if (ImGui::Button(btnText.c_str(), ImVec2(60, 24))) {
        *v = !(*v);
        clicked = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::Text("%s", label);
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);

    ImGui::PopID();
    return clicked;
}

static void DrawWindowHeader(const char* title, const char* subtitle, bool* pOpen) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.20f, 1.0f));
    ImGui::TextUnformatted("WOLFENSTEIN (2009)");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.60f, 0.63f, 0.68f, 1.0f), "// %s", subtitle);

    float closeBtnX = ImGui::GetWindowWidth() - 75.0f;
    ImGui::SameLine(closeBtnX);
    if (ImGui::Button("CLOSE", ImVec2(60, 22))) {
        if (pOpen) *pOpen = false;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

// BXT-Style New Run Setup Wizard
static void DrawNewRunWizard(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 440.0f, io.DisplaySize.y * 0.5f - 310.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("OPERATION INITIALIZATION // RUN SETUP WIZARD", &g_wizardOpen, ImGuiWindowFlags_NoCollapse)) {
        DrawWindowHeader("RUN SETUP WIZARD", "MISSION DISPATCH & REGULATION ENFORCEMENT", &g_wizardOpen);

        // Columns: Left = Setup Controls, Right = Live Rules Dossier
        ImGui::Columns(2, "WizardLayout", true);
        ImGui::SetColumnWidth(0, 520.0f);

        // STEP 1: CATEGORY SELECTION
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "/// STEP 1: SELECT CATEGORY");
        ImGui::Spacing();

        for (int i = 0; i < CAT_COUNT; i++) {
            bool selected = (g_set.category == kCategoryKeys[i]);
            if (ImGui::RadioButton(kCategoryNames[i], selected)) {
                g_set.category = kCategoryKeys[i];
                SaveSettings(g_set);
                std::string catKey = GetPbCategoryKey(g_set);
                g_pb = ReadPb(catKey, g_run.map);
                g_pbSplits = ReadPbSplits(catKey);
            }
        }

        // IL Specific options
        if (g_set.category == "il") {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.60f, 1.0f), "Individual Level (15 speedrun.com Levels):");
            if (ImGui::Combo("Mission", &g_set.ilLevelIdx, kIlLevelNames, kIlLevelsCount)) {
                SaveSettings(g_set);
            }
            if (ToggleSwitch("New Game+ Loadout (Spawn with All Weapons & Powers)", &g_set.ilNewGamePlus, "Equips BJ with all weapons, veil upgrades, and ammo upon level start")) {
                SaveSettings(g_set);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 2: RULESET ENFORCEMENT
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "/// STEP 2: RULESET ('Edits in config')");
        ImGui::Spacing();

        bool isRestricted = (g_set.ruleset == RULESET_RESTRICTED);
        if (ImGui::RadioButton("Restricted (Official Competitive Standard)", isRestricted)) {
            g_set.ruleset = RULESET_RESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map); g_pbSplits = ReadPbSplits(catKey);
        }
        if (ImGui::RadioButton("Unrestricted (All Skips & Quicksaves Allowed)", !isRestricted)) {
            g_set.ruleset = RULESET_UNRESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map); g_pbSplits = ReadPbSplits(catKey);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 3: DIFFICULTY
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "/// STEP 3: DIFFICULTY (g_skill)");
        ImGui::Spacing();

        for (int i = 0; i < DIFF_COUNT; i++) {
            if (ImGui::RadioButton(kDifficultyNames[i], g_set.difficulty == i)) {
                g_set.difficulty = i; SaveSettings(g_set);
            }
            if (i < DIFF_COUNT - 1) ImGui::SameLine();
        }

        // NEXT COLUMN: Live Dossier & Summary
        ImGui::NextColumn();

        ImGui::BeginChild("WizardDossier", ImVec2(0, 420), true);
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "[ REGULATION CRITERIA ]");
        ImGui::Separator();
        ImGui::Spacing();

        if (g_set.category == "any") {
            ImGui::TextWrapped("Any%%: Reach the final cutscene after Hans Grosse and Black Sun dimension as fast as possible.");
        } else if (g_set.category == "all_missions") {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.60f, 1.0f), "All Missions: Complete all 18 story & district levels.");
            ImGui::TextWrapped("Currently has 0 runs submitted on speedrun.com! All 18 missions are tracked live.");
        } else if (g_set.category == "100") {
            ImGui::TextWrapped("100%%: Collect all 148 Gold, 88 Intel, 16 Tomes of Power, and purchase all weapon and veil upgrades.");
        } else if (g_set.category == "il") {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.60f, 1.0f), "IL: %s", kIlLevelNames[g_set.ilLevelIdx]);
            ImGui::TextWrapped("Timing starts on player control and stops on mission complete screen. New Game+ equips full loadout.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (g_set.ruleset == RULESET_RESTRICTED) {
            ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.40f, 1.0f), "RESTRICTED MODE ACTIVE:");
            ImGui::BulletText("Quicksaves (F5) and Quickloads (F9) are BLOCKED.");
            ImGui::BulletText("No console commands or wolf.cfg cheats.");
            ImGui::BulletText("ALLOWED: Mission replay warps (Train Station trick).");
            ImGui::BulletText("ALLOWED: FOV adjustment, View Bobbing disable.");
        } else {
            ImGui::TextColored(ImVec4(0.40f, 1.0f, 0.40f, 1.0f), "UNRESTRICTED MODE ACTIVE:");
            ImGui::BulletText("Quicksaves and Quickloads permitted.");
            ImGui::BulletText("All geometry skips and clips allowed.");
            ImGui::BulletText("Warps and mission abort skips permitted.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Active Target:");
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "%s", GetFullCategoryDisplay(g_set).c_str());

        ImGui::EndChild();

        ImGui::Columns(1);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Launch Action Button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.68f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.45f, 0.20f, 1.0f));
        if (ImGui::Button("START OPERATION NOW (Launch Run)", ImVec2(340, 36))) {
            StartNewRunFromWizard();
            g_wizardOpen = false;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Dismiss (Esc)", ImVec2(120, 36))) {
            g_wizardOpen = false;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("Quick Dispatch Hotkey: [F2]");
    }
    ImGui::End();
}

// In-Game Tactical Operations & Cheats Menu (Advanced Configurator Style)
static void DrawInGameMenu(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 440.0f, io.DisplaySize.y * 0.5f - 310.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("WOLFENSTEIN // TACTICAL OPERATIONS & CONFIGURATOR", &g_menuOpen, ImGuiWindowFlags_NoCollapse)) {
        DrawWindowHeader("TACTICAL OPERATIONS", "ADVANCED CONFIGURATOR & SPEEDRUN SUITE", &g_menuOpen);

        if (ImGui::BeginTabBar("TacticalTabs")) {

            // TAB 1: RUN DISPATCH
            if (ImGui::BeginTabItem("  [ ⚔ OPERATION SETUP ]  ")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Speedrun Setup & Categories");
                ImGui::Spacing();

                ImGui::Text("Current Profile: %s", GetFullCategoryDisplay(g_set).c_str());
                ImGui::Spacing();

                if (ImGui::Button("OPEN INTERACTIVE RUN WIZARD (F2)", ImVec2(320, 32))) {
                    g_wizardOpen = true; g_menuOpen = false;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Configure category, ruleset, difficulty and launch in 1 click.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Timing & Trigger Controls");
                ToggleSwitch("Auto-Start Timer on Campaign Intro Cutscene", &g_set.autoStartCutscene, "Resets and starts run automatically when wolf_campaign_start cutscene begins");
                ToggleSwitch("Auto-Start Timer on Individual Levels (IL)", &g_set.autoStartIl, "Starts timer immediately when spawning into IL level");
                ToggleSwitch("Warp Protection Lock", &g_warpProtection, "Prevents Train Station reloads from wiping your timer during active runs");

                ImGui::EndTabItem();
            }

            // TAB 2: CHEATS & ARSENAL
            if (ImGui::BeginTabItem("  [ ⚡ CHEATS & ARSENAL ]  ")) {
                ImGui::Spacing();

                // Unlocks & DLC
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Engine Unlocks & DLC");
                if (ImGui::Button("Unlock FlameDLC (Flamethrower in First Room)")) {
                    ExecCommand("seta g_unlockedFlameDLC 1\n"); Message("FlameDLC Unlocked!");
                }
                ImGui::SameLine();
                if (ImGui::Button("Unlock Built-in Cheat Menu")) {
                    ExecCommand("seta g_showCheatMenuMsg 1\nseta g_gameFinished 1\n"); Message("Built-in Cheats Unlocked!");
                }
                ImGui::SameLine();
                if (ImGui::Button("Unlock Developer Console")) {
                    ExecCommand("set com_allowConsole 1\nseta g_cheatsAreOn 1\n"); Message("Console Unlocked! [R-Alt]+[L-Alt]+~");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Player & Combat Cheats
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Combat & Player Cheats");
                if (ImGui::Button("God Mode (Invulnerable)")) { ExecCommand("god\n"); Message("God Mode Toggled"); }
                ImGui::SameLine();
                if (ImGui::Button("No Target (Enemies Ignore)")) { ExecCommand("notarget\n"); Message("NoTarget Toggled"); }
                ImGui::SameLine();
                if (ImGui::Button("Infinite Stamina")) { ExecCommand("seta g_infiniteStamina 1\n"); Message("Infinite Stamina Activated"); }

                if (ImGui::Button("Give All Weapons & Ammo")) { ExecCommand("give all\ngive ammocomponent\n"); Message("Arsenal Restocked"); }
                ImGui::SameLine();
                if (ImGui::Button("Give All Veil Powers & Upgrades")) { ExecCommand("givePowers\ngiveALLPowerUpgrades\n"); Message("Veil Powers Fully Upgraded"); }
                ImGui::SameLine();
                if (ImGui::Button("Max Gold ($100,000)")) { ExecCommand("giveGold 100000\n"); Message("Max Gold Issued"); }
                ImGui::SameLine();
                if (ImGui::Button("Unlock All Intel & Objectives")) { ExecCommand("giveAllIntel\ngiveObjectives\n"); Message("Intel Unlocked"); }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Ammo Dispensary
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Ammo Dispensary");
                if (ImGui::Button("MP40")) ExecCommand("give ammo_mp40\n"); ImGui::SameLine();
                if (ImGui::Button("MP43")) ExecCommand("give ammo_mp43\n"); ImGui::SameLine();
                if (ImGui::Button("Kar98")) ExecCommand("give ammo_mauser\n"); ImGui::SameLine();
                if (ImGui::Button("Panzer")) ExecCommand("give ammo_panzer\n"); ImGui::SameLine();
                if (ImGui::Button("Particle Cannon")) ExecCommand("give ammo_particle_cannon\n"); ImGui::SameLine();
                if (ImGui::Button("Tesla")) ExecCommand("give ammo_tesla\n"); ImGui::SameLine();
                if (ImGui::Button("MDL24")) ExecCommand("give ammo_mdl24\n");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Spawner & Warps
                ImGui::Columns(2, "CheatsSplit", false);

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Entity Spawner (19 Entities)");
                ImGui::Combo("##SpawnEnt", &g_selectedSpawnEntity, kSpawnEntityNames, kSpawnEntitiesCount);
                if (ImGui::Button("SPAWN SELECTED ENTITY", ImVec2(240, 28))) {
                    ExecCommand(std::string("spawn ") + kSpawnEntityIds[g_selectedSpawnEntity] + "\n");
                    Message("Spawned: " + std::string(kSpawnEntityNames[g_selectedSpawnEntity]));
                }

                ImGui::NextColumn();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Tactical Map Warp");
                ImGui::Combo("##WarpMap", &g_selectedWarpMap, kIlLevelNames, kIlLevelsCount);
                if (ImGui::Button("WARP TO SELECTED MAP", ImVec2(240, 28))) {
                    ExecCommand(std::string("map ") + kIlLevelPaths[g_selectedWarpMap] + "\n");
                    Message("Warping to: " + std::string(kIlLevelNames[g_selectedWarpMap]));
                }

                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            // TAB 3: CAMERA & VISUALS
            if (ImGui::BeginTabItem("  [ 👁 CAMERA & VISUALS ]  ")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Field of View (FOV)");
                if (ImGui::SliderInt("##FOV", &g_set.fov, 60, 120, "%d deg")) {
                    SaveSettings(g_set); ApplyFovAndBob();
                }
                ImGui::TextDisabled("Verified legal in speedrun.com Restricted & Unrestricted categories.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Head Bobbing & Camera Shake (Sprint Fix)");
                if (ToggleSwitch("Enable Head Bobbing", &g_set.headBob, "Toggle between realistic head bobbing and motionless camera")) {
                    SaveSettings(g_set); ApplyFovAndBob();
                }
                ImGui::TextColored(ImVec4(0.40f, 1.0f, 0.40f, 1.0f),
                    "[Fixed]: Eliminates motion sickness shake without freezing your sprint arms animation!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Game Language (sys_lang)");
                int curLang = 0;
                for (int i = 0; i < kLangCount; i++) {
                    if (g_set.language == kLanguages[i]) { curLang = i; break; }
                }
                if (ImGui::Combo("##Language", &curLang, kLanguages, kLangCount)) {
                    g_set.language = kLanguages[curLang];
                    SaveSettings(g_set);
                    ExecCommand(std::string("set sys_lang \"") + g_set.language + "\"\n");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Checkpoint Controls");
                if (ImGui::Button("Restart Current Checkpoint", ImVec2(240, 28))) {
                    ExecCommand("ScaleformDiedRestart\n"); Message("Restart Checkpoint");
                }
                ImGui::EndTabItem();
            }

            // TAB 4: HUD & TELEMETRY
            if (ImGui::BeginTabItem("  [ 📊 HUD & TELEMETRY ]  ")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Master Speedrun Overlay");
                ToggleSwitch("Master Overlay Visible", &g_visible, "Show or hide in-game speedrun gauges");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Telemetry Gauges Editor");
                ImGui::Combo("Gauge Select", &g_selectedWidget, kWidgetDisplayNames, W_COUNT);

                Widget& w = g_set.w[g_selectedWidget];
                if (ToggleSwitch("Gauge Enabled", &w.on)) SaveSettings(g_set);
                if (ImGui::SliderFloat("Position X", &w.x, 0.0f, 1.0f, "%.3f")) SaveSettings(g_set);
                if (ImGui::SliderFloat("Position Y", &w.y, 0.0f, 1.0f, "%.3f")) SaveSettings(g_set);
                if (ImGui::SliderInt("Size / Height", &w.size, 12, 48, "%d px")) SaveSettings(g_set);

                char lblBuf[64]; snprintf(lblBuf, sizeof lblBuf, "%s", w.label.c_str());
                if (ImGui::InputText("Custom Label", lblBuf, sizeof lblBuf)) {
                    w.label = lblBuf; SaveSettings(g_set);
                }
                ImGui::EndTabItem();
            }

            // TAB 5: RECORDS & SPLITS
            if (ImGui::BeginTabItem("  [ ⏱ RECORDS & SPLITS ]  ")) {
                ImGui::Spacing();
                std::string catKey = GetPbCategoryKey(g_set);
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Records Profile: %s", catKey.c_str());
                ImGui::Text("Current Map PB: %s", (g_pb > 0) ? FormatTime(g_pb).c_str() : "No PB set yet");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Split Records for Active Profile");
                ImGui::BeginChild("SplitsTable", ImVec2(0, 240), true);
                for (size_t i = 0; i < g_pbSplits.size(); i++) {
                    ImGui::Text("%2d. %-24s  %s", (int)i + 1, g_pbSplits[i].first.c_str(), FormatTime(g_pbSplits[i].second).c_str());
                }
                ImGui::EndChild();

                ImGui::Spacing();
                if (ImGui::Button("Reset Personal Bests for Active Profile", ImVec2(300, 28))) {
                    WritePrivateProfileSectionA(("Splits_" + catKey).c_str(), "", g_pbIni);
                    WritePrivateProfileSectionA(("PB_" + catKey).c_str(), "", g_pbIni);
                    g_pb = 0; g_pbSplits.clear();
                    Message("PBs reset for: " + catKey);
                }
                ImGui::EndTabItem();
            }

            // TAB 6: HOTKEYS
            if (ImGui::BeginTabItem("  [ ⌨ HOTKEYS ]  ")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "Click any button to rebind (Esc cancels):");
                ImGui::Spacing();

                for (int i = 0; i < K_COUNT; i++) {
                    char btnLabel[64];
                    if (g_rebindKeyIndex == i) snprintf(btnLabel, sizeof btnLabel, "Press key for %s...", kKeyNames[i]);
                    else snprintf(btnLabel, sizeof btnLabel, "%-18s: [ %s ]", kKeyNames[i], GetKeyDisplayName(g_set.keys[i]));

                    if (ImGui::Button(btnLabel, ImVec2(340, 26))) {
                        g_rebindKeyIndex = i;
                    }
                }
                ImGui::EndTabItem();
            }

        }
        ImGui::EndTabBar();

        // Footer Bar
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Hotkeys: [F1] Toggle Menu  |  [F2] Start Run Wizard  |  [F5] Save  |  [F9] Load  |  [Esc] Dismiss");
    }
    ImGui::End();
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
    default: snprintf(buf, sizeof buf, "Key 0x%02X", vk); return buf;
    }
}

// Master Overlay Render
static void DrawOverlay(IDirect3DDevice9* dev) {
    D3DVIEWPORT9 vp; dev->GetViewport(&vp);
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    IDirect3DSurface9* bb = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (bb) dev->SetRenderTarget(0, bb);

    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);

    // Initialize ImGui on first render
    if (!g_imguiInit) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        dev->GetCreationParameters(&cp);
        g_gameHwnd = cp.hFocusWindow;
        if (g_gameHwnd) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            SetupAdvancedConfiguratorTheme();

            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // Don't create rogue imgui.ini

            // Load high-resolution JetBrains Mono font
            ImFontConfig cfg;
            cfg.OversampleH = 3; cfg.OversampleV = 2;
            ImFont* customFont = nullptr;
            if (GetFileAttributesA("srmod_font.ttf") != INVALID_FILE_ATTRIBUTES) {
                customFont = io.Fonts->AddFontFromFileTTF("srmod_font.ttf", 16.0f, &cfg);
            } else if (GetFileAttributesA("..\\SP\\srmod_font.ttf") != INVALID_FILE_ATTRIBUTES) {
                customFont = io.Fonts->AddFontFromFileTTF("..\\SP\\srmod_font.ttf", 16.0f, &cfg);
            }
            if (!customFont) io.Fonts->AddFontDefault();

            ImGui_ImplWin32_Init(g_gameHwnd);
            ImGui_ImplDX9_Init(dev);
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
            g_imguiInit = true;
            Log("ImGui initialized on HWND %p with Advanced Configurator Theme & JetBrains Mono", g_gameHwnd);
        }
    }

    // Mouse cursor unclip & show/hide
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

        // Draw In-Game HUD Telemetry Gauges
        if (g_visible) {
            if (!g_supported)
                DrawLine(dev, st.font, 22, true, "", 0, "srmod: verifying Gamex86 memory...", 0xFFFF5050,
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
                    DrawTelemetryPanel(dev, bx - 16, by - 6, bx + m.cx + 16, by + m.cy + 6, 0xF0101216, 0xFFFA5252);
                    Text(f, badge, RECT{bx, by, bx + m.cx, by + m.cy}, DT_CENTER, 0xFFEAECEE, true);
                }
            }

            // Bottom status prompt
            if (!g_menuOpen && !g_wizardOpen) {
                bool isMainMenu = (g_run.map == "menu" || g_run.map.empty());
                std::string prompt = isMainMenu
                    ? "[F2] START SPEEDRUN WIZARD  |  [F1] ADVANCED CONFIGURATOR & CHEATS"
                    : "[F1] Tactical Suite  |  [F2] Run Setup";
                D3DCOLOR promptColor = isMainMenu ? 0xFFFFB834 : 0x80FFFFFF;
                DrawLine(dev, st.font, 14, false, "", 0, prompt, promptColor, 0x60000000, 0.005f, 0.995f, 2, W, H);
            }
        }

        // Render Dear ImGui menus
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
    DWORD old;
    VirtualProtect(&vtable[idx], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vtable[idx];
    vtable[idx] = hook;
    VirtualProtect(&vtable[idx], sizeof(void*), old, &old);
    Log("hook %d: vtable (%p -> %p)", idx, *orig, hook);
    return true;
}

static void InitD3D(IDirect3DDevice9* dev) {
    // Register high-DPI JetBrains Mono into GDI private font table
    AddFontResourceExA("srmod_font.ttf", FR_PRIVATE, 0);
    AddFontResourceExA("..\\SP\\srmod_font.ttf", FR_PRIVATE, 0);

    HMODULE d3dx = LoadLibraryA("d3dx9_43.dll");
    if (!d3dx) {
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\d3dx9_43.dll", sys);
        d3dx = LoadLibraryA(p);
    }
    if (d3dx) g_createFont = (D3DXCreateFontA_t)GetProcAddress(d3dx, "D3DXCreateFontA");
    void** vt = *(void***)dev;
    Hook(vt, 17, (void*)hkPresent, (void**)&oPresent);
    Hook(vt, 16, (void*)hkReset,   (void**)&oReset);
}

static void HookD3D9() {
    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "SrTmp", nullptr };
    RegisterClassExA(&wc);
    HWND h = CreateWindowA("SrTmp", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { DestroyWindow(h); UnregisterClassA("SrTmp", wc.hInstance); return; }
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, h, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        InitD3D(dev);
        dev->Release();
    }
    d3d->Release();
    DestroyWindow(h);
    UnregisterClassA("SrTmp", wc.hInstance);
}

// ---- Bink Proxy Export Forwarding ------------------------------------------------------------
static HMODULE g_binkOrig;

static void* BinkOrig(const char* name) {
    if (!g_binkOrig) {
        g_binkOrig = LoadLibraryA("binkw32_orig.dll");
        if (!g_binkOrig) {
            char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\binkw32.dll", sys);
            g_binkOrig = LoadLibraryA(p);
        }
    }
    return g_binkOrig ? (void*)GetProcAddress(g_binkOrig, name) : nullptr;
}

extern "C" void* __stdcall SrBinkOpen(const char* file, unsigned flags) {
    static auto fn = (void*(__stdcall*)(const char*, unsigned))BinkOrig("_BinkOpen@8");
    void* h = fn ? fn(file, flags) : nullptr;
    if (h) InterlockedIncrement(&g_binkOpen);

    if (file) {
        std::string fnLower = file;
        for (char& c : fnLower) c = (char)tolower((unsigned char)c);
        if (fnLower.find("wolf_campaign_start") != std::string::npos) {
            std::lock_guard<std::mutex> lk(g_mx);
            bool isMidRun = (g_run.state == Run::Running && g_havePlayer && g_run.map != "menu");
            if (isMidRun) {
                Log("WARP PROTECTION: Suppressed cutscene reset during active run (map=%s)", g_run.map.c_str());
            } else if (g_set.autoStartCutscene) {
                if (!g_wizardOpen && g_run.state != Run::Running) {
                    g_wizardOpen = true;
                    g_menuOpen = false;
                    Log("Campaign start cutscene intercepted -> Opened in-game Run Setup Wizard");
                }
                g_run.Start();
                memset(g_allMissionsDone, 0, sizeof(g_allMissionsDone));
                g_run.mapClean = true;
                g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
                Message("Operation Launched (" + GetFullCategoryDisplay(g_set) + ")");
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
        char* sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        snprintf(g_ini, sizeof g_ini, "%s\\srmod.ini", g_dir);
        snprintf(g_log, sizeof g_log, "%s\\srmod.log", g_dir);
        snprintf(g_pbIni, sizeof g_pbIni, "%s\\srmod_pb.ini", g_dir);
        DeleteFileA(g_log);

        Log("srmod attached, base=%s", g_dir);
        g_session = FindSession();
        Log("session=%p", g_session);

        g_set = LoadSettings();
        SaveSettings(g_set);

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (int i = 0; i < 200; i++) {
                if (!g_session) g_session = FindSession();
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

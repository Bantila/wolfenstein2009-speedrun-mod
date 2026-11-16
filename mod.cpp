// srmod: Wolfenstein (2009) speedrun overlay and in-game configurator.
// Loaded as a binkw32.dll proxy (exports forwarded to binkw32_orig.dll via binkw32.def).
// A poll thread reads game memory and drives the timers;
// a hooked IDirect3DDevice9::Present draws the overlay and the in-game settings menu.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <timeapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "timer.h"

#pragma comment(lib, "winmm.lib")

// Forward declaration for ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- Addresses (Wolfenstein 1.21 SP, patched Gamex86.dll 6739216 bytes) -------------------
// Gamex86.dll RVAs
const DWORD RVA_GAMELOCAL       = 0x61f0b0;  // idGameLocal gameLocal
const DWORD RVA_CMDSYSTEM       = 0x61f050;  // idCmdSystem *cmdSystem
const DWORD RVA_PHYS_GETORIGIN  = 0x2f8760;  // idPhysics_Player::GetOrigin (shared by rvHkPhysics_Player)
const DWORD GL_LOCALCLIENT      = 0x63d4c;   // int localClientNum
const DWORD GL_ENTITIES         = 0x4d0;     // idEntity *entities[]
const DWORD GL_MAPNAME          = 0x63e28;   // idStr mapFileName data pointer
const DWORD GL_INCINEMATIC      = 0x63bdc;   // bool inCinematic (set by SetCamera)
const DWORD RVA_MISSIONCOMPLETE = 0x875e8c;  // mission complete menu object (from the community ASL)
const DWORD MC_ACTIVE           = 0x268;     // bool: mission complete screen shown
const DWORD PL_PHYSICS          = 0x2ec;     // idPhysics *physics
const DWORD PL_HEALTH           = 0xd8;      // float health
const DWORD PL_GODMODE          = 0x5fa;     // bool godmode
const DWORD PH_ORIGIN           = 0x9c;      // idVec3 current.origin
const DWORD PH_VELOCITY         = 0xa8;      // idVec3 current.velocity
const int   CMD_BUFFERTEXT_IDX  = 8;         // idCmdSystem::BufferCommandText vtable slot
const int   CMD_EXEC_APPEND     = 2;
// Wolf2.exe (fixed base 0x10000000)
const DWORD WOLF_SESSION_VT     = 0x105e78cc; // idSessionLocal vtable; the object lives in .data
const DWORD SESS_INSIDE_MAPCHANGE = 0xd;      // bool insideExecuteMapChange (set during all loads)

// Position-independent code bytes used as a version check.
struct Sig { DWORD rva; const char* bytes; int len; };
const Sig kSigs[] = {
    {0x06e1f0, "\x8B\x81\x28\x3E\x06\x00\xC3", 7},             // GetMapName
    {0x011460, "\x8B\x81\xEC\x02\x00\x00\xC3", 7},             // idPlayer::GetPhysics
    {0x2f8760, "\x8D\x81\x9C\x00\x00\x00\xC2\x04\x00", 9},     // idPhysics_Player::GetOrigin
};

// ---- Minimal D3DX font declarations (no DirectX SDK needed) ---------------------------------
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

// ---- Settings --------------------------------------------------------------------------------
enum WidgetId { W_COORDS, W_SPEED, W_RTA, W_IGT, W_MAP_RTA, W_MAP_IGT, W_PB, W_LOADS, W_DEATHS,
                W_CATEGORY, W_MAP, W_SPLITS, W_COUNT };
const char* kWidgetNames[W_COUNT] = {"coords", "speed", "total_rta", "total_igt", "map_rta", "map_igt",
                                     "pb", "loads", "deaths", "category", "map", "splits"};
const char* kWidgetLabels[W_COUNT] = {"", "Speed: ", "RTA: ", "IGT: ", "Map RTA: ", "Map IGT: ",
                                      "PB: ", "Loads: ", "Deaths: ", "", "", ""};
const char* kWidgetDisplayNames[W_COUNT] = {
    "Coordinates (X, Y, Z)", "Speedometer", "Total RTA", "Total IGT", "Map RTA", "Map IGT",
    "Current Map PB & Live Delta", "Loads Counter", "Deaths Counter", "Category Name", "Map Name", "Splits Table"
};

struct Widget {
    bool on; float x, y; int anchor, size; std::string label;
    D3DCOLOR color, labelColor, bg;                    // value text, label text, background (0 = none)
    D3DCOLOR nameColor, segColor, timeColor, aheadColor, behindColor, goldColor;  // splits only
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
    "Quick Save", "Quick Load", "Restart Checkpoint", "Open / Close In-Game Menu",
    "Start New Run (Wizard)"
};

// Speedrun Categories (speedrun.com style)
enum CategoryId {
    CAT_ANY,
    CAT_100,
    CAT_ALL_BOSSES,
    CAT_IL,
    CAT_CHEAT,
    CAT_COUNT
};
const char* kCategoryKeys[CAT_COUNT] = { "any", "100", "all_bosses", "il", "cheat" };
const char* kCategoryNames[CAT_COUNT] = { "Any%", "100%", "All Bosses", "Individual Level (IL)", "Cheat% (Practice)" };
const char* kCategorySummaries[CAT_COUNT] = {
    "Complete the game as fast as possible from campaign start to Black Sun end.",
    "Complete 100% of the game: all 148 Gold, 88 Intel, 16 Tomes of Power, and all weapon/veil upgrades.",
    "Defeat all 3 major story bosses: General Viktor Zetta, Queen Geist, and Hans Grosse.",
    "Individual level time attack for the selected mission.",
    "Practice category with developer cheats, god mode, and position teleportation."
};
const char* kCategoryTimingRules[CAT_COUNT] = {
    "Timing starts when wolf_campaign_start cutscene begins. Ends on final hit on Hans Grosse / dimension collapse.",
    "Timing starts on wolf_campaign_start. Ends when final Black Sun boss falls with 100% journal stats.",
    "Timing starts on wolf_campaign_start. Ends when Hans Grosse is defeated.",
    "Timing starts upon spawning into the map. Ends upon mission complete.",
    "Practice / unrestricted timing."
};

// Speedrun Rulesets
enum RulesetId {
    RULESET_RESTRICTED,    // 0: Competitive standard: Quicksaves/loads BLOCKED, no cheats, no warps
    RULESET_UNRESTRICTED,  // 1: Glitched: Quicksaves/loads allowed, warp glitches allowed
    RULESET_COUNT
};
const char* kRulesetKeys[RULESET_COUNT] = { "restricted", "unrestricted" };
const char* kRulesetNames[RULESET_COUNT] = { "Restricted", "Unrestricted" };
const char* kRulesetSummaries[RULESET_COUNT] = {
    "Competitive standard: Quicksave (F5) and Quickload (F9) are STRICTLY FORBIDDEN and blocked by mod proxy. Cheats & teleports blocked. Allowed: FOV adjustment, View Bobbing toggle, Language, Checkpoint Reload.",
    "Glitched runs: Quicksave and Quickload permitted. Mission replay warp glitch, geometry clipping, and out-of-bounds skips permitted."
};

// Difficulties (g_skill)
enum DifficultyId {
    DIFF_CASUAL,   // 0
    DIFF_NORMAL,   // 1
    DIFF_HARD,     // 2
    DIFF_UBER,     // 3
    DIFF_COUNT
};
const char* kDifficultyNames[DIFF_COUNT] = { "Casual (Easy)", "Normal (Medium)", "Hard", "Über (Competitive Standard)" };
const char* kDifficultyShortNames[DIFF_COUNT] = { "Casual", "Normal", "Hard", "Über" };

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
    int difficulty = DIFF_UBER;       // 0..3 (default 3 = Über)
    std::string cheatCmds, teleportCmd;
};

const int kMissionCount = 12;
const char* kMissionIds[kMissionCount] = {
    "trainyard", "downtown", "tavern", "digsite", "church", "farm",
    "hospital", "cannery", "castle", "airfield", "zeppelin", "blacksun"
};
const char* kMissionNames[kMissionCount] = {
    "Train Station (Вокзал)",
    "Downtown (Центр города)",
    "Tavern (Таверна)",
    "Dig Site (Раскопки)",
    "Church (Церковь)",
    "Farm (Ферма)",
    "Hospital (Больница)",
    "Cannery (Консервный завод)",
    "Castle (Замок)",
    "Airfield (Аэродром)",
    "Zeppelin (Дирижабль)",
    "Black Sun (Чёрное Солнце)",
};

const char* kLanguages[] = { "english", "russian", "french", "german", "italian", "spanish", "polish" };
const int kLangCount = 7;

static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_pbIni[MAX_PATH], g_log[MAX_PATH];
static std::mutex g_mx;          // guards everything below
static Settings g_set;
static Run g_run;
static bool g_fontsDirty = true, g_visible = true, g_supported = false;
static float g_pos[3], g_speed, g_maxSpeed;
static bool g_havePlayer;
static double g_pb = -1;          // PB of current map in current category, -1 = none
static std::vector<Split> g_pbSplits;  // splits of the best full run in current category
static volatile LONG g_binkOpen;  // open Bink videos (pre-rendered cutscenes)
static std::string g_msg; static DWORD g_msgUntil;

// In-Game Menu & Wizard state
static bool g_imguiInit = false;
static HWND g_gameHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool g_menuOpen = false;
static bool g_wizardOpen = false;
static int g_rebindKeyIndex = -1;
static int g_selectedWidget = 0;
static bool g_confirmResetPb = false;

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
        w.size = IniI(sec.c_str(), "size", 22);
        auto C = [&](const char* key, const char* def) { return (D3DCOLOR)strtoul(IniS(sec.c_str(), key, def).c_str(), nullptr, 16); };
        w.color = C("color", "FFFFFFFF");
        char hex[16]; snprintf(hex, sizeof hex, "%08X", w.color);
        w.labelColor = C("label_color", hex);
        w.bg = C("bg", "00000000");
        w.nameColor = C("name_color", hex); w.segColor = C("seg_color", hex); w.timeColor = C("time_color", hex);
        w.aheadColor = C("ahead_color", "FF40E040"); w.behindColor = C("behind_color", "FFE04848");
        w.goldColor = C("gold_color", "FFFFD700");
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
    if (s.category == "il") {
        std::string mName = s.startMap;
        for (int i = 0; i < kMissionCount; i++) {
            if (s.startMap == kMissionIds[i]) { mName = kMissionNames[i]; break; }
        }
        return "IL: " + mName + " (" + kRulesetNames[s.ruleset] + ") | " + kDifficultyShortNames[s.difficulty];
    }
    return catName + " (" + kRulesetNames[s.ruleset] + ") | " + kDifficultyShortNames[s.difficulty];
}

static const char* CategoryName(const std::string& c) {
    for (int i = 0; i < CAT_COUNT; i++) {
        if (c == kCategoryKeys[i]) return kCategoryNames[i];
    }
    return "Any%";
}

static double ReadPb(const std::string& catKey, const std::string& map) {
    std::string v = IniS(catKey.c_str(), map.c_str(), "", g_pbIni);
    if (v.empty() && catKey.find('_') != std::string::npos) {
        // Fallback to base category for legacy records
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

static void Message(const std::string& m) { g_msg = m; g_msgUntil = GetTickCount() + 2500; }

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
    for (auto& s : kSigs) if (memcmp(gx + s.rva, s.bytes, s.len)) return false;
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

static void FinishRun() {
    bool clean = g_run.mapClean;
    std::string map = g_run.map; double mapIgt = g_run.mapIgt;
    g_run.Finish();
    std::string catKey = GetPbCategoryKey(g_set);
    double pb = ReadPb(catKey, map);
    if (clean && (pb < 0 || mapIgt < pb)) {
        char v[32]; snprintf(v, sizeof v, "%.3f", mapIgt);
        WritePrivateProfileStringA(catKey.c_str(), map.c_str(), v, g_pbIni);
        g_run.splits.back().gold = true;
    }
    bool better = g_pbSplits.empty() || g_run.igt < g_pbSplits.back().time;
    if (!g_run.practice && better) {
        WritePbSplits(catKey, g_run.splits);
        Message("New PB run: " + FormatTime(g_run.igt, 3, false));
    } else Message("Run finished: " + FormatTime(g_run.igt, 2, false));
}

static void StartNewRunFromWizard() {
    g_run.Reset();
    g_run.practice = false;
    g_maxSpeed = 0;
    std::string catKey = GetPbCategoryKey(g_set);
    g_pb = ReadPb(catKey, g_set.startMap);
    g_pbSplits = ReadPbSplits(catKey);

    char buf[512];
    if (g_set.category == "il") {
        snprintf(buf, sizeof buf,
            "set g_skill %d\n"
            "map game/%s/%s\n",
            g_set.difficulty, g_set.startMap.c_str(), g_set.startMap.c_str());
        ExecCommandList(buf);
        Message("IL Run Started: " + g_set.startMap);
    } else {
        snprintf(buf, sizeof buf,
            "set g_skill %d\n"
            "NewCampaignHasStarted\n"
            "set g_postVideoCommand \"map game/trainyard/trainyard\"\n"
            "PlayVideo videos/wolf_campaign_start_vo_%s\n",
            g_set.difficulty, g_set.language.c_str());
        ExecCommandList(buf);
        Message("Speedrun Initialized: " + GetFullCategoryDisplay(g_set));
    }

    if (g_set.ruleset == RULESET_RESTRICTED) {
        ExecCommand("unbind F5\nunbind F9\n");
    }

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
    case VK_ESCAPE: return "Esc";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "Caps Lock";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "Left";
    case VK_UP: return "Up";
    case VK_RIGHT: return "Right";
    case VK_DOWN: return "Down";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num .";
    case VK_DIVIDE: return "Num /";
    case VK_OEM_3: return "~";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_4: return "[";
    case VK_OEM_6: return "]";
    case VK_OEM_1: return ";";
    case VK_OEM_7: return "'";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case 0: return "None";
    default:
        snprintf(buf, sizeof buf, "Key 0x%02X", vk);
        return buf;
    }
}

// Convert D3DCOLOR (0xAARRGGBB) to ImVec4 and back
static ImVec4 D3DToImVec4(D3DCOLOR c) {
    float a = ((c >> 24) & 0xFF) / 255.0f;
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8) & 0xFF) / 255.0f;
    float b = (c & 0xFF) / 255.0f;
    return ImVec4(r, g, b, a);
}

static D3DCOLOR ImVec4ToD3D(const ImVec4& v) {
    int a = (int)(v.w * 255.0f + 0.5f);
    int r = (int)(v.x * 255.0f + 0.5f);
    int g = (int)(v.y * 255.0f + 0.5f);
    int b = (int)(v.z * 255.0f + 0.5f);
    if (a < 0) a = 0; if (a > 255) a = 255;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
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
    // If the runner is in Restricted ruleset, physically block Quicksave and Quickload key events
    // from ever reaching Wolf2.exe engine!
    if (g_set.ruleset == RULESET_RESTRICTED && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if ((int)wParam == g_set.keys[K_QUICKSAVE] || (int)wParam == g_set.keys[K_QUICKLOAD] ||
            wParam == VK_F5 || wParam == VK_F9) {
            Message("[RESTRICTED] Quicksaves & Quickloads are FORBIDDEN!");
            return 0; // Drop keystroke completely
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
        double dt = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;

        // Live reload of srmod.ini
        if (GetTickCount() - lastIniCheck > 500) {
            lastIniCheck = GetTickCount();
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa) &&
                CompareFileTime(&fa.ftLastWriteTime, &iniTime)) {
                iniTime = fa.ftLastWriteTime;
                Settings s = LoadSettings();
                std::lock_guard<std::mutex> lk(g_mx);
                if (s.category != g_set.category || s.ruleset != g_set.ruleset || s.difficulty != g_set.difficulty) {
                    std::string catKey = GetPbCategoryKey(s);
                    g_pb = ReadPb(catKey, g_run.map);
                    g_pbSplits = ReadPbSplits(catKey);
                }
                g_set = s; g_fontsDirty = true;
            }
        }

        std::string cmds;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            bool cheat = (g_set.category == "cheat" && g_set.ruleset != RULESET_RESTRICTED);
            Snapshot s; ReadGame(&s, cheat && g_set.cheatGod);

            g_run.SetLoading(s.loading);
            g_run.paused = s.paused;
            if (s.mission && !wasMission) g_run.MissionComplete();
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
                char fovCmd[64], bobCmd[64];
                snprintf(fovCmd, sizeof fovCmd, "set g_fov %d\n", g_set.fov);
                snprintf(bobCmd, sizeof bobCmd, "set g_noViewBob %d\n", g_set.headBob ? 0 : 1);
                cmds += fovCmd;
                cmds += bobCmd;
            }

            g_havePlayer = s.player;
            if (s.player) {
                memcpy(g_pos, s.pos, sizeof g_pos);
                g_speed = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
                if (g_speed > g_maxSpeed) g_maxSpeed = g_speed;
                g_run.SetHealth(s.hp);
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
                            g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
                            Message("Run started (" + GetFullCategoryDisplay(g_set) + ")");
                        }
                        break;
                    case K_RESET: g_run.Reset(); g_maxSpeed = 0; Message("Reset"); break;
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
                        if (g_set.ruleset == RULESET_RESTRICTED) {
                            Message("[RESTRICTED] Teleport is FORBIDDEN!");
                        } else if (haveSaved && s.player) {
                            char b[256];
                            snprintf(b, sizeof b, g_set.teleportCmd.c_str(), savedPos[0], savedPos[1], savedPos[2]);
                            cmds += b; cmds += "\n"; g_run.Teleported(); Message("Teleport (practice)");
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

// ---- Rendering -------------------------------------------------------------------------------
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
        snprintf(b, sizeof b, "X %.1f  Y %.1f  Z %.1f", g_pos[0], g_pos[1], g_pos[2]); return b;
    case W_SPEED: snprintf(b, sizeof b, "%.0f u/s (max %.0f)", g_speed, g_maxSpeed); return b;
    case W_RTA: return T(g_run.rta);
    case W_IGT: return T(g_run.igt) + (g_run.loading ? " [load]" : g_run.paused ? " [paused]" : "");
    case W_MAP_RTA: return T(g_run.mapRta);
    case W_MAP_IGT: return T(g_run.mapIgt);
    case W_PB:
        if (g_pb < 0) return "--";
        return T(g_pb) + "  " + FormatDelta(g_run.mapIgt - g_pb, st.decimals);
    case W_LOADS: snprintf(b, sizeof b, "%d (", g_run.loads); return b + T(g_run.loadTime) + ")";
    case W_DEATHS: return std::to_string(g_run.deaths);
    case W_CATEGORY: return GetFullCategoryDisplay(st) + (g_run.practice ? "  [PRACTICE]" : "");
    case W_MAP: return g_run.map;
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
    int w = a.cx + b.cx, h = a.cy > b.cy ? a.cy : b.cy, pad = size / 4;
    POINT p = Place(fx, fy, anchor, w, h, W, H);
    FillRect(dev, p.x - pad, p.y - pad / 2, p.x + w + pad, p.y + h + pad / 2, bg);
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
    int totalW = colW[0] + colW[1] + colW[2] + colW[3] + 3 * gap, totalH = lineH * (int)rows.size();
    POINT p = Place(w.x, w.y, w.anchor, totalW, totalH, W, H);
    FillRect(dev, p.x - pad, p.y - pad / 2, p.x + totalW + pad, p.y + totalH + pad / 2, w.bg);

    const D3DCOLOR colColor[4] = {w.nameColor, w.segColor, 0, w.timeColor};
    int y = p.y;
    for (auto& r : rows) {
        int x = p.x;
        for (int c = 0; c < 4; c++) {
            Text(f, r.col[c], RECT{x, y, x + colW[c], y + lineH}, c == 0 ? DT_LEFT : DT_RIGHT,
                 c == 2 ? r.deltaColor : colColor[c], st.shadow);
            x += colW[c] + gap;
        }
        y += lineH;
    }
}

// BXT-Style New Run Setup Wizard
static void DrawNewRunWizard(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(780, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 390.0f, io.DisplaySize.y * 0.5f - 280.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Wolfenstein (2009) Speedrun Setup - BXT Wizard", &g_wizardOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "=== NEW SPEEDRUN CAMPAIGN WIZARD ===");
        ImGui::TextDisabled("Select category, ruleset and difficulty before starting a fresh run.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 1: CATEGORY SELECTION
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "STEP 1: Speedrun Category (Категория)");
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

        // Category summary box
        ImGui::BeginChild("CategoryDesc", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Goal: %s", kCategorySummaries[currentCatIdx]);
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Timing: %s", kCategoryTimingRules[currentCatIdx]);
        ImGui::EndChild();

        if (g_set.category == "il") {
            int selMission = 0;
            for (int i = 0; i < kMissionCount; i++) {
                if (g_set.startMap == kMissionIds[i]) { selMission = i; break; }
            }
            if (ImGui::Combo("Select Mission for IL", &selMission, kMissionNames, kMissionCount)) {
                g_set.startMap = kMissionIds[selMission]; SaveSettings(g_set);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 2: RULESET SELECTION
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "STEP 2: Ruleset (Рестрикт / Без ограничений)");
        if (ImGui::RadioButton("Restricted (No Quicksave / Glitchless)", g_set.ruleset == RULESET_RESTRICTED)) {
            g_set.ruleset = RULESET_RESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map);
            g_pbSplits = ReadPbSplits(catKey);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Unrestricted (All Skips & Quicksaves Allowed)", g_set.ruleset == RULESET_UNRESTRICTED)) {
            g_set.ruleset = RULESET_UNRESTRICTED; SaveSettings(g_set);
            std::string catKey = GetPbCategoryKey(g_set);
            g_pb = ReadPb(catKey, g_run.map);
            g_pbSplits = ReadPbSplits(catKey);
        }

        // Ruleset enforcement banner
        ImGui::BeginChild("RulesetDesc", ImVec2(0, 65), true);
        if (g_set.ruleset == RULESET_RESTRICTED) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[!] RESTRICTED ENFORCEMENT ACTIVE:");
            ImGui::TextWrapped("Quicksaves (F5) and Quickloads (F9) are BLOCKED by the mod proxy.");
            ImGui::TextDisabled("Warps and developer cheats are prohibited. FOV, head bobbing, and checkpoint reloads are permitted.");
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[*] UNRESTRICTED RULES ACTIVE:");
            ImGui::TextWrapped("Quicksaves (F5) & Quickloads (F9) are allowed. Mission replay warp glitch, geometry clips and out-of-bounds skips permitted.");
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // STEP 3: DIFFICULTY SELECTION
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "STEP 3: Difficulty (Сложность g_skill)");
        for (int i = 0; i < DIFF_COUNT; i++) {
            if (ImGui::RadioButton(kDifficultyNames[i], g_set.difficulty == i)) {
                g_set.difficulty = i; SaveSettings(g_set);
            }
            if (i < DIFF_COUNT - 1) ImGui::SameLine();
        }
        ImGui::TextDisabled("Competitive community standard for Any%% runs is Über difficulty (g_skill 3).");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // LAUNCH BUTTONS
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.2f, 1.0f), "Summary: %s", GetFullCategoryDisplay(g_set).c_str());
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.75f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.50f, 0.20f, 1.0f));
        if (ImGui::Button("START RUN NOW (Launch Campaign & Cutscene)", ImVec2(380, 36))) {
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

// In-Game Configuration Menu (ImGui)
static void DrawInGameMenu(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(820, 580), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 410.0f, io.DisplaySize.y * 0.5f - 290.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Wolfenstein (2009) Speedrun Mod - Settings", &g_menuOpen, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("SettingsTabs")) {
            // TAB 1: Game & Visuals
            if (ImGui::BeginTabItem("Game & Visuals")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Field of View (FOV)");
                if (ImGui::SliderInt("##FOV", &g_set.fov, 60, 120, "%d deg")) {
                    char cmd[64]; snprintf(cmd, sizeof cmd, "set g_fov %d\n", g_set.fov);
                    ExecCommand(cmd); SaveSettings(g_set);
                }
                ImGui::SameLine();
                if (ImGui::Button("80 (Default)")) {
                    g_set.fov = 80; char cmd[64]; snprintf(cmd, sizeof cmd, "set g_fov %d\n", g_set.fov);
                    ExecCommand(cmd); SaveSettings(g_set);
                }
                ImGui::SameLine();
                if (ImGui::Button("90")) {
                    g_set.fov = 90; char cmd[64]; snprintf(cmd, sizeof cmd, "set g_fov %d\n", g_set.fov);
                    ExecCommand(cmd); SaveSettings(g_set);
                }
                ImGui::SameLine();
                if (ImGui::Button("100")) {
                    g_set.fov = 100; char cmd[64]; snprintf(cmd, sizeof cmd, "set g_fov %d\n", g_set.fov);
                    ExecCommand(cmd); SaveSettings(g_set);
                }
                ImGui::SameLine();
                if (ImGui::Button("110")) {
                    g_set.fov = 110; char cmd[64]; snprintf(cmd, sizeof cmd, "set g_fov %d\n", g_set.fov);
                    ExecCommand(cmd); SaveSettings(g_set);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Head & Weapon Bobbing (Раскачивание камеры / оружия)");
                if (ImGui::Checkbox("Enable View & Weapon Bobbing", &g_set.headBob)) {
                    if (g_set.headBob) ExecCommand("set g_noViewBob 0\n");
                    else ExecCommand("set g_noViewBob 1\n");
                    SaveSettings(g_set);
                }
                ImGui::TextDisabled(g_set.headBob ? "View bobbing is ENABLED (standard game behavior)."
                                                  : "View bobbing is DISABLED (g_noViewBob 1) - removes screen shaking during sprint/run.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Game Language (sys_lang)");
                int currentLangIdx = 0;
                for (int i = 0; i < kLangCount; i++) {
                    if (g_set.language == kLanguages[i]) { currentLangIdx = i; break; }
                }
                if (ImGui::Combo("##Language", &currentLangIdx, kLanguages, kLangCount)) {
                    g_set.language = kLanguages[currentLangIdx];
                    ExecCommand(std::string("set sys_lang \"") + g_set.language + "\"\n");
                    SaveSettings(g_set);
                }
                ImGui::TextDisabled("Changes game language (subtitles/voice packs). Applied to launch configuration.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quick Actions");
                if (g_set.ruleset == RULESET_RESTRICTED) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[RESTRICTED MODE: Quick Save & Quick Load are BLOCKED]");
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Quick Save (F5)", ImVec2(140, 26))) {
                    ExecCommand("contentManagerCommand save full\n");
                    Message("Quick Save");
                }
                ImGui::SameLine();
                if (ImGui::Button("Quick Load (F9)", ImVec2(140, 26))) {
                    ExecCommand("contentManagerCommand loadLast\n");
                    Message("Quick Load");
                }
                if (g_set.ruleset == RULESET_RESTRICTED) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (ImGui::Button("Restart Checkpoint (F11)", ImVec2(190, 26))) {
                    ExecCommand("ScaleformDiedRestart\n");
                    Message("Restart Checkpoint");
                }

                ImGui::EndTabItem();
            }

            // TAB 2: HUD & Widgets
            if (ImGui::BeginTabItem("HUD & Widgets")) {
                ImGui::Spacing();
                ImGui::Checkbox("Show Overlay", &g_visible);
                ImGui::SameLine();
                if (ImGui::Checkbox("Text Shadow", &g_set.shadow)) SaveSettings(g_set);
                ImGui::SameLine();
                if (ImGui::Checkbox("Always Show Hours", &g_set.hoursAlways)) SaveSettings(g_set);

                ImGui::Spacing();
                ImGui::Columns(2, "WidgetColumns", true);

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Select Widget");
                ImGui::BeginChild("WidgetList", ImVec2(0, 340), true);
                for (int i = 0; i < W_COUNT; i++) {
                    char label[128];
                    snprintf(label, sizeof label, "[%s] %s", g_set.w[i].on ? "X" : " ", kWidgetDisplayNames[i]);
                    if (ImGui::Selectable(label, g_selectedWidget == i)) g_selectedWidget = i;
                }
                ImGui::EndChild();

                ImGui::NextColumn();

                Widget& sel = g_set.w[g_selectedWidget];
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", kWidgetDisplayNames[g_selectedWidget]);
                if (ImGui::Checkbox("Widget Enabled", &sel.on)) SaveSettings(g_set);

                char lblBuf[128]; snprintf(lblBuf, sizeof lblBuf, "%s", sel.label.c_str());
                if (ImGui::InputText("Label Text", lblBuf, sizeof lblBuf)) {
                    sel.label = lblBuf; SaveSettings(g_set);
                }

                if (ImGui::SliderInt("Size (px)", &sel.size, 8, 120)) {
                    g_fontsDirty = true; SaveSettings(g_set);
                }
                if (ImGui::SliderFloat("Position X", &sel.x, 0.0f, 1.0f, "%.4f")) SaveSettings(g_set);
                if (ImGui::SliderFloat("Position Y", &sel.y, 0.0f, 1.0f, "%.4f")) SaveSettings(g_set);

                const char* anchorNames[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right", "Center" };
                if (ImGui::Combo("Anchor", &sel.anchor, anchorNames, 5)) SaveSettings(g_set);

                ImVec4 cVal = D3DToImVec4(sel.color);
                if (ImGui::ColorEdit4("Value Color", (float*)&cVal, ImGuiColorEditFlags_AlphaBar)) {
                    sel.color = ImVec4ToD3D(cVal); SaveSettings(g_set);
                }
                ImVec4 cLbl = D3DToImVec4(sel.labelColor);
                if (ImGui::ColorEdit4("Label Color", (float*)&cLbl, ImGuiColorEditFlags_AlphaBar)) {
                    sel.labelColor = ImVec4ToD3D(cLbl); SaveSettings(g_set);
                }
                ImVec4 cBg = D3DToImVec4(sel.bg);
                if (ImGui::ColorEdit4("Background Box", (float*)&cBg, ImGuiColorEditFlags_AlphaBar)) {
                    sel.bg = ImVec4ToD3D(cBg); SaveSettings(g_set);
                }

                if (g_selectedWidget == W_SPLITS) {
                    ImGui::Separator();
                    ImGui::Text("Splits Table Colors:");
                    ImVec4 cName = D3DToImVec4(sel.nameColor);
                    if (ImGui::ColorEdit4("Map Name", (float*)&cName, ImGuiColorEditFlags_AlphaBar)) { sel.nameColor = ImVec4ToD3D(cName); SaveSettings(g_set); }
                    ImVec4 cSeg = D3DToImVec4(sel.segColor);
                    if (ImGui::ColorEdit4("Segment Time", (float*)&cSeg, ImGuiColorEditFlags_AlphaBar)) { sel.segColor = ImVec4ToD3D(cSeg); SaveSettings(g_set); }
                    ImVec4 cTime = D3DToImVec4(sel.timeColor);
                    if (ImGui::ColorEdit4("Split Time", (float*)&cTime, ImGuiColorEditFlags_AlphaBar)) { sel.timeColor = ImVec4ToD3D(cTime); SaveSettings(g_set); }
                    ImVec4 cAhead = D3DToImVec4(sel.aheadColor);
                    if (ImGui::ColorEdit4("Ahead (-Delta)", (float*)&cAhead, ImGuiColorEditFlags_AlphaBar)) { sel.aheadColor = ImVec4ToD3D(cAhead); SaveSettings(g_set); }
                    ImVec4 cBehind = D3DToImVec4(sel.behindColor);
                    if (ImGui::ColorEdit4("Behind (+Delta)", (float*)&cBehind, ImGuiColorEditFlags_AlphaBar)) { sel.behindColor = ImVec4ToD3D(cBehind); SaveSettings(g_set); }
                    ImVec4 cGold = D3DToImVec4(sel.goldColor);
                    if (ImGui::ColorEdit4("Best Segment (Gold)", (float*)&cGold, ImGuiColorEditFlags_AlphaBar)) { sel.goldColor = ImVec4ToD3D(cGold); SaveSettings(g_set); }
                }

                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            // TAB 3: Keybinds
            if (ImGui::BeginTabItem("Keybinds")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Hotkeys & Binds");
                ImGui::TextDisabled("Click 'Rebind' and press any key on your keyboard or mouse keypad.");

                ImGui::Spacing();
                ImGui::BeginChild("KeyTable", ImVec2(0, 360), true);
                for (int i = 0; i < K_COUNT; i++) {
                    ImGui::PushID(i);
                    ImGui::Text("%-28s :", kKeyTitles[i]);
                    ImGui::SameLine(240);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[ %s ]", GetKeyDisplayName(g_set.keys[i]));
                    ImGui::SameLine(360);
                    if (g_rebindKeyIndex == i) {
                        ImGui::Button("Press any key... (Esc to cancel)", ImVec2(240, 24));
                    } else {
                        if (ImGui::Button("Rebind", ImVec2(100, 24))) g_rebindKeyIndex = i;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                if (ImGui::Button("Reset All Keybinds to Defaults")) {
                    for (int i = 0; i < K_COUNT; i++) g_set.keys[i] = kKeyDefaults[i];
                    SaveSettings(g_set);
                }
                ImGui::EndTabItem();
            }

            // TAB 4: Categories & Rules
            if (ImGui::BeginTabItem("Categories & Rules")) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.60f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.70f, 0.30f, 1.0f));
                if (ImGui::Button("Open Start New Run Wizard (F2)", ImVec2(280, 30))) {
                    g_wizardOpen = true;
                    g_menuOpen = false;
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::TextDisabled("(Interactive category, ruleset & difficulty setup)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Speedrun Category");
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

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Ruleset Enforcement");
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
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[!] RESTRICTED: Quicksaves & Quickloads are BLOCKED. Cheats/warps forbidden.");
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[*] UNRESTRICTED: Quicksaves, quickloads, mission replay warps and skips allowed.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Game Difficulty (g_skill)");
                for (int i = 0; i < DIFF_COUNT; i++) {
                    if (ImGui::RadioButton(kDifficultyNames[i], g_set.difficulty == i)) {
                        g_set.difficulty = i; SaveSettings(g_set);
                    }
                    if (i < DIFF_COUNT - 1) ImGui::SameLine();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Timer Reset Triggers & Warp Protection");
                if (ImGui::Checkbox("Auto-start & reset on campaign cutscene (wolf_campaign_start_vo_english.bik)", &g_set.autoStartCutscene))
                    SaveSettings(g_set);
                ImGui::TextDisabled("Resets run when campaign intro cutscene starts. IGT pauses during cutscenes.");

                if (ImGui::Checkbox("Auto-start & reset on map load (Individual Level IL runs only)", &g_set.autoStartMap))
                    SaveSettings(g_set);
                if (g_set.autoStartMap) {
                    int selMission = 0;
                    for (int i = 0; i < kMissionCount; i++) {
                        if (g_set.startMap == kMissionIds[i]) { selMission = i; break; }
                    }
                    if (ImGui::Combo("Start Mission", &selMission, kMissionNames, kMissionCount)) {
                        g_set.startMap = kMissionIds[selMission]; SaveSettings(g_set);
                    }
                }
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "WARP PROTECTION: Mid-run mission replays/warps will NEVER reset an active run.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Cheat% Configuration");
                if (ImGui::Checkbox("Lock God Mode (Cheat% only)", &g_set.cheatGod)) SaveSettings(g_set);

                char cmdBuf[512]; snprintf(cmdBuf, sizeof cmdBuf, "%s", g_set.cheatCmds.c_str());
                if (ImGui::InputText("Commands on Map Load", cmdBuf, sizeof cmdBuf)) {
                    g_set.cheatCmds = cmdBuf; SaveSettings(g_set);
                }
                char tpBuf[512]; snprintf(tpBuf, sizeof tpBuf, "%s", g_set.teleportCmd.c_str());
                if (ImGui::InputText("Teleport Command Template", tpBuf, sizeof tpBuf)) {
                    g_set.teleportCmd = tpBuf; SaveSettings(g_set);
                }

                ImGui::EndTabItem();
            }

            // TAB 5: Records & Splits
            if (ImGui::BeginTabItem("Records & Splits")) {
                ImGui::Spacing();
                std::string catKey = GetPbCategoryKey(g_set);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Best Run Splits - %s", GetFullCategoryDisplay(g_set).c_str());
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

    // Initialize ImGui on first valid frame
    if (!g_imguiInit) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
            g_gameHwnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();
            if (g_gameHwnd) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr; // Don't use imgui.ini; settings saved in srmod.ini
                ImGui::StyleColorsDark();
                ImGuiStyle& style = ImGui::GetStyle();
                style.WindowRounding = 6.0f;
                style.FrameRounding = 4.0f;
                style.PopupRounding = 4.0f;
                style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
                style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.22f, 0.30f, 1.0f);

                ImGui_ImplWin32_Init(g_gameHwnd);
                ImGui_ImplDX9_Init(dev);

                g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
                g_imguiInit = true;
                Log("ImGui initialized on HWND %p", g_gameHwnd);
            }
        }
    }

    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);

    if (SUCCEEDED(dev->BeginScene())) {
        int W = desc.Width, H = desc.Height;
        const Settings& st = g_set;

        // Draw Speedrun Overlay widgets (if visible)
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
            if (GetTickCount() < g_msgUntil)
                DrawLine(dev, st.font, 30, true, "", 0, g_msg, 0xFFFFD040, 0x90000000, 0.5f, 0.2f, 4, W, H);
            if (!g_menuOpen && !g_wizardOpen)
                DrawLine(dev, st.font, 14, false, "", 0, "[F1] Settings  |  [F2] Start Run Wizard", 0x80FFFFFF, 0x40000000, 0.005f, 0.995f, 2, W, H);
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

static bool InstallD3DHooks() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) return false;
    auto create = (IDirect3D9 * (WINAPI*)(UINT))GetProcAddress(d3d9, "Direct3DCreate9");
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return false;
    HWND wnd = CreateWindowA("STATIC", "srmod", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        void** vt = *(void***)dev;
        Hook(vt, 17, (void*)hkPresent, (void**)&oPresent);
        Hook(vt, 16, (void*)hkReset, (void**)&oReset);
        dev->Release();
    } else Log("dummy device failed: 0x%08lx", hr);
    d3d->Release();
    DestroyWindow(wnd);
    return ok;
}

static DWORD WINAPI MainThread(LPVOID) {
    timeBeginPeriod(1);
    HMODULE dx = LoadLibraryA("d3dx9_43.dll");
    g_createFont = dx ? (D3DXCreateFontA_t)GetProcAddress(dx, "D3DXCreateFontA") : nullptr;
    if (!g_createFont) Log("d3dx9_43.dll / D3DXCreateFontA not found: install DirectX End-User Runtime");

    g_set = LoadSettings();
    g_pbSplits = ReadPbSplits(GetPbCategoryKey(g_set));
    g_session = FindSession();
    Log("session: %p", g_session);

    bool hooked = false;
    while (!hooked || !GetModuleHandleA("Gamex86.dll")) {
        if (!hooked && GetModuleHandleA("d3d9.dll")) hooked = InstallD3DHooks();
        Sleep(100);
    }
    BYTE* gx = (BYTE*)GetModuleHandleA("Gamex86.dll");
    g_supported = CheckGameDll(gx);
    Log("Gamex86.dll at %p, supported=%d", gx, g_supported);
    Poll();
    return 0;
}

// ---- Bink exports we wrap (the rest are forwarded to binkw32_orig.dll by binkw32.def) --------
static FARPROC BinkOrig(const char* name) {
    static HMODULE orig = LoadLibraryA("binkw32_orig.dll");
    return orig ? GetProcAddress(orig, name) : nullptr;
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
                g_run.Start();
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
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

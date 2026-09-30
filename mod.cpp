// srmod: Wolfenstein (2009) speedrun overlay. Loaded as a binkw32.dll proxy (exports forwarded
// to binkw32_orig.dll via binkw32.def). A poll thread reads game memory and drives the timers;
// a hooked IDirect3DDevice9::Present draws the overlay.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <timeapi.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "timer.h"

#pragma comment(lib, "winmm.lib")

// ---- Addresses (Wolfenstein 1.21 SP, patched Gamex86.dll 6739216 bytes) -------------------
// Gamex86.dll RVAs
const DWORD RVA_GAMELOCAL      = 0x61f0b0;  // idGameLocal gameLocal
const DWORD RVA_CMDSYSTEM      = 0x61f050;  // idCmdSystem *cmdSystem
const DWORD RVA_PHYS_GETORIGIN = 0x2f8760;  // idPhysics_Player::GetOrigin (shared by rvHkPhysics_Player)
const DWORD GL_LOCALCLIENT     = 0x63d4c;   // int localClientNum
const DWORD GL_ENTITIES        = 0x4d0;     // idEntity *entities[]
const DWORD GL_MAPNAME         = 0x63e28;   // idStr mapFileName data pointer
const DWORD GL_INCINEMATIC     = 0x63bdc;   // bool inCinematic (set by SetCamera)
const DWORD RVA_MISSIONCOMPLETE = 0x875e8c; // mission complete menu object (from the community ASL)
const DWORD MC_ACTIVE          = 0x268;     // bool: mission complete screen shown
const DWORD PL_PHYSICS         = 0x2ec;     // idPhysics *physics
const DWORD PL_HEALTH          = 0xd8;      // float health
const DWORD PL_GODMODE         = 0x5fa;     // bool godmode
const DWORD PH_ORIGIN          = 0x9c;      // idVec3 current.origin
const DWORD PH_VELOCITY        = 0xa8;      // idVec3 current.velocity
const int   CMD_BUFFERTEXT_IDX = 8;         // idCmdSystem::BufferCommandText vtable slot
const int   CMD_EXEC_APPEND    = 2;
// Wolf2.exe (fixed base 0x10000000)
const DWORD WOLF_SESSION_VT    = 0x105e78cc; // idSessionLocal vtable; the object lives in .data
const DWORD SESS_INSIDE_MAPCHANGE = 0xd;     // bool insideExecuteMapChange (set during all loads)

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
const char* kWidgetLabels[W_COUNT] = {"", "Speed ", "RTA ", "IGT ", "Map RTA ", "Map IGT ",
                                      "PB ", "Loads ", "Deaths ", "", "", ""};
const D3DCOLOR COLOR_AHEAD = 0xFF40E040, COLOR_BEHIND = 0xFFE04848, COLOR_GOLD = 0xFFFFD700;

struct Widget { bool on; float x, y; int anchor, size; D3DCOLOR color; std::string label; };
enum Key { K_TOGGLE, K_STARTSTOP, K_RESET, K_CATEGORY, K_SAVEPOS, K_TELEPORT, K_COUNT };
const char* kKeyNames[K_COUNT] = {"toggle", "start_stop", "reset", "category", "save_pos", "teleport"};
const int kKeyDefaults[K_COUNT] = {VK_F6, VK_F7, VK_F8, VK_F10, VK_NUMPAD7, VK_NUMPAD9};

struct Settings {
    Widget w[W_COUNT];
    int keys[K_COUNT];
    int decimals = 2, splitLines = 10;
    bool hoursAlways = false, autoStart = true, cheatGod = true, shadow = true;
    std::string font = "Consolas", category = "any", startMap = "trainyard";
    std::string cheatCmds, teleportCmd;
};

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
        bool splits = i == W_SPLITS;  // splits default to the top right corner
        w.x = IniF(sec.c_str(), "x", splits ? 0.99f : 0.01f);
        w.y = IniF(sec.c_str(), "y", splits ? 0.02f : 0.02f + 0.03f * i);
        w.anchor = IniI(sec.c_str(), "anchor", splits ? 1 : 0);
        w.size = IniI(sec.c_str(), "size", 22);
        w.color = strtoul(IniS(sec.c_str(), "color", "FFFFFFFF").c_str(), nullptr, 16);
        w.label = IniS(sec.c_str(), "label", kWidgetLabels[i]);
    }
    for (int i = 0; i < K_COUNT; i++) s.keys[i] = IniI("keys", kKeyNames[i], kKeyDefaults[i]);
    s.decimals = IniI("general", "decimals", 2);
    s.splitLines = IniI("general", "split_lines", 10);
    s.hoursAlways = IniI("general", "hours_always", 0) != 0;
    s.autoStart = IniI("general", "auto_start", 1) != 0;
    s.shadow = IniI("general", "shadow", 1) != 0;
    s.font = IniS("general", "font", "Consolas");
    s.category = IniS("general", "category", "any");
    s.startMap = IniS("general", "start_map", "trainyard");
    s.cheatGod = IniI("cheat", "god", 1) != 0;
    s.cheatCmds = IniS("cheat", "commands", "give all;giveAllPowerUpgrades;momoney");
    s.teleportCmd = IniS("cheat", "teleport_cmd", "script $player1.setOrigin('%.3f %.3f %.3f');");
    return s;
}

static const char* CategoryName(const std::string& c) { return c == "cheat" ? "Cheat%" : "Any%"; }

static double ReadPb(const std::string& cat, const std::string& map) {
    std::string v = IniS(cat.c_str(), map.c_str(), "", g_pbIni);
    return v.empty() ? -1 : atof(v.c_str());
}

// Best full run: [splits.<category>] n=, map0=, time0=, ... in srmod_pb.ini
static std::vector<Split> ReadPbSplits(const std::string& cat) {
    std::string sec = "splits." + cat;
    std::vector<Split> v;
    int n = GetPrivateProfileIntA(sec.c_str(), "n", 0, g_pbIni);
    for (int i = 0; i < n; i++) {
        std::string k = std::to_string(i);
        double t = atof(IniS(sec.c_str(), ("time" + k).c_str(), "0", g_pbIni).c_str());
        v.push_back(Split{IniS(sec.c_str(), ("map" + k).c_str(), "", g_pbIni), t - (v.empty() ? 0 : v.back().time), t, false});
    }
    return v;
}

static void WritePbSplits(const std::string& cat, const std::vector<Split>& v) {
    std::string sec = "splits." + cat;
    WritePrivateProfileStringA(sec.c_str(), nullptr, nullptr, g_pbIni);  // drop old run
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

// SEH-guarded: pointers can be stale mid-load.
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
            const char* base = m;  // "maps/game/trainyard.map" -> "trainyard"
            for (const char* c = m; *c && c - m < 256; c++) if (*c == '/' || *c == '\\') base = c + 1;
            int n = 0;
            while (base[n] && base[n] != '.' && n < 63) { s->map[n] = base[n]; n++; }
        }
        BYTE* pl = LocalPlayer(gx);
        if (!pl) return;
        BYTE* ph = *(BYTE**)(pl + PL_PHYSICS);
        if (!ph || (*(DWORD**)ph)[38] != (DWORD)(gx + RVA_PHYS_GETORIGIN)) return;  // vtable slot 38 = GetOrigin
        memcpy(s->pos, ph + PH_ORIGIN, 12);
        memcpy(s->vel, ph + PH_VELOCITY, 12);
        s->hp = *(float*)(pl + PL_HEALTH);
        s->player = true;
        if (writeGod && s->hp > 0) pl[PL_GODMODE] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->player = false;
    }
}

// ponytail: BufferCommandText is called from our thread, not the game thread; the engine's
// command buffer has no lock. Fine for rare hotkey/cheat use; move into a game-thread hook if it ever races.
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

// ---- Poll thread: timers, hotkeys, ini reload ------------------------------------------------
static bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Final split; saves the map PB for the last map and the whole run as PB if faster. Caller holds g_mx.
static void FinishRun() {
    bool clean = g_run.mapClean;
    std::string map = g_run.map; double mapIgt = g_run.mapIgt;
    g_run.Finish();
    double pb = ReadPb(g_set.category, map);
    if (clean && (pb < 0 || mapIgt < pb)) {
        char v[32]; snprintf(v, sizeof v, "%.3f", mapIgt);
        WritePrivateProfileStringA(g_set.category.c_str(), map.c_str(), v, g_pbIni);
        g_run.splits.back().gold = true;
    }
    bool better = g_pbSplits.empty() || g_run.igt < g_pbSplits.back().time;
    if (!g_run.practice && better) {
        WritePbSplits(g_set.category, g_run.splits);
        Message("New PB run: " + FormatTime(g_run.igt, 3, false));
    } else Message("Run finished");
}

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

        // Live reload of srmod.ini (written by the launcher or by us).
        if (GetTickCount() - lastIniCheck > 500) {
            lastIniCheck = GetTickCount();
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa) &&
                CompareFileTime(&fa.ftLastWriteTime, &iniTime)) {
                iniTime = fa.ftLastWriteTime;
                Settings s = LoadSettings();
                std::lock_guard<std::mutex> lk(g_mx);
                if (s.category != g_set.category) { g_pb = ReadPb(s.category, g_run.map); g_pbSplits = ReadPbSplits(s.category); }
                g_set = s; g_fontsDirty = true;
            }
        }

        std::string cmds;  // executed after releasing the lock
        {
            std::lock_guard<std::mutex> lk(g_mx);
            bool cheat = g_set.category == "cheat";
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
            if (!s.loading && s.map[0] && g_run.map != s.map) {
                std::string done; double t;
                if (g_run.MapLoaded(s.map, &done, &t)) {
                    double pb = ReadPb(g_set.category, done);
                    if (pb < 0 || t < pb) {
                        char v[32]; snprintf(v, sizeof v, "%.3f", t);
                        WritePrivateProfileStringA(g_set.category.c_str(), done.c_str(), v, g_pbIni);
                        Message("New PB on " + done + ": " + FormatTime(t, 3, false));
                        if (!g_run.splits.empty() && g_run.splits.back().map == done) g_run.splits.back().gold = true;
                    }
                }
                // Entering the start map (from another map) resets and starts a new run.
                if (g_set.autoStart && !g_set.startMap.empty() &&
                    _stricmp(g_run.map.c_str(), g_set.startMap.c_str()) == 0) {
                    g_run.Start(); g_run.mapClean = true;
                    g_pbSplits = ReadPbSplits(g_set.category);
                    Message("Run started");
                }
                g_pb = ReadPb(g_set.category, g_run.map);
                g_maxSpeed = 0;
                cheatsPending = true;
            }

            g_havePlayer = s.player;
            if (s.player) {
                memcpy(g_pos, s.pos, sizeof g_pos);
                g_speed = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
                if (g_speed > g_maxSpeed) g_maxSpeed = g_speed;
                g_run.SetHealth(s.hp);
                // Cheat%: give items once per map, 1 s after control is gained.
                if (cheat && cheatsPending && s.hp > 0 && GetTickCount() - loadEndTick > 1000) {
                    cmds = g_set.cheatCmds; cheatsPending = false;
                }
            }

            if (GameFocused()) {
                for (int k = 0; k < K_COUNT; k++) {
                    bool down = (GetAsyncKeyState(g_set.keys[k]) & 0x8000) != 0;
                    bool pressed = down && !keyDown[k];
                    keyDown[k] = down;
                    if (!pressed) continue;
                    switch (k) {
                    case K_TOGGLE: g_visible = !g_visible; break;
                    case K_STARTSTOP:
                        if (g_run.state == Run::Running) FinishRun();
                        else { g_run.Start(); g_pbSplits = ReadPbSplits(g_set.category); Message("Run started"); }
                        break;
                    case K_RESET: g_run.Reset(); g_maxSpeed = 0; Message("Reset"); break;
                    case K_CATEGORY: {
                        std::string c = g_set.category == "cheat" ? "any" : "cheat";
                        WritePrivateProfileStringA("general", "category", c.c_str(), g_ini);
                        g_set.category = c; g_pb = ReadPb(c, g_run.map); g_pbSplits = ReadPbSplits(c);
                        if (c == "cheat") cheatsPending = true;
                        Message(std::string("Category: ") + CategoryName(c));
                        break;
                    }
                    case K_SAVEPOS:
                        if (s.player) { memcpy(savedPos, s.pos, sizeof savedPos); haveSaved = true; Message("Position saved"); }
                        break;
                    case K_TELEPORT:
                        if (haveSaved && s.player) {
                            char b[256];
                            snprintf(b, sizeof b, g_set.teleportCmd.c_str(), savedPos[0], savedPos[1], savedPos[2]);
                            cmds = b; g_run.Teleported(); Message("Teleport (practice)");
                        }
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
static std::map<int, ID3DXFont*> g_fonts;  // by pixel size

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
    case W_CATEGORY: return std::string(CategoryName(st.category)) + (g_run.practice ? "  PRACTICE" : "");
    case W_MAP: return g_run.map;
    }
    return "";
}

static void DrawLine(IDirect3DDevice9* dev, const std::string& face, int size, D3DCOLOR color, bool shadow,
                     const std::string& text, float fx, float fy, int anchor, int W, int H) {
    ID3DXFont* f = Font(dev, size, face);
    if (!f || text.empty()) return;
    RECT r = {0, 0, 0, 0};
    f->DrawTextA(nullptr, text.c_str(), -1, &r, DT_CALCRECT | DT_NOCLIP, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = int(fx * W), y = int(fy * H);
    if (anchor == 1 || anchor == 3) x -= w;   // right-anchored
    if (anchor == 2 || anchor == 3) y -= h;   // bottom-anchored
    if (anchor == 4) { x -= w / 2; y -= h / 2; }  // centered
    if (shadow) {
        RECT s = {x + 2, y + 2, x + w + 2, y + h + 2};
        f->DrawTextA(nullptr, text.c_str(), -1, &s, DT_NOCLIP, D3DCOLOR_ARGB((color >> 24) & 0xFF, 0, 0, 0));
    }
    RECT d = {x, y, x + w, y + h};
    f->DrawTextA(nullptr, text.c_str(), -1, &d, DT_NOCLIP, color);
}

// LiveSplit-style table: map | segment | delta vs PB run | split time. Last row is the current map (live).
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
        Row r; r.col[0] = map; r.col[1] = T(seg); r.col[3] = T(time); r.deltaColor = w.color;
        if (i < g_pbSplits.size() && g_pbSplits[i].map == map) {
            double d = time - g_pbSplits[i].time;
            r.col[2] = FormatDelta(d, st.decimals);
            r.deltaColor = d < 0 ? COLOR_AHEAD : COLOR_BEHIND;
        }
        if (!live && sp[i].gold) r.deltaColor = COLOR_GOLD;
        if (live) r.col[1] = "";
        rows.push_back(r);
    }
    if (rows.empty()) return;

    int colW[4] = {}, lineH = 0, gap = w.size;
    for (auto& r : rows)
        for (int c = 0; c < 4; c++) {
            RECT m = {0, 0, 0, 0};
            f->DrawTextA(nullptr, r.col[c].empty() ? " " : r.col[c].c_str(), -1, &m, DT_CALCRECT | DT_NOCLIP, 0);
            if (m.right > colW[c]) colW[c] = m.right;
            if (m.bottom > lineH) lineH = m.bottom;
        }
    int totalW = colW[0] + colW[1] + colW[2] + colW[3] + 3 * gap, totalH = lineH * (int)rows.size();
    int x0 = int(w.x * W), y = int(w.y * H);
    if (w.anchor == 1 || w.anchor == 3) x0 -= totalW;
    if (w.anchor == 2 || w.anchor == 3) y -= totalH;
    if (w.anchor == 4) { x0 -= totalW / 2; y -= totalH / 2; }

    for (auto& r : rows) {
        int x = x0;
        for (int c = 0; c < 4; c++) {
            // Name left-aligned, numbers right-aligned in their column.
            RECT d = {x, y, x + colW[c], y + lineH};
            DWORD fmt = DT_NOCLIP | (c == 0 ? DT_LEFT : DT_RIGHT);
            D3DCOLOR col = c == 2 ? r.deltaColor : w.color;
            if (st.shadow) {
                RECT s = {d.left + 2, d.top + 2, d.right + 2, d.bottom + 2};
                f->DrawTextA(nullptr, r.col[c].c_str(), -1, &s, fmt, D3DCOLOR_ARGB((col >> 24) & 0xFF, 0, 0, 0));
            }
            f->DrawTextA(nullptr, r.col[c].c_str(), -1, &d, fmt, col);
            x += colW[c] + gap;
        }
        y += lineH;
    }
}

static void DrawOverlay(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_fontsDirty) { ReleaseFonts(); g_fontsDirty = false; }
    if (!g_visible) return;

    IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    D3DVIEWPORT9 vp; dev->GetViewport(&vp);
    dev->GetRenderTarget(0, &oldRt);
    dev->SetRenderTarget(0, bb);

    if (SUCCEEDED(dev->BeginScene())) {
        int W = desc.Width, H = desc.Height;
        const Settings& st = g_set;
        if (!g_supported)
            DrawLine(dev, st.font, 22, 0xFFFF5050, true, "srmod: unsupported game version (see srmod.log)",
                     0.5f, 0.05f, 4, W, H);
        for (int i = 0; i < W_COUNT; i++) {
            const Widget& w = st.w[i];
            if (!w.on) continue;
            if (i == W_SPLITS) DrawSplits(dev, st, w, W, H);
            else DrawLine(dev, st.font, w.size, w.color, st.shadow, w.label + WidgetText(i, st), w.x, w.y, w.anchor, W, H);
        }
        if (GetTickCount() < g_msgUntil)
            DrawLine(dev, st.font, 30, 0xFFFFD040, true, g_msg, 0.5f, 0.2f, 4, W, H);
        dev->EndScene();
    }

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
    }
    HRESULT hr = oReset(dev, pp);
    std::lock_guard<std::mutex> lk(g_mx);
    if (SUCCEEDED(hr)) for (auto& f : g_fonts) f.second->OnResetDevice();
    else g_fontsDirty = true;
    return hr;
}

// Hot-patch (mov edi,edi + 5 byte pad) when possible, else patch the shared vtable slot.
static bool Hook(void** vtable, int idx, void* hook, void** orig) {
    BYTE* fn = (BYTE*)vtable[idx];
    DWORD old;
    bool pad = true;
    for (int i = 1; i <= 5; i++) pad &= fn[-i] == 0x90 || fn[-i] == 0xCC;
    if (fn[0] == 0x8B && fn[1] == 0xFF && pad) {
        VirtualProtect(fn - 5, 7, PAGE_EXECUTE_READWRITE, &old);
        *orig = fn + 2;
        fn[-5] = 0xE9; *(int*)(fn - 4) = int((BYTE*)hook - fn);
        *(WORD*)fn = 0xF9EB;  // jmp short -5
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
    g_pbSplits = ReadPbSplits(g_set.category);
    g_session = FindSession();
    Log("session: %p", g_session);

    // Wait for the renderer and the game DLL, then hook and start polling.
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
// An open Bink handle means a pre-rendered cutscene is playing; IGT pauses meanwhile.
// ponytail: counts every Bink video, including menu backgrounds; fine because runs don't sit in menus.
static FARPROC BinkOrig(const char* name) {
    static HMODULE orig = LoadLibraryA("binkw32_orig.dll");
    return orig ? GetProcAddress(orig, name) : nullptr;
}

extern "C" void* __stdcall SrBinkOpen(const char* file, unsigned flags) {
    static auto fn = (void*(__stdcall*)(const char*, unsigned))BinkOrig("_BinkOpen@8");
    void* h = fn ? fn(file, flags) : nullptr;
    if (h) InterlockedIncrement(&g_binkOpen);
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

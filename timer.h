// Pure run/timer logic, no Windows or game dependencies (tested by timer_test.cpp).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

inline std::string FormatTime(double s, int decimals, bool hoursAlways) {
    if (s < 0) s = 0;
    if (decimals < 0) decimals = 0;
    if (decimals > 3) decimals = 3;
    static const int scale[] = {1, 10, 100, 1000};
    long long units = (long long)std::floor(s * scale[decimals]); // truncate, never round up
    long long whole = units / scale[decimals];
    int frac = (int)(units % scale[decimals]);
    int h = (int)(whole / 3600), m = (int)(whole / 60 % 60), sec = (int)(whole % 60);
    char buf[48];
    int n = (h || hoursAlways) ? snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, sec)
                               : snprintf(buf, sizeof buf, "%d:%02d", m, sec);
    if (decimals) snprintf(buf + n, sizeof buf - n, ".%0*d", decimals, frac);
    return buf;
}

// Signed delta, e.g. "-0:01.20" / "+0:03.00".
inline std::string FormatDelta(double d, int decimals) {
    return (d < 0 ? "-" : "+") + FormatTime(std::fabs(d), decimals, false);
}

struct Run {
    enum State { Idle, Running, Finished } state = Idle;
    double rta = 0, igt = 0;           // whole run
    double mapRta = 0, mapIgt = 0;     // current map (always ticking, also outside a run)
    double loadTime = 0;               // time spent in loads during the run
    int loads = 0, deaths = 0;
    std::string map;                   // current map name
    bool mapClean = false;             // entered via level transition, no teleport -> PB eligible
    bool practice = false;             // teleport used during the run
    bool loading = false;
    bool alive = true;

    void Reset() { *this = Run{map}; }
    explicit Run(std::string m = "") : map(std::move(m)) {}
    void Start() { Reset(); state = Running; }
    void Finish() { if (state == Running) state = Finished; }

    void Tick(double dt) {
        mapRta += dt;
        if (!loading) mapIgt += dt;
        if (state != Running) return;
        rta += dt;
        if (loading) loadTime += dt; else igt += dt;
    }

    void SetLoading(bool now) {
        if (now && !loading && state == Running) loads++;
        loading = now;
    }

    // Called when a load finishes. Returns true (and the finished map's IGT via outIgt)
    // when the previous map was completed cleanly and is PB-eligible.
    bool MapLoaded(const std::string& newMap, std::string* outMap, double* outIgt) {
        if (newMap == map) return false;           // reload / death / quickload on same map
        bool eligible = mapClean && !map.empty();
        if (eligible) { *outMap = map; *outIgt = mapIgt; }
        bool transition = !map.empty();            // first map after launch isn't a clean entry
        map = newMap;
        mapRta = mapIgt = 0;
        mapClean = transition;
        return eligible;
    }

    void Teleported() { practice = true; mapClean = false; }

    void SetHealth(float hp) {
        bool nowAlive = hp > 0;
        if (alive && !nowAlive) deaths++;
        alive = nowAlive;
    }
};

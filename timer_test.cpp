#include "timer.h"
#include <cassert>
#include <cstdio>

int main() {
    assert(FormatTime(83.456, 2, false) == "1:23.45");
    assert(FormatTime(83.456, 0, false) == "1:23");
    assert(FormatTime(83.456, 3, false) == "1:23.456");
    assert(FormatTime(3725.5, 1, false) == "1:02:05.5");
    assert(FormatTime(5, 1, true) == "0:00:05.0");
    assert(FormatDelta(-1.2, 2) == "-0:01.20");

    Run r;
    std::string m; double t;
    assert(!r.MapLoaded("menu", &m, &t));       // first map: not clean
    r.Start();
    r.Tick(1);
    r.SetLoading(true); r.Tick(2);              // load: IGT stops, RTA runs
    assert(!r.MapLoaded("trainyard", &m, &t));  // menu was not clean
    r.SetLoading(false); r.Tick(3);
    assert(r.rta == 6 && r.igt == 4 && r.loadTime == 2 && r.loads == 1);
    assert(r.mapIgt == 3 && r.mapRta == 3);

    r.SetHealth(100); r.SetHealth(0); r.SetHealth(0); r.SetHealth(100);
    assert(r.deaths == 1);
    assert(!r.MapLoaded("trainyard", &m, &t));  // quickload same map keeps map timer
    assert(r.mapIgt == 3);

    assert(r.MapLoaded("church", &m, &t) && m == "trainyard" && t == 3);
    assert(r.mapIgt == 0 && r.mapClean);
    r.Teleported();
    assert(!r.MapLoaded("farm", &m, &t) && r.practice);

    // Splits: trainyard -> church -> farm, plus final split on Finish.
    assert(r.splits.size() == 2 && r.splits[0].map == "trainyard" && r.splits[0].time == 4);
    assert(r.splits[1].map == "church" && r.splits[1].seg == 0);

    // Cutscene / mission complete: IGT stops, RTA runs, no load counted.
    r.paused = true; r.Tick(2); r.paused = false;
    assert(r.rta == 8 && r.igt == 4 && r.loads == 1 && r.mapIgt == 0);
    r.Tick(1);

    // Mission complete screen splits once; the next map load doesn't split again.
    Run a; a.MapLoaded("menu", &m, &t); a.MapLoaded("trainyard", &m, &t); a.Start(); a.mapClean = true;
    a.Tick(3); a.MissionComplete(); a.MissionComplete();
    assert(a.splits.size() == 1 && a.splits[0].time == 3);
    a.paused = true; a.Tick(4); a.paused = false;          // screen shown: IGT stopped
    assert(a.MapLoaded("farm", &m, &t) && m == "trainyard");
    assert(a.splits.size() == 1 && !a.splitDone);
    a.Tick(2);
    assert(!a.MapLoaded("menu", &m, &t) && a.map == "farm"); // quitting to menu: no split, map kept
    assert(!a.MapLoaded("farm", &m, &t) && a.mapIgt == 2);   // continuing = reload, timer kept
    assert(a.splits.size() == 1);

    // Warp Trick simulation:
    // Mid-run replaying a mission (e.g. trainyard) and immediately returning
    // MUST NOT reset total run RTA or IGT!
    Run w; w.MapLoaded("menu", &m, &t); w.MapLoaded("trainyard", &m, &t); w.Start();
    w.Tick(10); // 10s of gameplay on trainyard
    w.MapLoaded("downtown", &m, &t);
    w.Tick(20); // 20s of gameplay on downtown (total IGT = 30)
    assert(w.igt == 30 && w.state == Run::Running);
    // Runner does warp trick: enters trainyard replay
    w.SetLoading(true); w.Tick(1); w.SetLoading(false);
    w.MapLoaded("trainyard", &m, &t);
    w.Tick(2); // 2 seconds in trainyard before aborting
    // Runner aborts warp trick: loads back to downtown end
    w.SetLoading(true); w.Tick(1); w.SetLoading(false);
    w.MapLoaded("downtown", &m, &t);
    w.Tick(5);
    // Total run IGT must be 30 + 2 + 5 = 37, RTA must be 30 + 1 + 2 + 1 + 5 = 39
    assert(w.state == Run::Running);
    assert(w.igt == 37);
    assert(w.rta == 39);

    r.Finish(); r.Tick(5);
    assert(r.rta == 9 && r.state == Run::Finished);
    assert(r.splits.size() == 3 && r.splits[2].map == "farm" && r.splits[2].seg == 1 && r.splits[2].time == 5);
    puts("timer_test OK");
}

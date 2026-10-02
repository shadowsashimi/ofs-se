// Known-answer checks for dragging a selection as a group, see
// OFS-lib\Funscript\FunscriptGroupMove.h. Run through tools\groupmove-probe.ps1.

#include "Funscript/FunscriptGroupMove.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

static int failures = 0;

static void check(bool ok, const char* what) noexcept
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) failures += 1;
}

static FunscriptArray points(std::initializer_list<std::pair<float, int>> list) noexcept
{
    FunscriptArray out;
    for(auto& p : list) out.emplace(p.first, p.second);
    return out;
}

static bool near(float a, float b) noexcept { return std::abs(a - b) < 1e-4f; }

int main()
{
    using namespace FunscriptGroupMove;
    constexpr float gap = 0.01f;

    {
        auto others = points({ { 0.f, 0 }, { 10.f, 0 } });
        auto group = points({ { 2.f, 20 }, { 3.f, 80 } });
        auto off = Clamp(others, group, 1.f, 10, gap);
        check(near(off.time, 1.f) && off.pos == 10, "a move with nothing in the way is applied as asked");
    }
    {
        auto others = points({ { 0.f, 0 }, { 5.f, 0 } });
        auto group = points({ { 2.f, 20 }, { 3.f, 80 } });
        auto off = Clamp(others, group, 4.f, 0, gap);
        check(near(off.time, 1.99f), "the last point stops one gap short of the next point");
    }
    {
        auto others = points({ { 1.f, 0 }, { 9.f, 0 } });
        auto group = points({ { 2.f, 20 }, { 3.f, 80 } });
        auto off = Clamp(others, group, -4.f, 0, gap);
        check(near(off.time, -0.99f), "the first point stops one gap after the previous point");
    }
    {
        auto others = points({ { 9.f, 0 } });
        auto group = points({ { 0.5f, 20 }, { 3.f, 80 } });
        auto off = Clamp(others, group, -2.f, 0, gap);
        check(near(off.time, -0.5f), "nothing is moved before zero seconds");
    }
    {
        auto others = points({ { 0.f, 0 }, { 2.5f, 50 }, { 10.f, 0 } });
        auto group = points({ { 2.f, 20 }, { 3.f, 80 } });
        auto forward = Clamp(others, group, 1.f, 0, gap);
        auto back = Clamp(others, group, -1.f, 0, gap);
        check(near(forward.time, 0.49f) && near(back.time, -0.49f),
            "an unselected point between selected ones cannot be jumped");
    }
    {
        auto group = points({ { 1.f, 20 }, { 2.f, 90 } });
        auto up = Clamp(FunscriptArray(), group, 0.f, 30, gap);
        auto down = Clamp(FunscriptArray(), group, 0.f, -30, gap);
        check(up.pos == 10 && down.pos == -20, "position stops at the edge instead of squashing the group");
    }
    {
        auto others = points({ { 0.f, 0 }, { 2.005f, 0 } });
        auto group = points({ { 2.f, 20 } });
        auto still = Clamp(others, group, 0.f, 0, gap);
        auto pushed = Clamp(others, group, 1.f, 0, gap);
        check(near(still.time, 0.f) && near(pushed.time, 0.f),
            "points already closer than the gap may stay put but not close in");
    }
    {
        auto group = points({ { 1.f, 20 }, { 2.f, 90 } });
        auto moved = Apply(group, Offset{ 0.5f, 5 });
        check(moved.size() == 2
            && near(moved[0].atS, 1.5f) && moved[0].pos == 25
            && near(moved[1].atS, 2.5f) && moved[1].pos == 95,
            "applying an offset shifts every point by it");
    }
    {
        auto group = points({ { 1.f, 20 } });
        auto off = Clamp(FunscriptArray(), FunscriptArray(), 3.f, 3, gap);
        check(near(off.time, 0.f) && off.pos == 0 && group.size() == 1, "an empty group does not move");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}

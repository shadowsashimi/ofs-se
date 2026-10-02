#include "OFS_ScriptSimulator.h"
#include "OFS_VulvaHeightMap.h"

#include <algorithm>
#include <cmath>
#include "OFS_ImGui.h"

#include "OFS_EventSystem.h"
#include "Funscript.h"
#include "OFS_DynamicFontAtlas.h"

#include "imgui.h"
#include "stb_sprintf.h"

#include "state/SimulatorState.h"
#include "OpenFunscripter.h"

inline static float Distance(const ImVec2& p1, const ImVec2& p2) noexcept
{
    ImVec2 diff = p1 - p2;
    return SDL_sqrtf(diff.x * diff.x + diff.y * diff.y);
}

inline static ImVec2 Normalize(const ImVec2& p) noexcept
{
    auto mag = Distance(ImVec2(0.f, 0.f), p);
    return ImVec2(p.x / mag, p.y / mag);
}

inline static uint32_t GetColor(const ImColor& col, float opacity) noexcept
{
    auto color = ImGui::ColorConvertFloat4ToU32(col);
    ((uint8_t*)&color)[IM_COL32_A_SHIFT / 8] = ((uint8_t)(255 * col.Value.w * opacity));
    return color;
}

void ScriptSimulator::Init() noexcept
{
    stateHandle = OFS_ProjectState<SimulatorState>::Register(SimulatorState::StateName);
    EV::Queue().appendListener(SDL_MOUSEMOTION,
        OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptSimulator::MouseMovement)));
}

inline static float CalcBearing(const ImVec2 p1, const ImVec2 p2) noexcept 
{
    float theta = SDL_atan2f(p2.x - p1.x, p1.y - p2.y);
    if (theta < 0.0)
        theta += M_PI*2.f;
    return theta;
}

void ScriptSimulator::MouseMovement(const OFS_SDL_Event* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& state = SimulatorState::State(stateHandle);
    auto& motion = ev->sdl.motion;
    const auto& simP1 = state.P1;
    const auto& simP2 = state.P2;

    if (std::abs(simP1.x - simP2.x) > std::abs(simP1.y - simP2.y)) {
        // horizontal
        auto [top_x, bottom_x] = std::minmax(simP1.x, simP2.x);
        mouseValue = motion.x - top_x;
        mouseValue /= (bottom_x - top_x);
    }
    else {
        // vertical
        auto [top_y, bottom_y] = std::minmax(simP1.y, simP2.y);
        mouseValue = motion.y - bottom_y;
        mouseValue /= top_y - bottom_y;
    }
    auto clamped = Util::Clamp(mouseValue, 0.f, 1.f);

    // Create a axis aligned rectangle with the size of the simulator + padding
    constexpr float areaPadding = 10.f;
    float simLength = Distance(simP1, simP2);
    auto simPos = (simP1 + simP2) / 2.f;
    ImRect areaRect;
    areaRect.Min = simPos - ImVec2(areaPadding, areaPadding);
    areaRect.Max = simPos + ImVec2(state.Width, simLength) + ImVec2(areaPadding, areaPadding);
    areaRect.Min -= ImVec2(state.Width / 2.f, simLength / 2.f);
    areaRect.Max -= ImVec2(state.Width / 2.f, simLength / 2.f);

    // rotate mouse pos into the same direction as the simulator
    float theta = CalcBearing(simP1, simP2);
    ImVec2 mousePosOnSim = ImVec2(motion.x, motion.y) - simPos;
    mousePosOnSim = simPos + ImRotate(mousePosOnSim, -SDL_cosf(theta), SDL_sinf(theta));
    // check if mousePos is on the simulator
    MouseOnSimulator = areaRect.Contains(mousePosOnSim);

    mouseValue = clamped;
    mouseValue = mouseValue * 2.f - 1.f;
}

void ScriptSimulator::CenterSimulator() noexcept
{
    auto& state = SimulatorState::State(stateHandle);
    const auto* viewport = ImGui::GetMainViewport();

    // Over the video when it is on screen, since that is what the simulator is
    // read against. The middle of the whole window put it across the timeline
    // in any window not tall enough to clear it.
    ImVec2 center = viewport->Size / 2.f;
    float room = viewport->Size.y;
    if (const ImGuiWindow* video = ImGui::FindWindowByName(OFS_VideoplayerWindow::WindowId)) {
        if (video->WasActive && video->Size.y > 0.f) {
            center = (video->Pos - viewport->Pos) + (video->Size / 2.f);
            room = video->Size.y;
        }
    }

    const float length = Util::Clamp(state.Width * 3.f, state.Width, std::max(state.Width, std::min(1000.f, room * 0.8f)));
    state.P1 = center - ImVec2(state.Width / 2.f, length / 2.f);
    state.P2 = state.P1 + ImVec2(0.f, length);
}

// Multi-axis scripts are named like "video.roll.funscript"; see
// Funscript::AxisName.
static std::string AxisLabel(const Funscript* script) noexcept
{
    if(script == nullptr) return std::string();
    return script->AxisName();
}

// Maps a channel name onto one of the six degrees of freedom. Returns -1 for
// channels the model does not represent (suck, or anything custom).
static int32_t AxisFromLabel(const std::string& label) noexcept
{
    if(label == "stroke") return 0;
    if(label == "surge") return 1;
    if(label == "sway") return 2;
    if(label == "twist") return 3;
    if(label == "roll") return 4;
    if(label == "pitch") return 5;
    return -1;
}

// ---------------------------------------------------------------------------
// Multi axis model
//
// A stroker is easier to read as one object moving in six degrees of freedom
// than as six separate bars. This is a small software renderer: build a
// cylinder, transform it by the live axis values, project it, shade it, then
// paint it back to front. Going through the ImGui draw list rather than GL
// avoids touching render state in the middle of the ImGui pass, which is the
// awkward part of how the waveform shader has to work.
// ---------------------------------------------------------------------------
namespace
{

struct Vec3
{
    float x = 0.f, y = 0.f, z = 0.f;
};

inline Vec3 add(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}

inline float dot(const Vec3& a, const Vec3& b) noexcept
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3 cross(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{ (a.y * b.z) - (a.z * b.y),
                 (a.z * b.x) - (a.x * b.z),
                 (a.x * b.y) - (a.y * b.x) };
}

inline Vec3 normalize(const Vec3& v) noexcept
{
    const float len = SDL_sqrtf(dot(v, v));
    if(len < 1e-6f) return Vec3{ 0.f, 0.f, 0.f };
    return Vec3{ v.x / len, v.y / len, v.z / len };
}

inline Vec3 rotateX(const Vec3& v, float angle) noexcept
{
    const float s = SDL_sinf(angle), co = SDL_cosf(angle);
    return Vec3{ v.x, v.y * co - v.z * s, v.y * s + v.z * co };
}

inline Vec3 rotateY(const Vec3& v, float angle) noexcept
{
    const float s = SDL_sinf(angle), co = SDL_cosf(angle);
    return Vec3{ v.x * co + v.z * s, v.y, -v.x * s + v.z * co };
}

inline Vec3 rotateZ(const Vec3& v, float angle) noexcept
{
    const float s = SDL_sinf(angle), co = SDL_cosf(angle);
    return Vec3{ v.x * co - v.y * s, v.x * s + v.y * co, v.z };
}

// Weak perspective. The camera sits at -FocalLength looking down +z, so a
// larger z is further away.
constexpr float FocalLength = 6.f;

inline ImVec2 project(const Vec3& p, ImVec2 center, float scale) noexcept
{
    const float w = FocalLength / Util::Max(0.1f, FocalLength + p.z);
    return ImVec2(center.x + (p.x * w * scale), center.y - (p.y * w * scale));
}

// Maps 0..100 to -1..1 with 50 as neutral.
inline float centered(float value) noexcept
{
    return (value - 50.f) / 50.f;
}

constexpr int32_t Segments = 48;

// The body is a lathe: a radius profile revolved around the y axis, with an
// optional squash applied to one of those axes. Shaping the silhouette here
// rather than in the draw code keeps the renderer generic.
//
// The profile is closed. It starts at the centre of the domed top, runs down
// the outside through the ribbed cap, over the protruding sleeve, and folds
// back up inside the bore to finish at the centre of the bore ceiling. Folding
// back on itself makes the surface watertight, so there are no caps to fill,
// and where a run goes upward the slope flips the normal inward on its own,
// which is what the inside of the bore needs.
//
// y runs +1 (top) to -1 (bottom) scaled by the body half length, r is a
// fraction of the widest radius. sx squashes the x axis of that ring, which is
// what turns the round bore into a slit. ao darkens recessed geometry the
// lighting model cannot know about, seam marks the smooth walls that carry the
// twist stripe, and sleeve switches the band to the soft pale material rather
// than the hard case colour.
struct ProfileRing
{
    float y;
    float r;
    float sx;
    float ao;
    bool seam;
    bool sleeve;
};

// Narrow enough to read as a slit rather than an oval.
constexpr float SlitSquash = 0.24f;

constexpr ProfileRing Profile[] = {
    // ---- case ----
    // Taken from a side photograph of the familiar torch-style case, measured
    // along its length: a rounded closed end, a long slim grip, an S-curve
    // out to the threaded collar, four raised threads, and the wide end cap.
    // The sleeve presents through the cap as a full dome.
    { 1.000f, 0.000f, 1.f, 1.00f, false, false },  // closed end, centre
    { 0.998f, 0.350f, 1.f, 1.00f, false, false },
    { 0.993f, 0.550f, 1.f, 1.00f, false, false },
    { 0.982f, 0.660f, 1.f, 1.00f, false, false },
    { 0.965f, 0.715f, 1.f, 1.00f, false, false },
    { 0.940f, 0.738f, 1.f, 1.00f, false, false },  // end of the rounding
    // The grip, filling out very slightly towards the collar. Several rings
    // rather than one tall band: quads are depth sorted on their average z.
    { 0.700f, 0.742f, 1.f, 1.00f, false, false },
    { 0.400f, 0.746f, 1.f, 1.00f, false, false },
    { 0.100f, 0.752f, 1.f, 1.00f, false, false },
    { -0.100f, 0.760f, 1.f, 1.00f, false, false },
    // The S-curve out to the collar.
    { -0.160f, 0.772f, 1.f, 1.00f, false, false },
    { -0.220f, 0.800f, 1.f, 0.99f, false, false },
    { -0.280f, 0.845f, 1.f, 0.99f, false, false },
    { -0.340f, 0.895f, 1.f, 0.99f, false, false },
    { -0.395f, 0.930f, 1.f, 1.00f, false, false },
    { -0.440f, 0.945f, 1.f, 1.00f, false, false },
    // Four thin raised threads.
    { -0.465f, 0.945f, 1.f, 0.96f, false, false },
    { -0.473f, 0.985f, 1.f, 1.00f, false, false },
    { -0.487f, 0.985f, 1.f, 1.00f, false, false },
    { -0.495f, 0.945f, 1.f, 0.96f, false, false },
    { -0.540f, 0.945f, 1.f, 0.96f, false, false },
    { -0.548f, 0.985f, 1.f, 1.00f, false, false },
    { -0.562f, 0.985f, 1.f, 1.00f, false, false },
    { -0.570f, 0.945f, 1.f, 0.96f, false, false },
    { -0.615f, 0.945f, 1.f, 0.96f, false, false },
    { -0.623f, 0.985f, 1.f, 1.00f, false, false },
    { -0.637f, 0.985f, 1.f, 1.00f, false, false },
    { -0.645f, 0.945f, 1.f, 0.96f, false, false },
    { -0.690f, 0.945f, 1.f, 0.96f, false, false },
    { -0.698f, 0.985f, 1.f, 1.00f, false, false },
    { -0.712f, 0.985f, 1.f, 1.00f, false, false },
    { -0.720f, 0.945f, 1.f, 0.96f, false, false },
    // The end cap: a flange with softened edges, its rim just outside the
    // sleeve.
    { -0.735f, 0.945f, 1.f, 0.96f, false, false },
    { -0.742f, 0.990f, 1.f, 1.00f, false, false },
    { -0.752f, 1.000f, 1.f, 1.00f, false, false },
    { -0.805f, 1.000f, 1.f, 1.00f, false, false },
    { -0.816f, 0.990f, 1.f, 1.00f, false, false },
    { -0.823f, 0.970f, 1.f, 0.98f, false, false },
    { -0.826f, 0.950f, 1.f, 0.95f, false, false },  // rim of the cap

    // ---- sleeve ----
    // A closed form of its own: down the outside from a domed top, out through
    // the cap, over the face and around a rolled lip, then back up the canal.
    // Slim under the grip and full under the collar, as the case is. The
    // closed end reaches up under the top of the case, leaving the room it
    // needs to lift at full insertion (TipSwell), so the bulb can use it.
    { 0.905f, 0.000f, 1.f, 0.86f, false, true },  // top, centre
    { 0.890f, 0.420f, 1.f, 0.90f, false, true },  // domed top
    { 0.860f, 0.600f, 1.f, 0.94f, false, true },
    { 0.800f, 0.640f, 1.f, 0.96f, true, true },
    { 0.450f, 0.645f, 1.f, 0.98f, true, true },
    { 0.100f, 0.650f, 1.f, 0.99f, true, true },
    { -0.100f, 0.655f, 1.f, 1.00f, true, true },
    { -0.200f, 0.690f, 1.f, 1.00f, true, true },
    { -0.280f, 0.760f, 1.f, 1.00f, true, true },
    { -0.340f, 0.820f, 1.f, 1.00f, true, true },
    { -0.400f, 0.880f, 1.f, 1.00f, true, true },
    { -0.600f, 0.900f, 1.f, 1.00f, true, true },
    { -0.740f, 0.905f, 1.f, 1.00f, false, true },
    { -0.790f, 0.930f, 1.f, 1.00f, false, true },
    { -0.826f, 0.935f, 1.f, 1.00f, false, true },  // emerging from the cap
    // The face: a full dome standing out of the cap, sampled finely because
    // the labia are carved into it per vertex (see vulvaHeight).
    { -0.850f, 0.925f, 1.f, 0.98f, false, true },
    { -0.867f, 0.905f, 1.f, 0.98f, false, true },
    { -0.886f, 0.870f, 1.f, 0.97f, false, true },
    { -0.904f, 0.825f, 1.f, 0.97f, false, true },
    { -0.920f, 0.770f, 1.f, 0.96f, false, true },
    { -0.933f, 0.710f, 1.f, 0.96f, false, true },
    { -0.945f, 0.650f, 1.f, 0.96f, false, true },
    { -0.954f, 0.590f, 1.f, 0.95f, false, true },
    { -0.962f, 0.530f, 1.f, 0.95f, false, true },
    { -0.968f, 0.475f, 1.f, 0.94f, false, true },  // presenting face

    // The lip. It rolls slightly proud of the face and squashes as it goes, so
    // the vulva reads as a pair of lips around a slit rather than as a hole
    // punched in a flat surface.
    { -0.994f, 0.430f, 0.20f, 0.80f, false, true }, // rolling out
    { -0.998f, 0.320f, 0.13f, 0.50f, false, true }, // crest, proud of the face
    { -0.988f, 0.278f, 0.16f, 0.30f, false, true }, // turning inward

    // The canal, running most of the length of the sleeve. Split along its run
    // for the same depth sorting reason as the case body.
    // The canal. Sampled finely through the span the rod tip actually
    // sweeps, -1.04 to 0.33 here, because the wall is deformed per ring: at
    // the old spacing the bulge travelling up the canal moved in a handful
    // of visible steps rather than sliding. Coarse past that, where the rod
    // never reaches and only the taper matters.
    { -0.9720f, 0.4400f, SlitSquash, 0.260f, false, true },
    { -0.9560f, 0.4396f, SlitSquash, 0.254f, false, true },
    { -0.9400f, 0.4393f, SlitSquash, 0.247f, false, true },
    { -0.9240f, 0.4389f, SlitSquash, 0.241f, false, true },
    { -0.9080f, 0.4385f, SlitSquash, 0.235f, false, true },
    { -0.8920f, 0.4382f, SlitSquash, 0.229f, false, true },
    { -0.8760f, 0.4378f, SlitSquash, 0.224f, false, true },
    { -0.8600f, 0.4375f, SlitSquash, 0.219f, false, true },
    { -0.8440f, 0.4371f, SlitSquash, 0.214f, false, true },
    { -0.8280f, 0.4367f, SlitSquash, 0.209f, false, true },
    { -0.8120f, 0.4364f, SlitSquash, 0.205f, false, true },
    { -0.7960f, 0.4360f, SlitSquash, 0.202f, false, true },
    { -0.7800f, 0.4356f, SlitSquash, 0.198f, false, true },
    { -0.7640f, 0.4353f, SlitSquash, 0.195f, false, true },
    { -0.7480f, 0.4349f, SlitSquash, 0.191f, false, true },
    { -0.7320f, 0.4345f, SlitSquash, 0.188f, false, true },
    { -0.7160f, 0.4342f, SlitSquash, 0.184f, false, true },
    { -0.7000f, 0.4338f, SlitSquash, 0.181f, false, true },
    { -0.6840f, 0.4335f, SlitSquash, 0.179f, false, true },
    { -0.6680f, 0.4331f, SlitSquash, 0.176f, false, true },
    { -0.6520f, 0.4327f, SlitSquash, 0.174f, false, true },
    { -0.6360f, 0.4324f, SlitSquash, 0.171f, false, true },
    { -0.6200f, 0.4320f, SlitSquash, 0.169f, false, true },
    { -0.6040f, 0.4316f, SlitSquash, 0.167f, false, true },
    { -0.5880f, 0.4313f, SlitSquash, 0.165f, false, true },
    { -0.5720f, 0.4309f, SlitSquash, 0.164f, false, true },
    { -0.5560f, 0.4306f, SlitSquash, 0.162f, false, true },
    { -0.5400f, 0.4302f, SlitSquash, 0.160f, false, true },
    { -0.5240f, 0.4298f, SlitSquash, 0.158f, false, true },
    { -0.5080f, 0.4294f, SlitSquash, 0.157f, false, true },
    { -0.4920f, 0.4291f, SlitSquash, 0.155f, false, true },
    { -0.4760f, 0.4287f, SlitSquash, 0.154f, false, true },
    { -0.4600f, 0.4284f, SlitSquash, 0.153f, false, true },
    { -0.4440f, 0.4281f, SlitSquash, 0.151f, false, true },
    { -0.4280f, 0.4277f, SlitSquash, 0.150f, false, true },
    { -0.4120f, 0.4273f, SlitSquash, 0.149f, false, true },
    { -0.3960f, 0.4269f, SlitSquash, 0.148f, false, true },
    { -0.3800f, 0.4265f, SlitSquash, 0.147f, false, true },
    { -0.3640f, 0.4262f, SlitSquash, 0.146f, false, true },
    { -0.3480f, 0.4259f, SlitSquash, 0.145f, false, true },
    { -0.3320f, 0.4255f, SlitSquash, 0.145f, false, true },
    { -0.3160f, 0.4251f, SlitSquash, 0.144f, false, true },
    { -0.3000f, 0.4247f, SlitSquash, 0.143f, false, true },
    { -0.2840f, 0.4244f, SlitSquash, 0.142f, false, true },
    { -0.2680f, 0.4240f, SlitSquash, 0.142f, false, true },
    { -0.2520f, 0.4236f, SlitSquash, 0.141f, false, true },
    { -0.2360f, 0.4233f, SlitSquash, 0.141f, false, true },
    { -0.2200f, 0.4229f, SlitSquash, 0.141f, false, true },
    { -0.2040f, 0.4226f, SlitSquash, 0.140f, false, true },
    { -0.1880f, 0.4222f, SlitSquash, 0.140f, false, true },
    { -0.1720f, 0.4218f, SlitSquash, 0.139f, false, true },
    { -0.1560f, 0.4214f, SlitSquash, 0.139f, false, true },
    { -0.1400f, 0.4211f, SlitSquash, 0.138f, false, true },
    { -0.1240f, 0.4207f, SlitSquash, 0.138f, false, true },
    { -0.1080f, 0.4204f, SlitSquash, 0.137f, false, true },
    { -0.0920f, 0.4201f, SlitSquash, 0.137f, false, true },
    { -0.0760f, 0.4197f, SlitSquash, 0.136f, false, true },
    { -0.0600f, 0.4193f, SlitSquash, 0.136f, false, true },
    { -0.0440f, 0.4189f, SlitSquash, 0.136f, false, true },
    { -0.0280f, 0.4185f, SlitSquash, 0.136f, false, true },
    { -0.0120f, 0.4182f, SlitSquash, 0.135f, false, true },
    { 0.0040f, 0.4178f, SlitSquash, 0.135f, false, true },
    { 0.0200f, 0.4175f, SlitSquash, 0.134f, false, true },
    { 0.0360f, 0.4172f, SlitSquash, 0.134f, false, true },
    { 0.0520f, 0.4168f, SlitSquash, 0.134f, false, true },
    { 0.0680f, 0.4164f, SlitSquash, 0.134f, false, true },
    { 0.0840f, 0.4160f, SlitSquash, 0.134f, false, true },
    { 0.1000f, 0.4156f, SlitSquash, 0.134f, false, true },
    { 0.1160f, 0.4153f, SlitSquash, 0.133f, false, true },
    { 0.1320f, 0.4150f, SlitSquash, 0.133f, false, true },
    { 0.1480f, 0.4146f, SlitSquash, 0.133f, false, true },
    { 0.1640f, 0.4142f, SlitSquash, 0.133f, false, true },
    { 0.1800f, 0.4138f, SlitSquash, 0.133f, false, true },
    { 0.1960f, 0.4134f, SlitSquash, 0.133f, false, true },
    { 0.2120f, 0.4131f, SlitSquash, 0.132f, false, true },
    { 0.2280f, 0.4128f, SlitSquash, 0.132f, false, true },
    { 0.2440f, 0.4124f, SlitSquash, 0.132f, false, true },
    { 0.2600f, 0.4121f, SlitSquash, 0.132f, false, true },
    { 0.2760f, 0.4117f, SlitSquash, 0.132f, false, true },
    { 0.2920f, 0.4113f, SlitSquash, 0.132f, false, true },
    { 0.3080f, 0.4109f, SlitSquash, 0.132f, false, true },
    // Past the canal: the vault at its top, the cervix hanging low into it,
    // and the round uterus behind, where what the easter egg lets out collects.
    // The cervix hangs low enough that the rod bottomed out squashes it (see
    // CervixTipY). No ribs or flutes in any of it (see ringRibMask).
    //
    // The vault: close about the cervix all the way up, a thin gap between
    // them, closing into a roof where the cervix hangs from it.
    { 0.3220f, 0.4500f, SlitSquash, 0.128f, false, true },
    { 0.3290f, 0.5200f, SlitSquash, 0.126f, false, true },
    { 0.3370f, 0.6600f, SlitSquash, 0.124f, false, true },
    { 0.3480f, 0.8600f, SlitSquash, 0.122f, false, true },
    { 0.3610f, 1.0400f, SlitSquash, 0.120f, false, true },
    { 0.3760f, 1.2200f, SlitSquash, 0.118f, false, true },
    { 0.3930f, 1.3400f, SlitSquash, 0.116f, false, true },
    { 0.4060f, 1.4000f, SlitSquash, 0.112f, false, true },
    { 0.4150f, 1.3600f, SlitSquash, 0.110f, false, true },
    { 0.4180f, 1.3100f, SlitSquash, 0.110f, false, true },  // its roof, rounding over onto the cervix
    // The cervix: a dome hanging from the roof, widest where it meets it and
    // rounding down to a flattened bottom, narrowest where the rod meets it,
    // with its opening in the middle of that flat.
    { 0.4120f, 1.2700f, SlitSquash, 0.108f, false, true },  // widest, at the roof
    { 0.3920f, 1.2200f, SlitSquash, 0.106f, false, true },
    { 0.3740f, 1.1000f, SlitSquash, 0.104f, false, true },
    { 0.3600f, 0.9200f, SlitSquash, 0.102f, false, true },
    { 0.3480f, 0.7200f, SlitSquash, 0.100f, false, true },
    { 0.3380f, 0.5500f, SlitSquash, 0.098f, false, true },
    { 0.3310f, 0.4200f, SlitSquash, 0.096f, false, true },  // rounding under
    { 0.3270f, 0.3000f, SlitSquash, 0.094f, false, true },  // its flat bottom
    { 0.3255f, 0.1500f, SlitSquash, 0.092f, false, true },
    { 0.3250f, 0.0700f, SlitSquash, 0.090f, false, true },  // the os
    { 0.3310f, 0.0450f, SlitSquash, 0.088f, false, true },  // tightest
    // Up the cervical canal to where the uterus opens out.
    { 0.3600f, 0.0500f, SlitSquash, 0.090f, false, true },
    { 0.4000f, 0.0550f, SlitSquash, 0.092f, false, true },
    { 0.4400f, 0.0600f, SlitSquash, 0.094f, false, true },
    { 0.4650f, 0.1000f, SlitSquash, 0.096f, false, true },  // the uterus opens out
    // The uterus, empty (see UterusRest and UterusFull).
    { 0.4700f, 0.1200f, SlitSquash, 0.098f, false, true },
    { 0.4890f, 0.1291f, SlitSquash, 0.100f, false, true },
    { 0.5080f, 0.1620f, SlitSquash, 0.102f, false, true },
    { 0.5308f, 0.2382f, SlitSquash, 0.104f, false, true },
    { 0.5555f, 0.3703f, SlitSquash, 0.106f, false, true },
    { 0.5802f, 0.5574f, SlitSquash, 0.108f, false, true },
    { 0.6030f, 0.7816f, SlitSquash, 0.110f, false, true },
    { 0.6220f, 1.0075f, SlitSquash, 0.112f, false, true },
    { 0.6372f, 1.2145f, SlitSquash, 0.114f, false, true },
    { 0.6505f, 1.4153f, SlitSquash, 0.116f, false, true },
    { 0.6660f, 1.5600f, SlitSquash, 0.118f, false, true },
    { 0.6730f, 1.5400f, SlitSquash, 0.120f, false, true },
    { 0.6700f, 1.3600f, SlitSquash, 0.122f, false, true },
    { 0.6570f, 1.0000f, SlitSquash, 0.124f, false, true },
    { 0.6420f, 0.5200f, SlitSquash, 0.126f, false, true },
    { 0.6340f, 0.0000f, SlitSquash, 0.124f, false, true },  // canal ceiling
};

// Index of the ring that forms the vulva of the sleeve. Everything from here
// to the last ring is the bore, which is what the rod travels through.
// Crest of the lip: the narrowest point of the vulva, and what the stroke
// range and the rod depth are both measured against.
constexpr int32_t SlitRing = 64;
constexpr int32_t RingCount = (int32_t)(sizeof(Profile) / sizeof(Profile[0]));
constexpr int32_t BandCount = RingCount - 1;
// Lip of the orifice, used to draw a hard edge around the opening.
constexpr int32_t OrificeRimRing = SlitRing;

// The uterus from empty to full, ring for ring from where it opens out, as
// height and radius in the profile's own units. Empty it is a triangle with
// curved sides, its top corners reaching up a little and its top drooping
// between them; full it is a balloon filling the room in the sleeve above the
// cervix. Its rings in the profile are the empty shape.
constexpr int32_t UterusShapeRings = 16;
constexpr float UterusRest[UterusShapeRings][2] = {
    { 0.4700f, 0.1200f },
    { 0.4890f, 0.1291f },
    { 0.5080f, 0.1620f },
    { 0.5308f, 0.2382f },
    { 0.5555f, 0.3703f },
    { 0.5802f, 0.5574f },
    { 0.6030f, 0.7816f },
    { 0.6220f, 1.0075f },
    { 0.6372f, 1.2145f },
    { 0.6505f, 1.4153f },
    { 0.6660f, 1.5600f },
    { 0.6730f, 1.5400f },
    { 0.6700f, 1.3600f },
    { 0.6570f, 1.0000f },
    { 0.6420f, 0.5200f },
    { 0.6340f, 0.0000f },
};
constexpr float UterusFull[UterusShapeRings][2] = {
    { 0.4700f, 0.1400f },
    { 0.4956f, 0.7182f },
    { 0.5107f, 1.0950f },
    { 0.5317f, 1.4284f },
    { 0.5578f, 1.7051f },
    { 0.5880f, 1.9142f },
    { 0.6211f, 2.0473f },
    { 0.6556f, 2.0993f },
    { 0.6904f, 2.0681f },
    { 0.7239f, 1.9548f },
    { 0.7549f, 1.7641f },
    { 0.7822f, 1.5033f },
    { 0.8046f, 1.1830f },
    { 0.8213f, 0.8157f },
    { 0.8315f, 0.4161f },
    { 0.8350f, 0.0000f },
};

// The finish of the easter egg: four strong pulses close together, then three
// that weaken while the gaps between them widen, as the contractions run down.
// Nine contractions. The first is dry, as it usually is, and each of the
// eight after it lets out a spurt. They come 0.3 s apart at first and each
// gap is 0.1 s longer than the last, so 0.6 s apart on average over the
// eight, running down to nearly a second by the end. The second and third
// spurts are the strongest.
constexpr float FinishPulseTimes[] = { 0.00f, 0.30f, 0.70f, 1.20f, 1.80f, 2.50f, 3.30f, 4.20f, 5.20f };
constexpr float FinishPulseStrength[] = { 0.90f, 0.85f, 1.00f, 0.95f, 0.85f, 0.70f, 0.55f, 0.40f, 0.28f };
constexpr int32_t FinishPulseCount = 9;
// Contractions before this one let nothing out.
constexpr int32_t FinishFirstSpurt = 1;

// The rod during a finish: how much the whole shaft has tightened. clock is
// seconds since the finish began, or negative.
struct RodThrob
{
    float swell = 0.f;
};

inline RodThrob rodThrobAt(float clock) noexcept
{
    RodThrob t;
    if(clock < 0.f) return t;
    for(int32_t p = 0; p < FinishPulseCount; p += 1) {
        const float e = clock - FinishPulseTimes[p];
        if(e < 0.f) break;
        // The whole shaft tightens at each contraction and lets go over half
        // a second.
        t.swell += FinishPulseStrength[p] * SDL_expf(-e * 5.f);
    }
    return t;
}

// How much thicker the rod is along the shaft: the general tightening, the
// same everywhere. A swell used to run up the shaft with each pulse as well,
// and read as a ring travelling along it.
inline float rodThrobAlong(const RodThrob& t, float along) noexcept
{
    (void)along;
    return 1.f + (0.06f * Util::Min(1.5f, t.swell));
}

}

void ScriptSimulator::drawMultiAxisModel(ImDrawList* drawList, const SimulatorState& state,
    const MultiAxisValues& axes, ImVec2 center, float scale) noexcept
{
    OFS_PROFILE(__FUNCTION__);

    constexpr float Radius = 0.30f;
    constexpr float HalfLength = 0.88f;
    // How far the body travels along its own axis over the full stroke range.
    // Derived from the depth of the bore rather than set by hand: half the bore
    // means the rod sits at the vulva at one end of the stroke and touches the
    // far end of the sleeve at the other, so the full range of the script maps
    // onto the full range of the sleeve.
    // How deep the rod sits in the canal at the bottom of the stroke, which is
    // funscript position 0. Stated outright rather than falling out of the
    // travel, because it is the thing worth controlling.
    // The rod is fixed and the case slides over it, so the insertion range is
    // rigidly twice the travel and the two cannot be chosen independently.
    // This pairing puts the tip just clear of the vulva at position 100 and
    // buries roughly three quarters of the canal at position 0.
    constexpr float RodInsertDepth = 1.18f;
    // Half the stroke. Insertion range is exactly twice this, so the two cannot
    // be chosen independently: at the top of the stroke the rod ends up
    // (RodInsertDepth - 2 * StrokeTravel) deep, and this pairing leaves it just
    // barely engaged there while reaching well in at the bottom.
    constexpr float StrokeTravel = 0.60f;
    // Length of the visible shaft below the tip. Only needs to clear the vulva
    // of the case at the bottom of the stroke; longer than that just dangles
    // off the bottom of the window.
    // How far the lip travels past the top of the flare at the bottom of the
    // stroke. Seating it exactly on the top left the lip stopping level with a
    // cone it never touched, and a visible gap between the two.
    constexpr float LipSeatDepth = 0.070f;
    // The insertion depth less that overlap, so the two ends still line up by
    // construction: the tip sits at the vulva when the rod is all the way out,
    // and the lip is pressed onto the flare when it is all the way in.
    constexpr float RodLength = RodInsertDepth - LipSeatDepth;
    // How much of the body's lean the rod takes up by bending into line with
    // it. Held straight at its base and curving into the canal by the time it
    // reaches the vulva: without this it stays upright while the body tilts
    // away and simply passes out through the side of the case. The remainder,
    // 1 - RodFollow, is what the canal has to absorb by moving aside, which is
    // the deformation actually worth seeing.
    constexpr float RodFollow = 0.86f;
    // How much the vulva swells at full insertion, and how far back from the
    // opening that swelling reaches. The reach stops just short of the shell's
    // rim: past it the sleeve is inside the case with almost no room, so the
    // whole swell has to happen on the part that sticks out.
    // Only a little sideways while the lip is free. Material driven in has to
    // go somewhere, but out of the opening is the easy direction and swelling
    // against the shell is not.
    constexpr float VulvaBulge = 0.32f;
    // The material driven back by the rod bunches into stacked rings around
    // the shaft, the way a soft sleeve does: how tall they stand as a share
    // of the radius, and how far apart they are in world units.
    constexpr float FoldAmp = 0.12f;
    constexpr float FoldPeriod = 0.042f;
    constexpr float VulvaBulgeReach = 0.075f;
    // Which is where most of it goes: the lip pushes out of the case rather
    // than out to the side. Fades away as the lip comes down onto the foot,
    // because by then it is up against something and the only way left is
    // sideways -- which is what LipSplay then does.
    constexpr float VulvaExtend = 0.035f;
    constexpr float VulvaExtendReach = 0.20f;
    // The same displaced material at the other end. The closed end of the
    // sleeve is capped by the shell, so it rises into the space above it.
    // Seating the lip on the flared foot at the bottom of the stroke. Rubber
    // meeting a cone rolls outward rather than stopping flat against it, so the
    // last of the lip trumpets as it comes down onto the flare. Squared rather
    // than eased, so it is sharpest right at the end instead of swelling the
    // whole lip evenly.
    // The lips as a spring: the reach of the springy rings either side of the
    // crest in profile units, the stiffness, and the damping, which leaves them
    // underdamped enough to be seen settling after the rod has gone.
    constexpr float LipSpringZone = 0.16f;
    constexpr float LipSpringK = 2600.f;
    constexpr float LipSpringDamp = 85.f;
    // How far the outer lips stand proud of the face at rest, in world units,
    // and a wavy inner edge just inside them.
    // The face of the sleeve is drawn as a fine grid of its own over the
    // coarse rings, carved with a height map traced from a reference
    // (OFS_VulvaHeightMap.h): the face's radius the map spans, how far the
    // crest of the majora stands out in world units, and how many grid steps
    // each ring band and each segment is split into.
    constexpr float FaceRadius = 0.935f * Radius;
    constexpr float FaceRelief = 0.060f;
    constexpr int32_t FaceSubRadial = 3;
    constexpr int32_t FaceSubAround = 3;
    constexpr int32_t FaceBandsMax = 16;
    constexpr int32_t FaceRowsMax = (FaceBandsMax * FaceSubRadial) + 1;
    constexpr int32_t FaceColsMax = Segments * FaceSubAround;
    // The rod's base is pliable: as the lip comes down onto the flare the
    // flare gives, by this share of its radius, so the vulva is not stretched
    // out over the whole of it.
    constexpr float FootGive = 0.45f;
    // The most the lip opens to, as a multiple of the shaft: soft material has
    // a limit, and past it the base gives rather than the sleeve stretching.
    constexpr float MaxLipOpen = 1.6f;
    // Out past the cap the face bulges, but no wider than the cap itself, and
    // pressed onto the foot it keeps this much thickness over the lip.
    constexpr float VulvaMaxRadius = 0.96f * Radius;
    constexpr float VulvaFaceWall = 0.035f;
    constexpr float LipSplay = 0.06f;
    constexpr float LipSplayReach = 0.14f;
    // Where along the lip the roll sits, and how broad it is, as fractions of
    // that reach. Set back from the rim: rubber pressed against something rolls
    // into a bead behind the contact rather than coming to a peak at the very
    // edge, which is what made the end read sharp.
    constexpr float LipSplayPeak = 0.38f;
    constexpr float LipSplayWidth = 0.42f;
    constexpr float LipSeatRange = 0.22f;
    constexpr float TipSwell = 0.060f;
    constexpr float TipSwellReach = 0.42f;
    // How much of the opening the outer wall takes up. Without this the bore
    // simply eats into a wall of fixed thickness and eventually comes through
    // it; the sleeve is rubber, so it stretches as a whole. Small, because the
    // bore at rest is so much tighter than the rod that the opening is large:
    // any more and the wall is pinned against the shell for the rod's whole
    // length and stops varying along it.
    constexpr float SleeveStretch = 0.16f;
    // ...until it reaches the shell, which does not give at all. That limit is
    // what forces the rest of the displacement out of the vulva and up into
    // the cap.
    constexpr float ShellClearance = 0.010f;
    // The bore at rest, near enough constant down its length, used to work out
    // how far the rod has opened it.
    constexpr float CanalRestRadius = 0.42f * SlitSquash * Radius;
    // How far in from the lip the slit closes up into the bore. The opening is
    // a slit, but the passage behind it is not: running the slit's flattened
    // section all the way to the closed end left the whole canal oblong, which
    // reads as a rotating oval the moment the twist axis moves it. A bore is
    // symmetrical, so only the lip should turn visibly.
    //
    // It closes to the slit's narrow dimension, not its wide one. The bore at
    // rest is a good deal tighter than the rod, which is what makes the rod
    // visibly stretch it open; matching the wide dimension instead gave a
    // slack passage the rod barely touched.
    constexpr float SlitBlend = 0.25f;
    // How hard the lens pinches towards its ends. At one the lips run into the
    // corner dead straight and meet at about fifty degrees, which is a corner
    // by construction but blunt enough to read as a flat end. Above one they
    // curve in as they go, so the corner closes to a point.
    constexpr float SlitTipSharpness = 0.55f;
    // Ribs down the canal. Evaluated here rather than baked into the profile
    // because they move: the rod drags them along as it travels.
    constexpr float CanalRibPeriod = 0.078f;  // profile units
    // Depth of a rib as a fraction of the canal radius, at rest.
    constexpr float CanalRibAmp = 0.32f;
    // How far the ribs are pulled, in rib periods, and how quickly they creep
    // back once the rod stops moving.
    constexpr float RibDragGain = 3.0f;
    constexpr float RibDragLimit = 0.45f;
    constexpr float RibDragK = 70.f;
    constexpr float RibDragDamp = 3.f;
    // Flutes running down the bore. The ribs are turned rings, so they look
    // exactly the same however far the body has been twisted and the twist
    // axis moves nothing anyone can see. These vary around the bore instead,
    // and wind slowly along it, so rotating the body visibly turns them.
    // Kept well under what the tessellation can carry. The normal's lean
    // scales with the count, so at seven the wall swung nearly thirty degrees
    // between neighbouring facets -- only four and a half of them per flute --
    // and the bore came out blotchy rather than fluted. Segments / 8 is about
    // the limit.
    constexpr int32_t CanalFlutes = 4;
    constexpr float CanalFluteAmp = 0.16f;
    // What is left of a rib's depth once the rod is pressing on it. Rubber
    // being displaced squashes; it does not vanish.
    constexpr float RibSquish = 0.70f;
    // Negative: the canal closes a little inside the rod's surface rather
    // than standing off it. In the cutaway that reads as the sleeve gripping,
    // which is what it would actually be doing -- a wall standing off the rod
    // reads as a loose tube the rod happens to be passing through.
    constexpr float RodCanalGap = -0.08f;
    constexpr float RodRadius = 0.138f;
    // A flared foot, so the rod reads as mounted on something rather than
    // simply running off the bottom of the frame. Height above the foot
    // against a multiple of the shaft radius. Declared up here because the
    // sleeve needs it too: its lip is pressed onto this cone and has to know
    // the shape it is being pressed onto.
    // Wide at the foot, and tucked into the shaft over a short distance at the
    // top rather than tapering the whole way: a wide base with a tight fillet
    // rather than a broad soft cone.
    static constexpr float RodFootShape[][2] = {
        { 0.000f, 2.30f },
        { 0.120f, 2.26f },
        { 0.300f, 2.05f },
        { 0.480f, 1.78f },
        { 0.640f, 1.50f },
        { 0.760f, 1.26f },
        { 0.850f, 1.12f },
        { 0.920f, 1.045f },
        { 0.965f, 1.012f },
        { 1.000f, 1.000f },
    };
    constexpr int32_t RodFootSamples = (int32_t)(sizeof(RodFootShape) / sizeof(RodFootShape[0]));
    constexpr int32_t RodFootRings = RodFootSamples - 1;
    constexpr float RodFootHeight = RodRadius * 1.05f;
    // The lip rests on the cone rather than merging into it.
    constexpr float LipFootClearance = 0.010f;
    // Tilt limits, so a full swing stays readable instead of folding over.
    constexpr float MaxTiltRadians = 0.5236f;  // 30 degrees
    constexpr float LateralTravel = 0.45f;

    const float stroke = centered(axes.value[(int32_t)Axis::Stroke]);
    const float surge = centered(axes.value[(int32_t)Axis::Surge]);
    const float sway = centered(axes.value[(int32_t)Axis::Sway]);
    const float twist = centered(axes.value[(int32_t)Axis::Twist]);
    const float roll = centered(axes.value[(int32_t)Axis::Roll]);
    const float pitch = centered(axes.value[(int32_t)Axis::Pitch]);

    // Which way each axis goes is the TCode specification's, since that is
    // what the machine the model stands in for is obeying:
    //
    //   L0 stroke is positive up, L1 surge positive away from the user, and
    //   L2 sway positive to the user's LEFT. R0 twist, R1 roll and R2 pitch
    //   are positive by the right hand rule about L0, L1 and L2.
    //
    // The right hand rule about an axis pointing away from the user turns
    // clockwise as the user sees it, so roll at 100 leans the top to the
    // right; about the upright axis it turns counter clockwise seen from
    // above, so twist at 100 brings the front round to the user's right.
    //
    // The signs live here alone. Everything downstream derives from these,
    // unrotate and the fluid's idea of downhill included.
    const float twistAngle = -twist * (float)M_PI;
    const float rollAngle = -roll * MaxTiltRadians;
    const float pitchAngle = pitch * MaxTiltRadians;

    // Stroke moves the body along its own axis, so it is applied in model space
    // before the tilt. A tilted stroker then strokes along its tilt.
    const float strokeOffset = stroke * StrokeTravel;
    // Sway is positive to the user's left, which is -x on screen.
    const Vec3 worldOffset{ -sway * LateralTravel, 0.f, surge * LateralTravel };

    // Camera elevation. At zero the model is seen straight on, which reads
    // cleanest but leaves the orifice edge on and therefore invisible, since it
    // sits in a downward facing surface. Raising it tips the underside into
    // view at the cost of seeing less of the body. Folded into orient() so
    // normals get it too and the lighting stays consistent.
    const float CameraPitch = state.CameraElevation * ((float)M_PI / 180.f);
    const float CameraYaw = state.CameraYaw * ((float)M_PI / 180.f);
    // Turned around the upright axis first, then raised, so turning keeps the
    // model standing up on screen.
    auto camera = [&](const Vec3& p) noexcept {
        return rotateX(rotateY(p, CameraYaw), CameraPitch);
    };
    // Pitch and roll pivot at the vulva rather than at the middle of the body.
    // The vulva is where the rod enters, so swinging about it leaves that
    // contact point roughly where it was and tips the case around it. Pivoting
    // at the centre threw the opening through an arc, taking the apparent entry
    // point of the rod with it.
    const float vulvaY = Profile[SlitRing].y * HalfLength;
    const float pivotY = strokeOffset + vulvaY;

    // Rotation with no translation. Directions have no pivot, so normals and
    // the radial facing test use this directly.
    auto orient = [&](const Vec3& dir) noexcept {
        Vec3 p = rotateY(dir, twistAngle);
        p = rotateX(p, pitchAngle);
        p = rotateZ(p, rollAngle);
        return camera(p);
    };
    // Points swing about the pivot as well. Twist is about the body's own axis,
    // which passes through the pivot, so it is unaffected either way.
    auto toWorld = [&](const Vec3& local) noexcept {
        Vec3 p{ local.x, local.y - pivotY, local.z };
        p = rotateY(p, twistAngle);
        p = rotateX(p, pitchAngle);
        p = rotateZ(p, rollAngle);
        p.y += pivotY;
        p = add(p, worldOffset);
        return camera(p);
    };

    // The rod does not belong to the body. It is anchored at its own base and
    // stays upright while the sleeve tilts and slides over it, so it only ever
    // takes the camera.
    auto toView = [&](const Vec3& world) noexcept {
        return camera(world);
    };

    // The body's rotation without the camera, for reasoning about where things
    // are relative to each other rather than on screen.
    auto orientNoCamera = [&](const Vec3& dir) noexcept {
        Vec3 p = rotateY(dir, twistAngle);
        p = rotateX(p, pitchAngle);
        return rotateZ(p, rollAngle);
    };

    // Undoes the body's rotation, for putting the rod's upright axis into the
    // frame the profile is described in.
    auto unrotate = [&](const Vec3& v) noexcept {
        Vec3 p = rotateZ(v, -rollAngle);
        p = rotateX(p, -pitchAngle);
        return rotateY(p, -twistAngle);
    };

    // ---- shading -----------------------------------------------------------
    // Key light over the viewer's shoulder, plus a specular term and a rim
    // term. The rim is what stops a flat shaded body from reading as a
    // silhouette-less blob against a dark background.
    // Mostly overhead. With the key light close to the view direction the
    // top of the body ends up darker than its side walls, which makes the end
    // cap read as an opening rather than a surface.
    const Vec3 lightDir = normalize(Vec3{ -0.38f, 0.86f, -0.42f });
    // A dim fill from the opposite side. With a single key light the whole
    // right hand side of a body of revolution falls to ambient and the accent
    // colour stops reading as a colour at all.
    const Vec3 fillDir = normalize(Vec3{ 0.65f, -0.15f, -0.55f });
    const Vec3 viewDir{ 0.f, 0.f, -1.f };
    const Vec3 halfDir = normalize(add(lightDir, viewDir));

    const ImColor accent = state.Front;
    // The sleeve is the soft part, so it takes a pale wash of the accent rather
    // than a second colour of its own. Derived so it still tracks the theme.
    ImColor sleeveColor = accent;
    sleeveColor.Value.x += (1.f - accent.Value.x) * 0.30f;
    sleeveColor.Value.y += (1.f - accent.Value.y) * 0.14f;
    sleeveColor.Value.z += (1.f - accent.Value.z) * 0.20f;

    // The case is the monochrome part and the sleeve carries the accent. That
    // is how a real one looks, it keeps the pink as a highlight rather than
    // painting the whole object with it, and a dark shell is what gives the
    // x-ray its contrast: pink on pink reads as a wash.
    const ImColor caseColor = state.Border;

    // Cutaway removes the near half of the case rather than making it
    // translucent. Nothing is blended, so there is no sort ambiguity between
    // shell and sleeve at all, and the sleeve is seen at its own colour instead
    // of through a tint.
    const bool cutaway = state.CutawayCase;
    // Half angle of the window, as a dot product against the view.
    constexpr float CutawayCos = 0.36f;

    // soft: silicone rather than moulded plastic. Light wraps past the
    // terminator instead of stopping dead at it, which is what stands in for
    // subsurface scattering, the highlight is broad and weak rather than a
    // tight glint, and the edge glows because thin material lets light
    // through. Without this the sleeve reads as hard shiny plastic no matter
    // what colour it is.
    // Whole number powers, by multiplying rather than by calling powf. These
    // run per vertex of a model that builds tens of thousands of them a frame,
    // and powf was most of what a frame cost.
    auto squared = [](float x) noexcept { return x * x; };
    auto cubed = [](float x) noexcept { return x * x * x; };
    auto pow5 = [](float x) noexcept { const float x2 = x * x; return x2 * x2 * x; };
    auto pow6 = [](float x) noexcept { const float x2 = x * x; return x2 * x2 * x2; };
    auto pow10 = [](float x) noexcept {
        const float x2 = x * x;
        const float x4 = x2 * x2;
        return x4 * x4 * x2;
    };
    auto pow26 = [](float x) noexcept {
        const float x2 = x * x;
        const float x4 = x2 * x2;
        const float x8 = x4 * x4;
        const float x16 = x8 * x8;
        return x16 * x8 * x2;
    };

    auto shadeFace = [&](const Vec3& normal, float ao, bool seam,
                         const ImColor& base, float alpha, bool soft) noexcept {
        const float raw = dot(normal, lightDir);
        // Modest. Wrapping too far lights every face and the form flattens
        // out into a silhouette with no shading left in it.
        const float wrap = soft ? 0.30f : 0.f;
        const float diffuse = Util::Max(0.f, (raw + wrap) / (1.f + wrap));
        // Deliberately broad. Faces are flat shaded, because the draw list
        // only fills a polygon with a single colour, so a tight highlight is
        // constant across a whole band and steps at every ring boundary. That
        // reads as blocky dashes rather than as a highlight, and the
        // translucent case made it obvious by laying it over the sleeve.
        const float toLight = Util::Max(0.f, dot(normal, halfDir));
        const float specular = soft ? pow5(toLight) : pow10(toLight);
        // Fresnel style edge term: strongest where the surface turns away.
        // viewDir points from the surface toward the camera, so facing is the
        // positive dot, not its negation.
        const float facing = Util::Clamp(dot(normal, viewDir), 0.f, 1.f);
        const float rim = cubed(1.f - facing);

        const float fill = Util::Max(0.f, dot(normal, fillDir));
        const float ambient = soft ? 0.27f : 0.24f;
        const float lit = (ambient + ((soft ? 0.64f : 0.70f) * diffuse) + (0.20f * fill)) * ao;

        ImColor col;
        // The seam is a band of the body painted brighter. Without a marker a
        // body of revolution looks identical at every angle, so twist would be
        // invisible.
        const float seamBoost = seam ? 1.65f : 1.f;
        col.Value.x = base.Value.x * lit * seamBoost;
        col.Value.y = base.Value.y * lit * seamBoost;
        col.Value.z = base.Value.z * lit * seamBoost;

        // Rim pushes toward a lighter tint of the accent rather than white, so
        // the model keeps its hue at the edges.
        const float rimAmount = (soft ? 0.58f : 0.55f) * rim;
        col.Value.x += (1.f - col.Value.x) * rimAmount;
        col.Value.y += (1.f - col.Value.y) * rimAmount * (soft ? 0.52f : 0.75f);
        col.Value.z += (1.f - col.Value.z) * rimAmount * (soft ? 0.66f : 0.85f);

        // Specular is close to white for a glossy read.
        const float specAmount = (soft ? 0.13f : 0.20f) * specular;
        col.Value.x += (1.f - col.Value.x) * specAmount;
        col.Value.y += (1.f - col.Value.y) * specAmount;
        col.Value.z += (1.f - col.Value.z) * specAmount;

        col.Value.x = Util::Clamp(col.Value.x, 0.f, 1.f);
        col.Value.y = Util::Clamp(col.Value.y, 0.f, 1.f);
        col.Value.z = Util::Clamp(col.Value.z, 0.f, 1.f);
        col.Value.w = state.GlobalOpacity * alpha;
        return ImGui::ColorConvertFloat4ToU32(col);
    };

    // How thick the film is at one point of a coated surface, 0 to 1, given
    // the surface's coat there. A film is never even: it gathers into runs
    // down the surface with thinner wall between them, so the thickness
    // varies around the surface (col, a segment) far more than along it
    // (along, in rings), which is what makes the runs read as running down.
    auto filmAt = [&](float coat, int32_t col, float along) noexcept -> float {
        const float a = (2.f * (float)M_PI * (float)col) / (float)Segments;
        const float runs = 0.5f + (0.30f * SDL_sinf((3.f * a) + (0.20f * along)))
            + (0.20f * SDL_sinf((7.f * a) - (0.35f * along) + 1.7f));
        return Util::Clamp(coat * (0.45f + (0.85f * runs)), 0.f, 1.f);
    };

    // A film of fluid over a face. Wet, the surface goes a little darker and
    // richer, as a wet surface does; the film's own milky cast shows only
    // where it is thick; and it turns glossy, with a tight highlight where
    // the light catches it and a sheen along the edges where it turns away.
    auto wetten = [&](uint32_t base, float film, const Vec3& normal) noexcept -> uint32_t {
        constexpr float TintR = 0.97f, TintG = 0.94f, TintB = 0.84f;
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(base);
        const float k = Util::Clamp(film, 0.f, 1.f);
        const float dark = 1.f - (0.16f * k);
        c.x *= dark;
        c.y *= dark;
        c.z *= dark;
        const float tint = 0.62f * k * k;
        c.x += (TintR - c.x) * tint;
        c.y += (TintG - c.y) * tint;
        c.z += (TintB - c.z) * tint;
        const float toLight = Util::Max(0.f, dot(normal, halfDir));
        const float spec = pow26(toLight);
        const float broad = pow6(toLight);
        const float facing = Util::Clamp(dot(normal, viewDir), 0.f, 1.f);
        const float rim = squared(1.f - facing);
        const float gloss = k * ((0.85f * spec) + (0.18f * broad) + (0.22f * rim));
        c.x = Util::Clamp(c.x + ((1.f - c.x) * gloss), 0.f, 1.f);
        c.y = Util::Clamp(c.y + ((1.f - c.y) * gloss), 0.f, 1.f);
        c.z = Util::Clamp(c.z + ((1.f - c.z) * gloss), 0.f, 1.f);
        return ImGui::ColorConvertFloat4ToU32(c);
    };

    // ---- geometry ----------------------------------------------------------
    static ImVec2 ring[RingCount][Segments];
    static float ringDepth[RingCount][Segments];
    static Vec3 bandNormal[BandCount][Segments];
    // How far into the cleft between the labia each vertex of the face lies,
    // for shading it deeper.
    static float vulvaShade[RingCount][Segments];
    // Where each vertex lies across the face and along the slit, in face
    // radii, for the face grid to look the height map up at.
    static float faceU[RingCount][Segments];
    static float faceV[RingCount][Segments];
    // Every vertex of the body in view space, so the normals can be taken
    // from the surface as built, lips and folds and all.
    static Vec3 bodyPos[RingCount][Segments];
    // Semi axes of each ring after the rod has had its way with them.
    // How much each ring is drawn as a lens rather than an oval.
    float ringLens[RingCount] = {};
    float ringRx[RingCount];
    float ringRz[RingCount];
    // Lateral offset of each ring, non-zero only where the rod has shouldered
    // the canal off the body's axis.
    float ringCx[RingCount] = {};
    float ringCz[RingCount] = {};

    float cosA[Segments];
    float sinA[Segments];
    // The slit's taper depends only on which segment it is, so it is worked
    // out per segment here rather than per vertex in the rings below.
    float slitTaper[Segments];
    for(int32_t i = 0; i < Segments; i += 1) {
        const float a = (2.f * (float)M_PI * (float)i) / (float)Segments;
        cosA[i] = SDL_cosf(a);
        sinA[i] = SDL_sinf(a);
        slitTaper[i] = SDL_powf(std::abs(cosA[i]), SlitTipSharpness);
    }

    // The rod does not move. Its tip sits exactly where the bore ceiling ends
    // up at the bottom of the stroke, so the case bottoms out on it rather
    // than passing through, and lifts clear of it at the top.
    // Anchored so that at the bottom of the stroke the tip is RodInsertDepth
    // past the vulva. Measured against the vulva rather than the end of the
    // profile, because the canal ceiling is nowhere near the rod.
    const float rodTipY = -StrokeTravel + vulvaY + RodInsertDepth;

    // Where the rod's surface actually is at a given height. The canal is
    // opened to match this rather than to a ramp: the tip reaches most of its
    // width within the first few hundredths, so anything smoother than the rod
    // itself leaves the hole narrower than the thing in it.
    // Length of the head, set so that the tip to the corona is about as tall
    // as the head is across: a mushroom rather than a cap.
    constexpr float RodNoseLength = RodRadius * 2.7f;
    // Shape of that head, as distance below the tip against a multiple of the
    // shaft radius. A rounded cone that swells past the shaft, holds, tucks
    // under it into a groove, then returns to the shaft. A curve rather than a
    // dome, because a dome can neither flare nor undercut.
    //
    // The geometry and the canal that closes around it are both read from this
    // table, so they cannot end up describing different shapes.
    // A glans: blunt at the tip, filling out to a corona that stands well
    // proud of the shaft, which turns under sharply into the sulcus and then
    // eases back out to the shaft.
    static constexpr float RodNoseShape[][2] = {
        { 0.000f, 0.000f },
        { 0.010f, 0.160f },
        { 0.030f, 0.300f },
        { 0.070f, 0.460f },
        { 0.130f, 0.620f },
        { 0.210f, 0.780f },
        { 0.310f, 0.920f },
        { 0.420f, 1.040f },
        { 0.530f, 1.120f },
        { 0.620f, 1.160f },
        { 0.650f, 1.165f },
        { 0.672f, 1.120f },
        { 0.695f, 0.950f },
        { 0.718f, 0.870f },
        { 0.780f, 0.880f },
        { 0.870f, 0.940f },
        { 1.000f, 1.000f },
    };
    constexpr int32_t RodNoseSamples = (int32_t)(sizeof(RodNoseShape) / sizeof(RodNoseShape[0]));
    auto rodNoseRadius = [&](float below) noexcept -> float {
        const float u = Util::Clamp(below / RodNoseLength, 0.f, 1.f);
        for(int32_t i = 1; i < RodNoseSamples; i += 1) {
            if(u > RodNoseShape[i][0]) continue;
            const float span = Util::Max(1e-5f, RodNoseShape[i][0] - RodNoseShape[i - 1][0]);
            const float t = (u - RodNoseShape[i - 1][0]) / span;
            return RodRadius * (RodNoseShape[i - 1][1]
                + ((RodNoseShape[i][1] - RodNoseShape[i - 1][1]) * t));
        }
        return RodRadius;
    };
    // The pulses of a finish, as the rod shows them: a swell running up the
    // shaft into each spurt. Read here so the canal, which opens to the rod's
    // radius, is pushed out by the swell as it passes.
    const RodThrob rodThrob = rodThrobAt(FinishEasterEgg ? finishClock : -1.f);
    // The twitch, as an amount: fast up, slow down. Its direction is applied
    // to the centreline further on; here it swells the tip.
    float twitchAmp = 0.f;
    if(FinishEasterEgg && finishTwitchTime >= 0.f) {
        const float t = finishTwitchTime;
        twitchAmp = finishTwitchStrength * 1.3f * (SDL_expf(-t / 0.20f) - SDL_expf(-t / 0.035f));
    }
    auto rodThrobScale = [&](float y) noexcept -> float {
        const float along = Util::Clamp(1.f - ((rodTipY - y) / Util::Max(1e-3f, RodLength)), 0.f, 1.f);
        // The head swells with each twitch, most at the very tip.
        const float nose = Util::Clamp(1.f - ((rodTipY - y) / RodNoseLength), 0.f, 1.f);
        return rodThrobAlong(rodThrob, along) * (1.f + (0.30f * twitchAmp * nose));
    };
    auto rodRadiusAt = [&](float y) noexcept -> float {
        const float throb = rodThrobScale(y);
        const float below = rodTipY - y;
        if(below >= RodNoseLength) return RodRadius * throb;
        if(below >= 0.f) return rodNoseRadius(below) * throb;
        // Just above the tip the material is already being pushed aside.
        const float t = Util::Clamp(1.f + (below / 0.07f), 0.f, 1.f);
        return (rodNoseRadius(0.f) + ((RodRadius * 0.22f) * t)) * throb;
    };

    // Where the body's axis sits at a given height. This is the line the rod is
    // bending toward, and what the canal is centred on where the rod has not
    // reached it.
    const Vec3 axisDir = orientNoCamera(Vec3{ 0.f, 1.f, 0.f });
    auto bodyAxisLateralAt = [&](float worldY) noexcept -> Vec3 {
        if(std::abs(axisDir.y) < 1e-3f) return Vec3{ 0.f, 0.f, 0.f };
        const float k = (worldY - pivotY - worldOffset.y) / axisDir.y;
        return Vec3{ worldOffset.x + (axisDir.x * k), 0.f, worldOffset.z + (axisDir.z * k) };
    };

    // Centreline of the rod, as a beam: clamped upright where it is anchored,
    // and lying along the body's axis by the time it is in the canal. A single
    // cubic between those two end conditions. It used to be a straight run up
    // to the vulva and a second straight run after it, which met at an angle
    // and read as two rods hinged at the lip rather than one that bends.
    //
    // It passes exactly through the opening, because that is the hole it is
    // going into. What RodFollow scales is the angle it arrives at, not the
    // position; whatever angle it does not take up is what the canal has to
    // absorb, and that is the deformation worth seeing.
    const float rodBaseY = rodTipY - RodLength;

    // The twitch flexes the rod in the pitch plane, toward the front of the
    // body, the way a real one jerks upward, and that is fixed in the world
    // whatever the camera is doing. Most at the tip and not at all at the
    // base where it is bolted. The canal sits on the rod wherever the rod
    // touches it, so it is nudged along with it.
    const Vec3 twitchDir{ 0.f, 0.f, -1.f };
    auto twitchLateralAt = [&](float worldY) noexcept -> Vec3 {
        const float s = Util::Clamp((worldY - rodBaseY) / Util::Max(1e-3f, RodLength), 0.f, 1.f);
        const float d = twitchAmp * RodRadius * 0.55f * s * s;
        return Vec3{ twitchDir.x * d, 0.f, twitchDir.z * d };
    };
    // Radius of the flared foot at a height. Zero above it, where there is no
    // flare to press against.
    // How hard the lip is pressed onto the foot, for the foot's own shape:
    // nothing until the vulva is near it, everything once it is down.
    const float baseLipSeat = Util::Clamp(
        1.f - (((pivotY + worldOffset.y) - rodBaseY) / LipSeatRange), 0.f, 1.f);
    auto footRadiusAt = [&](float worldY) noexcept -> float {
        if(worldY >= rodBaseY) return 0.f;
        const float above = Util::Clamp(
            (worldY - (rodBaseY - RodFootHeight)) / RodFootHeight, 0.f, 1.f);
        float rad = RodRadius;
        for(int32_t i = 1; i < RodFootSamples; i += 1) {
            if(above > RodFootShape[i][0]) continue;
            const float span = Util::Max(1e-5f, RodFootShape[i][0] - RodFootShape[i - 1][0]);
            const float t = (above - RodFootShape[i - 1][0]) / span;
            rad = RodRadius * (RodFootShape[i - 1][1]
                + ((RodFootShape[i][1] - RodFootShape[i - 1][1]) * t));
            break;
        }
        // The base is soft rubber. The lip coming down on it presses a groove
        // into the top of the flare, and what is pressed out of the groove
        // bulges the flare below it. The sleeve seats in the groove, so it is
        // stretched over less.
        const float groove = SDL_expf(-squared((above - 0.82f) / 0.22f));
        const float bulge = Util::Clamp(1.f - (above / 0.55f), 0.f, 1.f);
        // Never narrower than the shaft it carries, or the base reads as a
        // separate part with a neck between the two.
        const float pressed = rad * (1.f - (FootGive * baseLipSeat * groove)) * (1.f + (0.30f * FootGive * baseLipSeat * bulge));
        return Util::Max(RodRadius, pressed);
    };
    const float vulvaWorldY = pivotY + worldOffset.y;
    // The bend is resolved over the shaft below the vulva, but that shaft goes
    // to nothing at the bottom of the stroke, where the vulva comes right down
    // onto the foot. A sideways offset resolved over no distance is a step, not
    // a bend, so the span is floored and the curve simply starts below the foot
    // when it has to. Above that it is the shaft length exactly, as before.
    constexpr float MinBendSpan = 0.45f;
    const float bendSpan = Util::Max(MinBendSpan, vulvaWorldY - rodBaseY);
    // The bend runs up from the base, never down past it. Anchoring it on the
    // vulva instead let it start below the foot once the vulva came down far
    // enough, and the foot swung about with the body -- but it is bolted to
    // something, and the one part of the rod that must never move is the plane
    // it is bolted at.
    const float bendTop = rodBaseY + bendSpan;
    const Vec3 vulvaSlope = std::abs(axisDir.y) < 1e-3f
        ? Vec3{ 0.f, 0.f, 0.f }
        : Vec3{ (axisDir.x / axisDir.y) * RodFollow, 0.f,
                (axisDir.z / axisDir.y) * RodFollow };
    // Where the bend has to arrive: on the canal, at whatever height it
    // actually finishes at. While there is shaft enough that height is the
    // vulva, and the canal passes through the vulva by definition, so this is
    // the vulva's own position and nothing changes. Where the span has been
    // floored it is a little further up the canal instead -- which is still
    // the canal. Aiming at the vulva's position at a height that is no longer
    // the vulva aimed the rod at a place the case had already swung away from:
    // the tilt pivots at the vulva, so half a body length up the axis has
    // moved several radii across, and the rod ran up outside the shell.
    const Vec3 bendTopCentre = bodyAxisLateralAt(bendTop);
    auto rodCentreAt = [&](float worldY) noexcept -> Vec3 {
        const Vec3 tw = twitchLateralAt(worldY);
        if(worldY >= bendTop) {
            // Straight on at the angle the bend finished with.
            const float k = worldY - bendTop;
            return Vec3{ bendTopCentre.x + (vulvaSlope.x * k) + tw.x, worldY,
                         bendTopCentre.z + (vulvaSlope.z * k) + tw.z };
        }
        // Hermite: clamped flat at the base, and at the far end the canal's
        // position and angle. The slight counter-curve near the base is what a
        // beam held at one end and pushed sideways at the other actually does.
        //
        // Where the stroke has brought the vulva right down to the foot there
        // is less than MinBendSpan to do it in, and the bend finishes a little
        // up the canal rather than at the lip. That costs a touch of alignment
        // through the lip, which is worth rather less than a base that stays
        // where it is bolted.
        const float t = Util::Clamp((worldY - rodBaseY) / bendSpan, 0.f, 1.f);
        const float h01 = (t * t) * (3.f - (2.f * t));
        const float h11 = (t * t) * (t - 1.f);
        return Vec3{
            (bendTopCentre.x * h01) + (vulvaSlope.x * bendSpan * h11) + tw.x,
            worldY,
            (bendTopCentre.z * h01) + (vulvaSlope.z * bendSpan * h11) + tw.z };
    };
    // Direction the centreline is running at a height. Taken from the curve
    // itself so the two cannot disagree.
    auto rodTangentAt = [&](float worldY) noexcept -> Vec3 {
        constexpr float h = 0.01f;
        const Vec3 a = rodCentreAt(worldY - h);
        const Vec3 b = rodCentreAt(worldY + h);
        return normalize(Vec3{ b.x - a.x, b.y - a.y, b.z - a.z });
    };
    // Cross-section frame at a height. Rings kept square to world up are
    // sheared rather than bent: the rod reads as a stack of slices sliding
    // sideways instead of one solid thing curving. Reference is fixed to X,
    // which the rod can never approach at these tilt limits, so the seam does
    // not swap sides partway up.
    auto rodFrameAt = [&](float worldY, Vec3& u, Vec3& v) noexcept {
        const Vec3 t = rodTangentAt(worldY);
        u = normalize(cross(Vec3{ 1.f, 0.f, 0.f }, t));
        v = cross(t, u);
    };

    // How far in the rod is, 0 withdrawn to 1 at the bottom of the stroke.
    const float insertion = RodInsertDepth > 0.f
        ? Util::Clamp((rodTipY - (strokeOffset + vulvaY)) / RodInsertDepth, 0.f, 1.f)
        : 0.f;

    // The ribs are elastic and in contact with a moving surface, so they are
    // pulled along with it and spring back when it stops. Integrated from the
    // change in insertion rather than from position, because what drags them
    // is the rod moving, not where it happens to be.
    {
        const float dt = Util::Clamp(ImGui::GetIO().DeltaTime, 1.f / 480.f, 1.f / 20.f);
        // Flesh, not rubber tubing: the ridges are carried along by the rod's
        // own movement through them, as friction does, and held back by the
        // wall they belong to, so they travel with each stroke, overshoot as it
        // turns, and wobble to rest. A damped spring, stepped implicitly.
        constexpr float DragCouple = 9.f;
        const float rodVel = lastInsertion >= 0.f ? ((insertion - lastInsertion) / dt) * RibDragGain : 0.f;
        ribDragVel = (ribDragVel + (((DragCouple * rodVel) - (RibDragK * ribDrag)) * dt))
            / (1.f + ((DragCouple + RibDragDamp) * dt) + (RibDragK * dt * dt));
        ribDrag += ribDragVel * dt;
        if(!std::isfinite(ribDrag) || !std::isfinite(ribDragVel)) {
            ribDrag = 0.f;
            ribDragVel = 0.f;
        }
        ribDrag = Util::Clamp(ribDrag, -RibDragLimit, RibDragLimit);
        lastInsertion = insertion;

        // The soft sleeve about the opening jiggles as the case strokes: the
        // stroke's acceleration sets a damped spring going that swells and
        // slims it a little.
        const float strokeVel = fleshPrimed ? (strokeOffset - fleshLastStroke) / dt : 0.f;
        const float strokeAccel = fleshPrimed ? (strokeVel - fleshLastStrokeVel) / dt : 0.f;
        fleshLastStroke = strokeOffset;
        fleshLastStrokeVel = strokeVel;
        fleshPrimed = true;
        fleshJiggleVel = (fleshJiggleVel + (((-Util::Clamp(strokeAccel, -300.f, 300.f) * 0.00012f) - (90.f * fleshJiggle)) * dt))
            / (1.f + (6.f * dt) + (90.f * dt * dt));
        fleshJiggle = Util::Clamp(fleshJiggle + (fleshJiggleVel * dt), -0.06f, 0.06f);
        if(!std::isfinite(fleshJiggle) || !std::isfinite(fleshJiggleVel)) {
            fleshJiggle = 0.f;
            fleshJiggleVel = 0.f;
        }
    }

    // Where the shell sits at a given height, so the sleeve can be told where
    // it has to stop. The case rings run from the closed end down to the vulva.
    auto caseBoreAt = [&](float profileY) noexcept -> float {
        float prevY = 0.f, prevR = 0.f;
        bool have = false;
        for(int32_t i = 0; i < RingCount; i += 1) {
            if(Profile[i].sleeve) continue;
            const float cy = Profile[i].y;
            const float cr = Profile[i].r * Radius;
            if(have && profileY <= prevY && profileY >= cy) {
                const float t = (prevY - profileY) / Util::Max(1e-6f, prevY - cy);
                return prevR + ((cr - prevR) * t);
            }
            prevY = cy;
            prevR = cr;
            have = true;
        }
        return 1e9f;  // past either end of the shell: nothing in the way
    };
    // First ring of the opening. Everything from here on is the lip and the
    // bore behind it. Squash used to identify those, but the bore is round
    // further in, so it no longer can.
    int32_t firstBoreRing = RingCount;
    for(int32_t i = 0; i < RingCount; i += 1) {
        if(Profile[i].sx < 1.f) { firstBoreRing = i; break; }
    }
    // The first ring of the cervix: past the vault, where the profile turns
    // back down to hang into it.
    int32_t cervixFirstRing = RingCount;
    for(int32_t i = firstBoreRing + 1; i < RingCount; i += 1) {
        if(Profile[i].y < Profile[i - 1].y && Profile[i].y > 0.f) { cervixFirstRing = i; break; }
    }
    // Cross-section of each ring. Across the slit it is the profile's own
    // squash throughout; along the slit it starts at full width at the lip and
    // closes down to meet the other, so the section is round from there in.
    float ringSquashZ[RingCount];
    // How far into the bore each ring is, on the same ramp: nothing at the lip,
    // everything once the section has come round.
    float ringBore[RingCount] = {};
    for(int32_t i = 0; i < RingCount; i += 1) {
        ringSquashZ[i] = 1.f;
        if(i <= SlitRing) continue;
        const float intoBore = (Profile[i].y - Profile[SlitRing].y) * HalfLength;
        float t = Util::Clamp(intoBore / SlitBlend, 0.f, 1.f);
        t = t * t * (3.f - (2.f * t));
        ringBore[i] = t;
        ringSquashZ[i] = 1.f + ((Profile[i].sx - 1.f) * t);
    }
    // The ring the highlight is drawn on: where the opening has come round
    // from the slit at the lips, rather than the lips themselves.
    int32_t rimRing = SlitRing;
    for(int32_t i = SlitRing; i < RingCount; i += 1) {
        if(ringBore[i] >= 0.98f) { rimRing = i; break; }
    }

    // Where the ribs and flutes give way to the smooth wall of the reservoir,
    // in profile units: 1 down the canal, easing to 0 across the neck.
    constexpr float ReservoirStartY = 0.310f;
    // Where the chamber itself begins to widen out of the neck: the floor of
    // the pool. Below it is canal, and a pool reaching down into it was drawn
    // tapering to a point there, a cone.
    constexpr float ReservoirFloorY = 0.470f;
    // The tip of the cervix, which the rod bottomed out meets and presses
    // back: how far it can be pressed, in world units, and how far up the
    // cervix that reaches before the uterus, which does not move.
    constexpr float CervixTipY = 0.3250f;
    constexpr float CervixPressMax = 0.045f;
    // The flutes stay out of the first stretch of the canal behind the
    // opening. That is the part seen through the vulva when the rod opens
    // it, and their four lobes showed there as points at the sides and ends
    // of the opening.
    float ringFluteFade[RingCount] = {};
    for(int32_t i = SlitRing; i < RingCount; i += 1) {
        const float into = (Profile[i].y - Profile[SlitRing].y) * HalfLength;
        float t = Util::Clamp((into - 0.12f) / 0.25f, 0.f, 1.f);
        ringFluteFade[i] = t * t * (3.f - (2.f * t));
    }
    float ringRibMask[RingCount];
    for(int32_t i = 0; i < RingCount; i += 1) {
        float t = Util::Clamp((Profile[i].y - ReservoirStartY) / 0.03f, 0.f, 1.f);
        ringRibMask[i] = 1.f - (t * t * (3.f - (2.f * t)));
    }

    // The uterus distends with what it holds (UterusDistension), each of its
    // rings moving from the empty shape to the full one (UterusRest, UterusFull).
    // The rings and the pool inside both read their shape from here, so the two
    // always line up. The rod bottomed out against the cervix pushes it up into
    // the uterus, which distends a little around it on top of what it holds.
    // Squashed as flesh is: it lags a touch behind the head pressing into it
    // and wobbles back when the head draws away, a damped spring towards how
    // far the rod reaches past it.
    {
        const float pressDt = Util::Clamp(ImGui::GetIO().DeltaTime, 1.f / 480.f, 1.f / 20.f);
        const float want = Util::Clamp(rodTipY + 0.004f - (strokeOffset + (CervixTipY * HalfLength)), 0.f, CervixPressMax);
        cervixPressVel = (cervixPressVel + ((400.f * (want - cervixPress)) * pressDt))
            / (1.f + (22.f * pressDt) + (400.f * pressDt * pressDt));
        cervixPress = Util::Clamp(cervixPress + (cervixPressVel * pressDt), 0.f, CervixPressMax * 1.15f);
        if(!std::isfinite(cervixPress) || !std::isfinite(cervixPressVel)) {
            cervixPress = want;
            cervixPressVel = 0.f;
        }
    }
    const float cervixPushShare = Util::Clamp(cervixPress / CervixPressMax, 0.f, 1.f);
    const float uterusShape = Util::Clamp(UterusDistension + (0.25f * cervixPushShare), 0.f, 1.f);
    int32_t uterusFirstRing = RingCount;
    for(int32_t i = cervixFirstRing; i < RingCount; i += 1) {
        if(Profile[i].y >= ReservoirFloorY - 1e-4f) { uterusFirstRing = i; break; }
    }
    auto uterusRing = [&](int32_t s, float distension, float& ringY, float& ringR) noexcept {
        const int32_t k = Util::Clamp(s - uterusFirstRing, 0, UterusShapeRings - 1);
        ringY = UterusRest[k][0] + ((UterusFull[k][0] - UterusRest[k][0]) * distension);
        ringR = UterusRest[k][1] + ((UterusFull[k][1] - UterusRest[k][1]) * distension);
    };

    // Phase of the flutes at a ring and segment. Rectified, so they only ever
    // stand proud of the wall: a trough cutting inward would dip inside the rod
    // wherever the two are in contact.
    // Straight down the bore rather than winding along it. A cosine so the
    // pattern falls either side of the slit's own axes: the opening has to
    // read the same on both sides, and a sine would be a half period out and
    // put a ridge on one lip against a hollow on the other.
    auto fluteAngle = [&](int32_t i) noexcept -> float {
        const float around = ((2.f * (float)M_PI) * (float)i) / (float)Segments;
        return around * (float)CanalFlutes;
    };
    // Faded out at the lip. They wind along the bore, and a helix has no
    // mirror image, so carrying them through the opening left one side of it
    // shaped differently from the other. The opening should read the same on
    // both sides of the slit; behind it, where the flutes are what makes the
    // twist axis legible, none of that matters.
    auto fluteAt = [&](int32_t s, int32_t i) noexcept -> float {
        const float amp = CanalFluteAmp * ringBore[s] * ringRibMask[s] * ringFluteFade[s];
        return 1.f + (amp * (0.5f + (0.5f * SDL_cosf(fluteAngle(i)))));
    };
    // How fast it is changing around the bore, which is what tilts the normal
    // and so the only reason the flutes are lit rather than merely bulging.
    auto fluteSlope = [&](int32_t s, int32_t i) noexcept -> float {
        return -CanalFluteAmp * ringBore[s] * ringRibMask[s] * ringFluteFade[s] * 0.5f * (float)CanalFlutes
            * SDL_sinf(fluteAngle(i));
    };

    // Where the cap's rim is, so the face can be told from the sleeve inside.
    const float CapRimY = -0.826f;
    // The rings the face grid spans: from where the sleeve comes out of the
    // cap to the lip turning inward. Their own quads are not drawn.
    int32_t faceFirstRing = SlitRing;
    for(int32_t i = 0; i < SlitRing; i += 1) {
        if(Profile[i].sleeve && Profile[i].y <= CapRimY + 1e-4f) { faceFirstRing = i; break; }
    }
    const int32_t faceLastRing = Util::Min(SlitRing + 2, faceFirstRing + FaceBandsMax);
    auto sampleMap = [](const uint8_t* map, float u, float v) noexcept -> float {
        const float fx = Util::Clamp(((u + 1.f) * 0.5f * (float)VulvaMapSize) - 0.5f, 0.f, (float)(VulvaMapSize - 1));
        const float fy = Util::Clamp(((1.f - v) * 0.5f * (float)VulvaMapSize) - 0.5f, 0.f, (float)(VulvaMapSize - 1));
        const int32_t x0 = (int32_t)fx;
        const int32_t y0 = (int32_t)fy;
        const int32_t x1 = Util::Min(x0 + 1, VulvaMapSize - 1);
        const int32_t y1 = Util::Min(y0 + 1, VulvaMapSize - 1);
        const float tx = fx - (float)x0;
        const float ty = fy - (float)y0;
        const float top = map[(y0 * VulvaMapSize) + x0] + ((map[(y0 * VulvaMapSize) + x1] - map[(y0 * VulvaMapSize) + x0]) * tx);
        const float bottom = map[(y1 * VulvaMapSize) + x0] + ((map[(y1 * VulvaMapSize) + x1] - map[(y1 * VulvaMapSize) + x0]) * tx);
        return (top + ((bottom - top) * ty)) / 255.f;
    };

    // How hard the lip is pressed onto the foot: nothing until the vulva is
    // near it, everything once it is down.
    const float lipSeat = Util::Clamp(
        1.f - ((vulvaWorldY - rodBaseY) / LipSeatRange), 0.f, 1.f);

    // Closed end of the sleeve, the far end from the vulva.
    float sleeveTopY = -1e9f;
    for(int32_t i = 0; i < RingCount; i += 1) {
        if(Profile[i].sleeve) sleeveTopY = Util::Max(sleeveTopY, Profile[i].y);
    }

    for(int32_t s = 0; s < RingCount; s += 1) {
        float y = strokeOffset + (Profile[s].y * HalfLength);
        float r = Profile[s].r * Radius;
        // The soft sleeve jiggles about the opening as the case strokes.
        if(Profile[s].sleeve) {
            const float fromVulvaJ = std::abs(Profile[s].y - Profile[SlitRing].y) * HalfLength;
            const float jiggleShare = Util::Clamp(1.f - (fromVulvaJ / 0.35f), 0.f, 1.f);
            r *= 1.f + (fleshJiggle * jiggleShare * jiggleShare);
        }
        // Bottomed out, the tip of the rod meets the tip of the cervix and
        // presses it back: the cervix rides up ahead of the rod and fattens a
        // little as it is squashed, fading out up the cervical canal so the
        // uterus behind it stays put.
        if(s >= cervixFirstRing && Profile[s].y > ReservoirStartY && Profile[s].y < ReservoirFloorY) {
            const float press = Util::Min(cervixPress, CervixPressMax);
            if(press > 0.f) {
                const float along = Util::Clamp((0.4050f - Profile[s].y) / 0.08f, 0.f, 1.f);
                const float knob = Util::Clamp((0.4150f - Profile[s].y) / 0.05f, 0.f, 1.f);
                y += press * along;
                r *= 1.f + (0.45f * (press / CervixPressMax) * knob);
                // It takes the shape of the head it is squashed onto rather
                // than resting on its very tip with a gap round the dome:
                // wherever the rod is still wider than the knob at a ring's
                // height, the ring is pushed up until it is not.
                const float ringR = r * Profile[s].sx;
                for(int32_t k = 0; k < 16 && rodRadiusAt(y) > ringR; k += 1) {
                    y += CervixPressMax / 16.f;
                }
            }
        }
        // The uterus at its current distension.
        if(s >= uterusFirstRing) {
            float ringY = 0.f;
            float ringR = 0.f;
            uterusRing(s, uterusShape, ringY, ringR);
            y = strokeOffset + (ringY * HalfLength);
            r = ringR * Radius;
        }
        if(FinishEasterEgg && s >= firstBoreRing) {
            // Fluid on its way up the canal bulges the wall around it, as the
            // rod does, so a rope shows as a swelling running up the sleeve
            // even where the near wall hides the rope itself.
            const float localY = Profile[s].y * HalfLength;
            float bulge = 0.f;
            for(const auto& drop : FinishDrops) {
                if(drop.rope < 0 || drop.phase != 0) continue;
                const float d = (localY - drop.y) / 0.045f;
                if(d > 4.f || d < -4.f) continue;
                bulge += drop.size * SDL_expf(-d * d);
            }
            r *= 1.f + (0.22f * Util::Min(1.f, bulge * 0.35f));
            // A full pool weighs on the neck below it and the chamber sags
            // into the canal, so the sleeve reads as holding a volume rather
            // than as a drawing of one.
            const float neck = Util::Clamp(1.f - (std::abs(Profile[s].y - ReservoirFloorY) / 0.10f), 0.f, 1.f);
            r *= 1.f + (0.10f * FinishPileFraction * neck * ringRibMask[s]);
        }

        // Material driven in at the vulva also has nowhere to go at the far
        // end, where the sleeve is closed and the shell is not touching it, so
        // the whole closed end lifts.
        if(Profile[s].sleeve && insertion > 0.f) {
            const float fromTip = (sleeveTopY - Profile[s].y) * HalfLength;
            const float f = Util::Clamp(1.f - (fromTip / TipSwellReach), 0.f, 1.f);
            y += TipSwell * insertion * f * f * (3.f - (2.f * f));

            // ...and the lip pushes the other way, out of the opening.
            const float fromVulva =
                std::abs(Profile[s].y - Profile[SlitRing].y) * HalfLength;
            const float m = Util::Clamp(1.f - (fromVulva / VulvaExtendReach), 0.f, 1.f);
            // Not all of it goes once the lip is seated: it still has to reach
            // down over the flare, which is what it is now wrapped around.
            y -= VulvaExtend * insertion * (1.f - (0.45f * lipSeat))
                * m * m * (3.f - (2.f * m));
        }

        // Driving the rod in displaces material, which has to go somewhere:
        // the sleeve swells around the vulva. Falls off quickly with distance
        // so the swelling stays at the opening and the body further up, which
        // is inside the case, does not grow into the shell.
        if(Profile[s].sleeve && insertion > 0.f) {
            const float fromVulva = std::abs(Profile[s].y - Profile[SlitRing].y) * HalfLength;
            const float falloff = Util::Clamp(1.f - (fromVulva / VulvaBulgeReach), 0.f, 1.f);
            // Eased so the swell is roundest right at the vulva rather than
            // ramping linearly, which reads as slack material rather than a
            // cone.
            const float eased = falloff * falloff * (3.f - (2.f * falloff));
            r *= 1.f + (VulvaBulge * insertion * eased);
            // Bunched into folds, which stand where the swell is.
            const float fold = SDL_sinf((fromVulva / FoldPeriod) * 2.f * (float)M_PI);
            r *= 1.f + (FoldAmp * insertion * eased * (0.5f + (0.5f * fold)));
        }

        // ...and rolled outward where it meets the foot.
        if(Profile[s].sleeve && lipSeat > 0.f) {
            const float fromVulva =
                std::abs(Profile[s].y - Profile[SlitRing].y) * HalfLength;
            const float d = ((fromVulva / LipSplayReach) - LipSplayPeak) / LipSplayWidth;
            r *= 1.f + (LipSplay * lipSeat * SDL_expf(-(d * d)));
        }
        // Inside the cap the shell holds it: whatever the swell and the folds
        // ask for, the sleeve stops at the shell and only bulges where it is
        // out in the open.
        if(Profile[s].sleeve && s < firstBoreRing) {
            r = Util::Min(r, Util::Max(Profile[s].r * Radius, caseBoreAt(Profile[s].y) - ShellClearance));
            r = Util::Min(r, Util::Max(Profile[s].r * Radius, VulvaMaxRadius));
        }

        // Where this ring sits in the world, so the rod can be asked what it
        // is doing at that height.
        Vec3 centreWorld = orientNoCamera(Vec3{ 0.f, y - pivotY, 0.f });
        centreWorld.y += pivotY;
        centreWorld = add(centreWorld, worldOffset);
        const float rodHeight = centreWorld.y;

        // The outer wall of the sleeve follows the bore out. Rubber stretched
        // around something thicker than the hole does not keep a constant
        // outside diameter, and letting it read that way was what made the
        // sleeve look like a rigid tube with a hole through it.
        if(s < firstBoreRing && Profile[s].sleeve) {
            const float opened = Util::Max(
                0.f, (rodRadiusAt(rodHeight) * (1.f + RodCanalGap)) - CanalRestRadius);
            const float limit = Util::Max(r, caseBoreAt(Profile[s].y) - ShellClearance);
            r = Util::Min(r + (SleeveStretch * opened), limit);
            // The stretched reservoir shows on the outside too: the sleeve
            // swells around it, up against the case, which is hard and stops
            // it, so the swell flattens where it meets the shell.
            if(FinishEasterEgg && UterusDistension > 0.5f) {
                const float w = SDL_expf(-squared((Profile[s].y - 0.680f) / 0.14f));
                r = Util::Min(r * (1.f + ((UterusDistension - 0.5f) * 0.45f * w)), limit);
            }
        }

        float rx = r * Profile[s].sx;
        float rz = r * ringSquashZ[s];
        // Rings the rod has passed through are stretched around it. Only the
        // squashed rings are affected, which is exactly the slit and the canal
        // behind it; the outer body is rigid. Opened to the rod's own radius
        // at that height plus a gap, so the wall follows the rod without the
        // two surfaces merging into one silhouette.
        if(s >= firstBoreRing) {
            const float contact = Util::Clamp((rodTipY - rodHeight) / 0.14f, 0.f, 1.f);

            // Ribs, dragged along only where the rod is actually touching them.
            // Above the tip nothing is in contact, so they sit at rest and the
            // drag fades in across the nose.
            const float phase = (Profile[s].y / CanalRibPeriod) + (ribDrag * contact);
            // Semicircular ridges: each a half disc standing into the canal,
            // rounded towards the rod, meeting the next in a crease out in the
            // wall, rather than a sine's even swell and trough.
            const float ribFrac = phase - std::floor(phase);
            const float ribX = (2.f * ribFrac) - 1.f;
            // Rounded at the crease as well as the crest: a true half disc meets
            // the next in a knife edge, and flesh has none.
            const float wave = 1.f - (2.f * SDL_powf(Util::Max(0.f, 1.f - (ribX * ribX)), 0.75f));
            const float ribbed = r * (1.f + (CanalRibAmp * ringRibMask[s] * wave));
            rx = ribbed * Profile[s].sx;
            rz = ribbed * ringSquashZ[s];

            const float target = rodRadiusAt(rodHeight) * (1.f + RodCanalGap);
            if(target > 0.f) {
                // Squashed, not erased. Clamping the wall flat to the rod
                // wipes out every rib the rod is wide enough to reach, which
                // is precisely where they should be gripping hardest. Instead
                // the ribs ride on it: troughs sit at the gap and crests still
                // stand proud, by what is left of their depth once compressed.
                const float bump = r * CanalRibAmp * ringRibMask[s] * RibSquish * (0.5f + (0.5f * wave));
                // Where a ridge meets the rod it presses into it rather than
                // lying flattened against it, so its crest overlaps the rod's
                // outline and the rod reads as held by the canal, not passed
                // through a tube.
                constexpr float RidgeGrip = 0.12f;
                const float crest = 0.5f - (0.5f * wave);
                const float pressed = target + bump - (target * RidgeGrip * crest * ringRibMask[s]);
                rx = Util::Max(rx, pressed);
                rz = Util::Max(rz, pressed);
                // The canal is pushed aside as well as opened. Whatever lean
                // the rod has not taken up by bending is left over as an offset
                // between the two centrelines, and the ring shifts onto the rod
                // to take it up. Measured in world and rotated back, because
                // the two are described in different frames.
                const Vec3 rodCentre = rodCentreAt(rodHeight);
                const Vec3 gap{ rodCentre.x - centreWorld.x, 0.f, rodCentre.z - centreWorld.z };
                const Vec3 local = unrotate(gap);
                ringCx[s] = local.x * contact;
                ringCz[s] = local.z * contact;
            }
        }
        // The vulva is pliable: its lips open to the rod as a spring rather than
        // snapping to its outline, so they lag a touch behind it going in and
        // wobble back as it withdraws.
        if(s >= firstBoreRing && std::abs(Profile[s].y - Profile[SlitRing].y) < LipSpringZone) {
            if(lipGape.size() != (size_t)RingCount) {
                lipGape.assign(RingCount, 0.f);
                lipGapeVel.assign(RingCount, 0.f);
            }
            const float lipDt = Util::Clamp(ImGui::GetIO().DeltaTime, 1.f / 480.f, 1.f / 30.f);
            const float restRx = r * Profile[s].sx;
            const float restRz = r * ringSquashZ[s];
            const float target = Util::Max(0.f, Util::Max(rx - restRx, rz - restRz));
            float& gape = lipGape[s];
            float& vel = lipGapeVel[s];
            // Semi-implicit: the damping is taken at the new velocity, which
            // keeps it stable at any frame time. Stepped explicitly, a spring
            // this stiff diverged whenever a frame took longer than about a
            // fortieth of a second, and the whole vulva exploded into giant
            // flickering triangles.
            vel = (vel + (LipSpringK * (target - gape) * lipDt)) / (1.f + (LipSpringDamp * lipDt) + (LipSpringK * lipDt * lipDt));
            gape = Util::Clamp(gape + (vel * lipDt), 0.f, target + (0.5f * r));
            if(!std::isfinite(gape) || !std::isfinite(vel)) {
                gape = target;
                vel = 0.f;
            }
            // Skin is continuous: each ring of the lips is drawn open by the
            // rings beside it, as they stood last frame, so the slit opens as
            // a whole into a rounded shape. Opened ring by ring, the rod's tip
            // made a round hole in the middle of a slit that stayed shut above
            // and below it, and the two met in points at its sides and ends.
            float neighbour = 0.f;
            if(s > firstBoreRing) neighbour = Util::Max(neighbour, lipGape[s - 1]);
            if(s + 1 < RingCount) neighbour = Util::Max(neighbour, lipGape[s + 1]);
            float openX = restRx + Util::Max(gape, 0.85f * neighbour);
            float openZ = restRz + gape;
            // ...but wherever the rod runs through the ring, no further along
            // either axis than the rod itself, so the skin closes onto the
            // shaft. The gape is one number for both axes, and the slit's long
            // axis starts out nearly as long as the rod is wide: opening it by
            // the same amount as the short one held the lip standing off the
            // shaft front and back, and the pull from the ring beside it held
            // it at the width of the head it had just passed. Once the rod has
            // gone nothing caps it, and it settles back as a spring.
            const float rodHere = rodRadiusAt(rodHeight) * (1.f + RodCanalGap);
            // Only once the rod is about as wide as the slit is long, though:
            // then the lip wraps it. The thin tip of the rod, just into the
            // lip, capped the ring at its own small radius and punched a round
            // hole in the middle of a slit that stayed long, which came to
            // points at its sides and ends; below that width the slit rounds
            // out as a whole. Eased between the two so nothing jumps as the
            // head goes through.
            if(rodHere > 0.f) {
                const float wrap = Util::Clamp(((rodHere / Util::Max(1e-5f, restRz)) - 0.5f) / 0.4f, 0.f, 1.f);
                openX += (Util::Min(openX, Util::Max(restRx, rodHere)) - openX) * wrap;
                openZ += (Util::Min(openZ, Util::Max(restRz, rodHere)) - openZ) * wrap;
            }
            rx = Util::Max(rx, openX);
            rz = Util::Max(rz, openZ);
        }

        // The relaxed opening is a lens, not an oval: two lips that meet at a
        // corner at each end rather than curving round one. Only where the
        // section is actually flattened, and only while it is relaxed -- once
        // the rod is filling it, it takes the rod's shape instead, which is
        // what it would really do.
        if(s >= firstBoreRing) {
            const float aspect = rx / Util::Max(1e-5f, rz);
            const float restRx = r * Profile[s].sx;
            const float opened = Util::Clamp(
                (rx - restRx) / Util::Max(1e-5f, restRx), 0.f, 1.f);
            ringLens[s] = Util::Clamp(1.f - aspect, 0.f, 1.f) * (1.f - opened);
        }

        // Where the lip has come down past the top of the flare it is lying on
        // a cone, and the cone decides its radius. Opening it here rather than
        // splaying it by a chosen amount is what closes the gap between the two
        // exactly, and keeps it closed however the seat is tuned.
        // (The base itself is handled per vertex below: the sleeve is laid
        // onto its surface rather than opened out to it.)

        ringRx[s] = rx;
        ringRz[s] = rz;

        for(int32_t i = 0; i < Segments; i += 1) {
            const float fl = (s >= firstBoreRing) ? fluteAt(s, i) : 1.f;
            // Pinches the width away towards the ends of the slit faster than
            // a cosine would, which is what puts a corner there instead of a
            // curve.
            const float lensX = (1.f - ringLens[s])
                + (ringLens[s] * slitTaper[i]);
            const float lx = cosA[i] * lensX * rx * fl;
            const float lz = sinA[i] * rz * fl;
            const float dyVulva = 0.f;
            vulvaShade[s][i] = 0.f;
            faceU[s][i] = lx / FaceRadius;
            faceV[s][i] = -lz / FaceRadius;
            Vec3 w = toWorld(Vec3{
                lx + ringCx[s], y + dyVulva,
                lz + ringCz[s] });
            if(Profile[s].sleeve) {
                // The sleeve cannot pass into the base. Where a point of it
                // would be below the base's surface at its distance from the
                // rod, it is laid onto that surface instead, so bottoming out
                // squashes the face down onto the flare and it spreads over it
                // like soft material, rather than passing through. The outer
                // face is what meets the base; the lip and canal stay a wall's
                // thickness further in, so they never show below it.
                Vec3 pw = orientNoCamera(Vec3{ lx + ringCx[s], (y + dyVulva) - pivotY, lz + ringCz[s] });
                pw.y += pivotY;
                pw = add(pw, worldOffset);
                const float clearance = (s < firstBoreRing)
                    ? LipFootClearance
                    : (LipFootClearance + VulvaFaceWall);
                if(pw.y < rodBaseY + clearance) {
                    const Vec3 axis = rodCentreAt(rodBaseY);
                    const float ddx = pw.x - axis.x;
                    const float ddz = pw.z - axis.z;
                    const float rho = SDL_sqrtf((ddx * ddx) + (ddz * ddz)) / RodRadius;
                    // Height of the flare where its radius is rho, from the
                    // same shape the rod is built from: its top at the shaft,
                    // its bottom at the full width of the foot.
                    float above = 1.f;
                    if(rho >= RodFootShape[0][1]) {
                        above = 0.f;
                    }
                    else if(rho > 1.f) {
                        for(int32_t k = 1; k < RodFootSamples; k += 1) {
                            if(rho < RodFootShape[k][1]) continue;
                            const float span = Util::Max(1e-5f, RodFootShape[k - 1][1] - RodFootShape[k][1]);
                            const float t = (RodFootShape[k - 1][1] - rho) / span;
                            above = RodFootShape[k - 1][0] + ((RodFootShape[k][0] - RodFootShape[k - 1][0]) * t);
                            break;
                        }
                    }
                    // The outer face squashes into a bulge at the top of the flare
                    // rather than draping down its cone.
                    float floorY = rodBaseY - (RodFootHeight * (1.f - above));
                    floorY = Util::Max(floorY, rodBaseY - (0.6f * RodFootHeight));
                    floorY += clearance;
                    if(pw.y < floorY) {
                        pw.y = floorY;
                        w = camera(pw);
                    }
                }
            }
            ring[s][i] = project(w, center, scale);
            bodyPos[s][i] = w;
            ringDepth[s][i] = w.z;
        }
    }

    // Band normals come from the profile slope, so a chamfer catches the light
    // differently from the wall above it.
    for(int32_t s = 0; s < BandCount; s += 1) {
        const float dy = (Profile[s + 1].y - Profile[s].y) * HalfLength;
        const float dr = (Profile[s + 1].r - Profile[s].r) * Radius;
        // Profile runs top to bottom so dy is negative; -dy keeps the radial
        // part pointing outward, and dr tilts the normal along the slope.
        const float nr = -dy;
        const float ny = dr;
        // An elliptical cross section does not have a radial normal: for
        // x = A cos t, z = B sin t the outward direction is (B cos t, A sin t),
        // so the axes swap onto the opposite components. Taken from the
        // deformed semi axes so the stretched slit lights correctly too.
        const float ax = Util::Max(1e-4f, (ringRx[s] + ringRx[s + 1]) * 0.5f);
        const float az = Util::Max(1e-4f, (ringRz[s] + ringRz[s + 1]) * 0.5f);
        // Pinching the section changes which way its wall faces: the lips of
        // a lens are steeper than the sides of an oval.
        const float bandLens = (ringLens[s] + ringLens[s + 1]) * 0.5f;
        for(int32_t i = 0; i < Segments; i += 1) {
            const float lensN = (1.f - bandLens)
                + (bandLens * (1.f + SlitTipSharpness) * slitTaper[i]);
            Vec3 radial = normalize(Vec3{ cosA[i] * az, 0.f, sinA[i] * ax * lensN });
            if(s >= firstBoreRing) {
                // For a wall whose radius varies around the bore the normal is
                // no longer radial: it leans by the rate of that variation.
                // Without this the flutes would bulge in silhouette and stay
                // flat everywhere else.
                const float fl = fluteAt(s, i);
                const float dfl = fluteSlope(s, i);
                radial = normalize(Vec3{
                    (radial.x * fl) + (sinA[i] * dfl),
                    0.f,
                    (radial.z * fl) - (cosA[i] * dfl) });
            }
            bandNormal[s][i] = normalize(orient(Vec3{
                radial.x * nr, ny, radial.z * nr }));
        }
    }
    // Then from the surface as built. The profile's normal knows nothing of
    // what shapes a ring around its circumference, the lips and the inner
    // ridge above all, so faces those fold over were lit as if flat and, worse,
    // culled as back faces, which left see-through holes in the vulva. The
    // profile normal only says which way is out.
    for(int32_t s = 0; s < BandCount; s += 1) {
        if(Profile[s].sleeve != Profile[s + 1].sleeve) continue;
        for(int32_t i = 0; i < Segments; i += 1) {
            const int32_t j = (i + 1) % Segments;
            const Vec3& a = bodyPos[s][i];
            const Vec3 up{ bodyPos[s + 1][i].x - a.x, bodyPos[s + 1][i].y - a.y, bodyPos[s + 1][i].z - a.z };
            const Vec3 round{ bodyPos[s][j].x - a.x, bodyPos[s][j].y - a.y, bodyPos[s][j].z - a.z };
            Vec3 g = cross(round, up);
            if(dot(g, g) < 1e-10f) continue;
            g = normalize(g);
            if(dot(g, bandNormal[s][i]) < 0.f) g = Vec3{ -g.x, -g.y, -g.z };
            bandNormal[s][i] = g;
        }
    }

    // Where a point on the body would be on screen for the given axis values,
    // each -1 to 1, placed as toWorld places it for the live ones.
    auto poseToScreen = [&](const Vec3& local, float st, float su, float sw, float tw, float ro, float pi) noexcept {
        const float so = st * StrokeTravel;
        const float pivot = so + vulvaY;
        Vec3 p{ local.x, local.y + so - pivot, local.z };
        p = rotateY(p, tw * (float)M_PI);
        p = rotateX(p, pi * MaxTiltRadians);
        p = rotateZ(p, ro * MaxTiltRadians);
        p.y += pivot;
        p = add(p, Vec3{ sw * LateralTravel, 0.f, su * LateralTravel });
        return project(camera(p), center, scale);
    };

    // A floor under the rod's foot and a wall behind the model, fixed in the
    // world. Against them it can be read which way the model leans, slides
    // and turns, and where the camera is looking from. The camera looks in
    // from -z, so that is the front, and the wall stands at +z.
    const float planeFloorY = rodBaseY - RodFootHeight;
    constexpr float PlaneHalf = 0.90f;
    const float planeWallTop = HalfLength + StrokeTravel + 0.15f;
    auto planePoint = [&](float x, float y, float z) noexcept {
        return project(camera(Vec3{ x, y, z }), center, scale);
    };
    {
        constexpr float Step = 0.15f;
        const int32_t across = (int32_t)std::round((2.f * PlaneHalf) / Step);
        const int32_t up = (int32_t)std::floor((planeWallTop - planeFloorY) / Step);
        const ImU32 planeFill = IM_COL32(10, 10, 14, (int)(80.f * state.GlobalOpacity));
        const ImU32 gridLine = GetColor(state.Text, state.GlobalOpacity * 0.14f);
        const ImU32 edgeLine = GetColor(state.Text, state.GlobalOpacity * 0.30f);
        const float lineThick = Util::Max(1.f, scale * 0.004f);

        // The wall, then the floor over its foot.
        drawList->AddQuadFilled(
            planePoint(-PlaneHalf, planeFloorY, PlaneHalf), planePoint(PlaneHalf, planeFloorY, PlaneHalf),
            planePoint(PlaneHalf, planeWallTop, PlaneHalf), planePoint(-PlaneHalf, planeWallTop, PlaneHalf), planeFill);
        for (int32_t i = 1; i < across; i += 1) {
            const float x = -PlaneHalf + (Step * (float)i);
            drawList->AddLine(planePoint(x, planeFloorY, PlaneHalf), planePoint(x, planeWallTop, PlaneHalf), gridLine, lineThick);
        }
        for (int32_t j = 1; j <= up; j += 1) {
            const float y = planeFloorY + (Step * (float)j);
            drawList->AddLine(planePoint(-PlaneHalf, y, PlaneHalf), planePoint(PlaneHalf, y, PlaneHalf), gridLine, lineThick);
        }
        {
            const ImVec2 wall[4] = {
                planePoint(-PlaneHalf, planeFloorY, PlaneHalf), planePoint(PlaneHalf, planeFloorY, PlaneHalf),
                planePoint(PlaneHalf, planeWallTop, PlaneHalf), planePoint(-PlaneHalf, planeWallTop, PlaneHalf) };
            drawList->AddPolyline(wall, 4, edgeLine, ImDrawFlags_Closed, lineThick);
        }

        drawList->AddQuadFilled(
            planePoint(-PlaneHalf, planeFloorY, -PlaneHalf), planePoint(PlaneHalf, planeFloorY, -PlaneHalf),
            planePoint(PlaneHalf, planeFloorY, PlaneHalf), planePoint(-PlaneHalf, planeFloorY, PlaneHalf), planeFill);
        for (int32_t i = 1; i < across; i += 1) {
            const float v = -PlaneHalf + (Step * (float)i);
            drawList->AddLine(planePoint(v, planeFloorY, -PlaneHalf), planePoint(v, planeFloorY, PlaneHalf), gridLine, lineThick);
            drawList->AddLine(planePoint(-PlaneHalf, planeFloorY, v), planePoint(PlaneHalf, planeFloorY, v), gridLine, lineThick);
        }
        drawList->AddLine(planePoint(-PlaneHalf, planeFloorY, PlaneHalf), planePoint(PlaneHalf, planeFloorY, PlaneHalf), edgeLine, lineThick);
        drawList->AddLine(planePoint(-PlaneHalf, planeFloorY, -PlaneHalf), planePoint(-PlaneHalf, planeFloorY, PlaneHalf), edgeLine, lineThick);
        drawList->AddLine(planePoint(PlaneHalf, planeFloorY, -PlaneHalf), planePoint(PlaneHalf, planeFloorY, PlaneHalf), edgeLine, lineThick);

        // The front: its edge of the floor in the accent colour, and an arrow
        // at the middle of it pointing out towards the front.
        const ImU32 frontColour = GetColor(accent, state.GlobalOpacity * 0.90f);
        drawList->AddLine(planePoint(-PlaneHalf, planeFloorY, -PlaneHalf), planePoint(PlaneHalf, planeFloorY, -PlaneHalf),
            frontColour, lineThick * 2.5f);
        drawList->AddTriangleFilled(
            planePoint(0.f, planeFloorY, -PlaneHalf - 0.16f),
            planePoint(-0.11f, planeFloorY, -PlaneHalf),
            planePoint(0.11f, planeFloorY, -PlaneHalf),
            frontColour);
    }

    // The frame the model is moved and resized by: the largest box it can
    // take up, at every extreme of every axis that has a script, from where
    // the camera is. Fixed, so it does not follow the model around as it
    // moves, and whatever the model does it stays inside.
    {
        float bodyRadius = 0.f;
        float bodyTop = -1e9f;
        float bodyBottom = 1e9f;
        for (const auto& profileRing : Profile) {
            bodyRadius = Util::Max(bodyRadius, profileRing.r * Radius);
            bodyTop = Util::Max(bodyTop, profileRing.y * HalfLength);
            bodyBottom = Util::Min(bodyBottom, profileRing.y * HalfLength);
        }
        auto choices = [&](Axis axis, float out[2]) noexcept -> int32_t {
            if (axes.mapped[(int32_t)axis]) { out[0] = -1.f; out[1] = 1.f; return 2; }
            out[0] = 0.f;
            return 1;
        };
        float stv[2], suv[2], swv[2], rov[2], piv[2];
        const int32_t stn = choices(Axis::Stroke, stv);
        const int32_t sun = choices(Axis::Surge, suv);
        const int32_t swn = choices(Axis::Sway, swv);
        const int32_t ron = choices(Axis::Roll, rov);
        const int32_t pin = choices(Axis::Pitch, piv);
        const float big = std::numeric_limits<float>::max();
        ImVec2 lo(big, big);
        ImVec2 hi(-big, -big);
        constexpr int32_t Around = 12;
        for (int32_t a = 0; a < stn; a += 1)
        for (int32_t b = 0; b < sun; b += 1)
        for (int32_t c = 0; c < swn; c += 1)
        for (int32_t d = 0; d < ron; d += 1)
        for (int32_t e = 0; e < pin; e += 1) {
            for (int32_t k = 0; k < Around; k += 1) {
                const float angle = (2.f * (float)M_PI * (float)k) / (float)Around;
                for (float level : { bodyTop, bodyBottom }) {
                    const ImVec2 at = poseToScreen(Vec3{ SDL_cosf(angle) * bodyRadius, level, SDL_sinf(angle) * bodyRadius },
                        stv[a], suv[b], swv[c], 0.f, rov[d], piv[e]);
                    lo = ImMin(lo, at);
                    hi = ImMax(hi, at);
                }
            }
        }
        // The rod's foot, which does not move.
        for (int32_t k = 0; k < Around; k += 1) {
            const float angle = (2.f * (float)M_PI * (float)k) / (float)Around;
            const float footRadius = RodRadius * RodFootShape[0][1];
            const ImVec2 at = project(camera(Vec3{ SDL_cosf(angle) * footRadius, rodBaseY - RodFootHeight, SDL_sinf(angle) * footRadius }), center, scale);
            lo = ImMin(lo, at);
            hi = ImMax(hi, at);
        }
        // And the floor and wall around it.
        for (float x : { -PlaneHalf, PlaneHalf }) {
            for (float z : { -PlaneHalf - 0.16f, PlaneHalf }) {
                const ImVec2 at = planePoint(x, planeFloorY, z);
                lo = ImMin(lo, at);
                hi = ImMax(hi, at);
            }
            const ImVec2 top = planePoint(x, planeWallTop, PlaneHalf);
            lo = ImMin(lo, top);
            hi = ImMax(hi, top);
        }
        const float margin = scale * 0.04f;
        ModelDrawnMin = lo - ImVec2(margin, margin);
        ModelDrawnMax = hi + ImVec2(margin, margin);
        ModelDrawnValid = true;
    }

    // Guides to how far each axis can take the model, behind it: for every
    // axis with a script, the path a point on the model follows across the
    // axis's whole range, with a tick at each end. Unlabelled: the ticks say
    // where the limits are, and names over the video were clutter. Stroke only marks its two
    // ends, and twist rings the top of the case with a mark at neutral and at
    // its limit, which for twist is half a turn either way.
    {
        const ImU32 guide = GetColor(accent, state.GlobalOpacity * 0.95f);
        const ImU32 guideFaint = GetColor(accent, state.GlobalOpacity * 0.55f);
        const float thick = Util::Max(1.f, scale * 0.007f);
        const float tick = Util::Max(4.f, scale * 0.045f);
        float caseRadius = 0.f;
        for (const auto& profileRing : Profile) caseRadius = Util::Max(caseRadius, profileRing.r * Radius);
        const float topY = Profile[0].y * HalfLength;
        const float lipY = Profile[SlitRing].y * HalfLength;

        auto tickAt = [&](ImVec2 at, ImVec2 along) noexcept {
            const float length = SDL_sqrtf((along.x * along.x) + (along.y * along.y));
            ImVec2 across = length > 1e-3f ? ImVec2(-along.y / length, along.x / length) : ImVec2(1.f, 0.f);
            drawList->AddLine(at - (across * tick), at + (across * tick), guide, thick * 1.6f);
        };
        auto sweep = [&](const Vec3& local, Axis axis) noexcept {
            if (!axes.mapped[(int32_t)axis]) return;
            constexpr int32_t Steps = 24;
            ImVec2 points[Steps + 1];
            for (int32_t i = 0; i <= Steps; i += 1) {
                float v[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
                v[(int32_t)axis] = -1.f + (2.f * (float)i / (float)Steps);
                points[i] = poseToScreen(local, v[0], v[1], v[2], v[3], v[4], v[5]);
            }
            drawList->AddPolyline(points, Steps + 1, guideFaint, 0, thick);
            tickAt(points[0], points[1] - points[0]);
            tickAt(points[Steps], points[Steps] - points[Steps - 1]);
        };

        if (axes.mapped[(int32_t)Axis::Stroke]) {
            const Vec3 top{ 0.f, topY, 0.f };
            const ImVec2 low = poseToScreen(top, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            const ImVec2 high = poseToScreen(top, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            const ImVec2 flat(caseRadius * scale * 0.9f, 0.f);
            drawList->AddLine(low - flat, low + flat, guide, thick * 1.6f);
            drawList->AddLine(high - flat, high + flat, guide, thick * 1.6f);
        }
        if (axes.mapped[(int32_t)Axis::Twist]) {
            constexpr int32_t RingSteps = 32;
            ImVec2 points[RingSteps];
            const float ringRadius = caseRadius * 1.15f;
            for (int32_t i = 0; i < RingSteps; i += 1) {
                const float angle = (2.f * (float)M_PI * (float)i) / (float)RingSteps;
                points[i] = poseToScreen(Vec3{ SDL_cosf(angle) * ringRadius, topY, SDL_sinf(angle) * ringRadius }, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            }
            drawList->AddPolyline(points, RingSteps, guideFaint, ImDrawFlags_Closed, thick);
            // The front faces -z at neutral, and half a turn takes it to +z.
            const ImVec2 neutralIn = poseToScreen(Vec3{ 0.f, topY, -caseRadius * 1.02f }, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            const ImVec2 neutralOut = poseToScreen(Vec3{ 0.f, topY, -caseRadius * 1.30f }, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            const ImVec2 limitIn = poseToScreen(Vec3{ 0.f, topY, caseRadius * 1.02f }, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            const ImVec2 limitOut = poseToScreen(Vec3{ 0.f, topY, caseRadius * 1.30f }, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
            drawList->AddLine(neutralIn, neutralOut, guideFaint, thick * 1.6f);
            drawList->AddLine(limitIn, limitOut, guide, thick * 1.6f);
        }
        sweep(Vec3{ 0.f, topY, 0.f }, Axis::Roll);
        sweep(Vec3{ 0.f, topY, 0.f }, Axis::Pitch);
        sweep(Vec3{ 0.f, lipY, 0.f }, Axis::Sway);
        sweep(Vec3{ 0.f, lipY, 0.f }, Axis::Surge);
    }

    // Which side of the axis each segment sits on, in view space. The cut is
    // spatial, a wedge taken out of the whole model, not a test on surface
    // normals: cutting on normals would remove the far wall of the canal,
    // which is the surface the cut exists to expose.
    float radialFacing[Segments];
    // Measured in the body's own cross-section: the view direction with its
    // component along the body's axis taken out. Taken against the full view
    // direction, raising or lowering the camera shrank every segment's facing
    // towards nothing, so the wedge cut from the case narrowed while the
    // sleeve and canal inside it were still cut across their whole near half,
    // and the two no longer lined up. Looking straight down the axis there
    // is no cross-section direction to use, so it falls back to the view.
    const Vec3 bodyAxisView = normalize(orient(Vec3{ 0.f, 1.f, 0.f }));
    Vec3 crossView{ viewDir.x - (bodyAxisView.x * dot(viewDir, bodyAxisView)),
                    viewDir.y - (bodyAxisView.y * dot(viewDir, bodyAxisView)),
                    viewDir.z - (bodyAxisView.z * dot(viewDir, bodyAxisView)) };
    crossView = dot(crossView, crossView) > 1e-4f ? normalize(crossView) : viewDir;
    // The case keeps its flanks either side of the cut when seen from the side,
    // but looking along the body those flanks stood out past the sleeve, which
    // is cut across its whole near half, as fins. So the wedge taken from the
    // case opens out to that same half as the view turns along the axis.
    const float alongAxis = std::abs(dot(viewDir, bodyAxisView));
    float cutSteep = Util::Clamp((alongAxis - 0.35f) / 0.50f, 0.f, 1.f);
    cutSteep = cutSteep * cutSteep * (3.f - (2.f * cutSteep));
    const float cutawayCos = CutawayCos * (1.f - cutSteep);
    for(int32_t i = 0; i < Segments; i += 1) {
        radialFacing[i] = dot(normalize(orient(Vec3{ cosA[i], 0.f, sinA[i] })), crossView);
    }

    // ---- collect visible quads, furthest first ------------------------------
    // The profile is not convex, so with the body tilted the base can cover part
    // of the tube. Back faces are skipped because the body is opaque, and what
    // survives is depth sorted.
    struct Quad
    {
        ImVec2 p[4];
        float depth;
        uint32_t color;
        // A colour at each corner, shaded from that corner's own normal, so
        // the fill runs smoothly from one to the next instead of stepping at
        // every band. Rings and bands are fine enough that flat shading
        // mostly passed, but the highlight on the case stepped visibly.
        uint32_t vcol[4];
        // Which nested shell this belongs to: the rod sits inside the sleeve,
        // which sits inside the case. Depth alone cannot separate two surfaces
        // this close together, because with the model tilted a case quad and a
        // sleeve quad covering the same pixels can be at comparable z, and
        // wherever the comparison flips the sleeve is painted over its own
        // shell as an untinted patch. The nesting is known up front, so
        // ordering by it is exact where sorting by z is guesswork.
        int32_t layer;
    };
    // Nesting, innermost outwards. A quad is ordered by which shell it belongs
    // to and whether it is on the near or far side of the axis, which gives a
    // total back to front order: case far, sleeve far, canal far, rod, canal
    // near, sleeve near, case near. Depth alone cannot separate shells this
    // close together, and once the cut exposes interior walls there are more
    // of them competing at similar z, not fewer.
    //
    // Spaced by twos so the rod can sit between two of them: behind the empty
    // canal running away from the cut, and in front of the near wall whose cut
    // edge shows the ribs in profile.
    constexpr int32_t RodLayer = -1;
    constexpr int32_t PartCanal = 2;
    constexpr int32_t PartSleeve = 4;
    constexpr int32_t PartCase = 6;
    // Case bands plus the rod, which is depth sorted along with them so it
    // occludes and is occluded correctly where it enters the slit.
    // The rod is a curve, not a cylinder: it runs straight from its anchored
    // base and bends into line with the canal past the vulva. That bend can
    // only be described by rings along its length, and with only a nose worth
    // of them the whole shaft was one straight segment cutting across the case
    // on its way in.
    constexpr int32_t RodShaftRings = 14;
    // One per entry in the shape table, so the corona and the groove land on
    // rings exactly rather than being averaged across them.
    constexpr int32_t RodNoseRings = RodNoseSamples - 1;
    constexpr int32_t RodBands = RodFootRings + RodShaftRings + RodNoseRings;
    static Quad quads[((BandCount + RodBands + 1) * Segments) + 1 + (FaceRowsMax * FaceColsMax)];
    int32_t quadCount = 0;
    // Height of each ring of the rod along its axis, for the easter egg to
    // match rod bands to canal rings.
    float rodBandY[RodBands + 1] = {};

    for(int32_t s = 0; s < BandCount; s += 1) {
        // Recessed geometry is darkened by hand. A single directional light
        // cannot know that the inside of a bore is shadowed by the walls
        // around it, and without this the orifice lights up like the outside.
        const float ao = Profile[s].ao;

        // The band spanning the join between the case and the sleeve is not a
        // surface, it is the gap between two separate closed parts.
        const bool isSleeve = Profile[s].sleeve;
        if(isSleeve != Profile[s + 1].sleeve) continue;
        // The face is drawn by its own finer grid below.
        if(s >= faceFirstRing && s < faceLastRing) continue;

        // The cut takes the case and the sleeve alike, so the canal and the rod
        // inside it are laid open. Only the rod stays whole.
        const int32_t part = isSleeve
            ? (s >= firstBoreRing ? PartCanal : PartSleeve)
            : PartCase;

        for(int32_t i = 0; i < Segments; i += 1) {
            // A wedge out of the front rather than the whole near half.
            // Removing everything on the near side leaves so little shell that
            // the case stops reading as one; this keeps its flanks and cuts a
            // window between them.
            if(cutaway && radialFacing[i] > cutawayCos) continue;

            // Outside the window the case is intact and opaque, so nothing
            // nested inside it can be seen on the near side. Dropping those is
            // free: with the cut disabling back face culling, the sleeve would
            // otherwise be drawn in full and then covered up.
            //
            // The canal is a partial exception. The strip of its near wall
            // that runs up to the edge of the cut is in front of the rod, and
            // draws the canal's own edge around it -- without it the rod's
            // head sits against the far wall with nothing of the bore in
            // front of it. Only that strip, though: now the bore is round
            // rather than a flattened slit, the rest of the near wall is a
            // whole half-tube lying across the very thing the window was cut
            // to show.
            constexpr float CanalEdgeBand = 0.45f;
            const bool canalEdge = (part == PartCanal)
                && (radialFacing[i] > cutawayCos - CanalEdgeBand);
            if(cutaway && radialFacing[i] > 0.f
                && part != PartCase && !canalEdge) continue;

            Vec3 normal = bandNormal[s][i];
            const float facing = dot(normal, viewDir);
            bool interior = false;
            if(cutaway) {
                // Nothing is culled inside the cut model: an interior wall is
                // exactly what should show. Surfaces turned away from the
                // camera are being seen from behind, so they light by the
                // opposite normal.
                if(facing < 0.f) {
                    normal = Vec3{ -normal.x, -normal.y, -normal.z };
                    interior = true;
                }
            }
            else {
                // Otherwise back faces are dropped as usual. This matters most
                // inside the canal: its near wall faces away from the camera,
                // and without culling the depth sort paints it last, straight
                // over the far wall that should show through the vulva.
                if(facing <= 0.f) continue;
            }
            const int32_t j = (i + 1) % Segments;
            // The stripe only belongs on the outer walls; running it through
            // the canal would light up the inside of the vulva.
            // No stripe down the body any more: it read as a rendering fault.
            // Twist is shown by the triangle over the closed end instead.
            const bool seam = false;

            Quad& q = quads[quadCount];
            q.p[0] = ring[s][i];
            q.p[1] = ring[s][j];
            q.p[2] = ring[s + 1][j];
            q.p[3] = ring[s + 1][i];
            q.depth = (ringDepth[s][i] + ringDepth[s][j]
                + ringDepth[s + 1][i] + ringDepth[s + 1][j]) * 0.25f;
            // The lid of the case is a closed surface over the bulb, so the
            // whole of it goes over the fluid, not just its near half: with
            // the bulb reaching up under it, a drop clinging to the ceiling
            // overlapped the far half of the lid on screen and was painted on
            // top of it, as if it had come out through the case.
            const bool caseLid = !isSleeve && Profile[s].y > 0.95f;
            q.layer = (radialFacing[i] > 0.f || caseLid) ? part : -part;
            // Each corner is shaded from its own normal, the average of the
            // bands meeting there, so the fill runs smoothly across bands.
            // The inside of a shell sits in its own shadow, so it is darkened
            // beyond what a single directional light would give it.
            const int32_t cornerRing[4] = { s, s, s + 1, s + 1 };
            const int32_t cornerCol[4] = { i, j, j, i };
            for(int32_t c = 0; c < 4; c += 1) {
                const int32_t r = cornerRing[c];
                const int32_t col = cornerCol[c];
                Vec3 vn{ 0.f, 0.f, 0.f };
                if(r > 0 && Profile[r - 1].sleeve == Profile[r].sleeve) vn = add(vn, bandNormal[r - 1][col]);
                if(r < BandCount && Profile[r].sleeve == Profile[r + 1].sleeve) vn = add(vn, bandNormal[r][col]);
                vn = normalize(vn);
                if(interior) vn = Vec3{ -vn.x, -vn.y, -vn.z };
                const float cornerAo = interior ? Profile[r].ao * 0.66f : Profile[r].ao;
                // Stretched over the rod the lips go paler and glossy, as
                // soft material pulled thin does.
                float stretch = 0.f;
                ImColor base = isSleeve ? sleeveColor : caseColor;
                if(isSleeve && r < firstBoreRing + 4 && Profile[r].y < -0.806f) {
                    const float fromVulva = std::abs(Profile[r].y - Profile[SlitRing].y) * HalfLength;
                    // The vulva is pinker than the body of the sleeve, and the
                    // inner lips deeper still.
                    const float vulva = Util::Clamp(1.f - (fromVulva / 0.20f), 0.f, 1.f);
                    const float inner = Util::Clamp(1.f - (fromVulva / 0.022f), 0.f, 1.f);
                    base.Value.x += (1.00f - base.Value.x) * 0.40f * vulva;
                    base.Value.y += (0.58f - base.Value.y) * 0.40f * vulva;
                    base.Value.z += (0.72f - base.Value.z) * 0.40f * vulva;
                    base.Value.x += (0.86f - base.Value.x) * 0.55f * inner;
                    base.Value.y += (0.30f - base.Value.y) * 0.55f * inner;
                    base.Value.z += (0.48f - base.Value.z) * 0.55f * inner;
                    // Deeper in the cleft between the labia.
                    const float cleft = vulvaShade[r][col];
                    base.Value.x += (0.62f - base.Value.x) * 0.60f * cleft;
                    base.Value.y += (0.24f - base.Value.y) * 0.60f * cleft;
                    base.Value.z += (0.32f - base.Value.z) * 0.60f * cleft;
                    stretch = insertion * Util::Clamp(1.f - (fromVulva / 0.14f), 0.f, 1.f);
                    // Stretched material blushes a little; it does not go pale
                    // and thin, it is bulging, not stretching thin.
                    base.Value.x += (1.f - base.Value.x) * 0.12f * stretch;
                    base.Value.y += (0.70f - base.Value.y) * 0.12f * stretch;
                    base.Value.z += (0.80f - base.Value.z) * 0.12f * stretch;
                }
                uint32_t vc = shadeFace(vn, cornerAo, seam, base, 1.f, isSleeve);
                // Everything inside is wet: the canal, the cervix and the uterus
                // all carry a sheen, and what the finish lays down adds to it.
                if(part == PartCanal) vc = wetten(vc, 0.35f, vn);
                if(stretch > 0.01f) vc = wetten(vc, 0.15f * stretch, vn);
                if(part == PartCanal && r < (int32_t)finishCanalCoat.size() && finishCanalCoat[r] > 0.01f) {
                    vc = wetten(vc, filmAt(finishCanalCoat[r], col, (float)r), vn);
                }
                q.vcol[c] = vc;
            }
            q.color = q.vcol[0];
            quadCount += 1;

            // The highlight ring: a thin strip on the ring where the opening
            // has come round, as geometry sorted with everything else, so the
            // rod and the walls hide it as they would. Drawn over the top it
            // floated on the shaft.
            if(state.ShowRim && s == rimRing) {
                Quad& h = quads[quadCount];
                const float w = 0.18f;
                h.p[0] = ring[s][i];
                h.p[1] = ring[s][j];
                h.p[2] = ImVec2(ring[s][j].x + ((ring[s + 1][j].x - ring[s][j].x) * w), ring[s][j].y + ((ring[s + 1][j].y - ring[s][j].y) * w));
                h.p[3] = ImVec2(ring[s][i].x + ((ring[s + 1][i].x - ring[s][i].x) * w), ring[s][i].y + ((ring[s + 1][i].y - ring[s][i].y) * w));
                h.depth = q.depth - 0.002f;
                h.layer = q.layer;
                ImColor edge = accent;
                edge.Value.x = Util::Clamp(edge.Value.x * 1.45f, 0.f, 1.f);
                edge.Value.y = Util::Clamp(edge.Value.y * 1.45f, 0.f, 1.f);
                edge.Value.z = Util::Clamp(edge.Value.z * 1.45f, 0.f, 1.f);
                edge.Value.w = state.GlobalOpacity * 0.9f;
                h.color = ImGui::ColorConvertFloat4ToU32(edge.Value);
                for(int32_t c = 0; c < 4; c += 1) h.vcol[c] = h.color;
                quadCount += 1;
            }
        }
    }


    // ---- the face ----------------------------------------------------------
    // A finer grid over the rings of the face, placed by interpolating their
    // vertices, so it takes every deformation the rings do -- the rod opening
    // the slit, the swell, being laid onto the base -- and then carved outward
    // along the body's axis by the traced height map.
    {
        const int32_t bands = faceLastRing - faceFirstRing;
        const int32_t rows = (bands * FaceSubRadial) + 1;
        const int32_t cols = Segments * FaceSubAround;
        static Vec3 facePos[FaceRowsMax][FaceColsMax];
        static Vec3 faceRef[FaceRowsMax][FaceColsMax];
        static ImVec2 faceScreen[FaceRowsMax][FaceColsMax];
        static float faceCavity[FaceRowsMax][FaceColsMax];
        static uint32_t faceColour[FaceRowsMax][FaceColsMax];
        static float faceColFacing[FaceColsMax];
        const Vec3 axisOut = normalize(orient(Vec3{ 0.f, -1.f, 0.f }));
        // How far out along the body's axis the crest of the lip stands. The
        // carving never takes the face past it: the hood and the ends of the
        // labia beside the slit otherwise stood out beyond the lip, and seen
        // side on they hung below it like drips.
        float lipLevel = -std::numeric_limits<float>::max();
        for(int32_t i = 0; i < Segments; i += 1) lipLevel = Util::Max(lipLevel, dot(bodyPos[SlitRing + 1][i], axisOut));
        // Pressed flatter as the rod opens it, and flatter still seated on the
        // base.
        const float press = (1.f - (0.6f * insertion)) * (1.f - lipSeat);
        auto mix = [](const Vec3& a, const Vec3& b, float t) noexcept {
            return Vec3{ a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t) };
        };
        for(int32_t c = 0; c < cols; c += 1) {
            const float angle = (2.f * (float)M_PI * (float)c) / (float)cols;
            faceColFacing[c] = dot(normalize(orient(Vec3{ SDL_cosf(angle), 0.f, SDL_sinf(angle) })), crossView);
        }
        for(int32_t r = 0; r < rows && bands > 0; r += 1) {
            const int32_t b = Util::Min(r / FaceSubRadial, bands - 1);
            const float t = ((float)r / (float)FaceSubRadial) - (float)b;
            const int32_t s0 = faceFirstRing + b;
            const int32_t s1 = s0 + 1;
            // The carving fades out at the cap's rim and at the lip turning
            // inward, where the grid joins the rest of the sleeve.
            const float along = (float)r / (float)(rows - 1);
            float fadeIn = Util::Clamp(along / 0.10f, 0.f, 1.f);
            float fadeOut = Util::Clamp((1.f - along) / 0.16f, 0.f, 1.f);
            fadeIn = fadeIn * fadeIn * (3.f - (2.f * fadeIn));
            fadeOut = fadeOut * fadeOut * (3.f - (2.f * fadeOut));
            // Squashed rings are the lip itself, rolled into the slit. Their
            // vertices crowd together across the slit, so read through the map
            // they alternated between its ridges and the slit into points around
            // the opening, and left uncarved they were a flat plate. They take a
            // smooth rolled rim along the slit instead, and the map only on the
            // face.
            const float sxHere = Profile[s0].sx + ((Profile[s1].sx - Profile[s0].sx) * t);
            const float mapShare = Util::Clamp((sxHere - 0.2f) / 0.8f, 0.f, 1.f);
            const float weight = fadeIn * fadeOut;
            for(int32_t c = 0; c < cols; c += 1) {
                const int32_t i0 = c / FaceSubAround;
                const int32_t i1 = (i0 + 1) % Segments;
                const float a = ((float)c / (float)FaceSubAround) - (float)i0;
                const Vec3 p = mix(mix(bodyPos[s0][i0], bodyPos[s0][i1], a), mix(bodyPos[s1][i0], bodyPos[s1][i1], a), t);
                const float u0 = faceU[s0][i0] + ((faceU[s0][i1] - faceU[s0][i0]) * a);
                const float u1 = faceU[s1][i0] + ((faceU[s1][i1] - faceU[s1][i0]) * a);
                const float v0 = faceV[s0][i0] + ((faceV[s0][i1] - faceV[s0][i0]) * a);
                const float v1 = faceV[s1][i0] + ((faceV[s1][i1] - faceV[s1][i0]) * a);
                const float u = u0 + ((u1 - u0) * t);
                const float v = v0 + ((v1 - v0) * t);
                const float mapped = VulvaMapLow + ((VulvaMapHigh - VulvaMapLow) * sampleMap(VulvaHeightMap, u, v));
                // The rim rolls fullest beside the slit and eases off towards its
                // ends, in the same units as the map.
                const float aroundAngle = (2.f * (float)M_PI * (float)c) / (float)cols;
                const float towardEnd = SDL_sinf(aroundAngle) * SDL_sinf(aroundAngle);
                const float rolled = 0.55f * (1.f - (0.5f * towardEnd));
                const float h = (mapped * mapShare) + (rolled * (1.f - mapShare));
                float lift = h * FaceRelief * weight * press;
                // Past the crest the carving eases off rather than stopping
                // dead: capped hard, everything that reached the crest was cut
                // off level, and the lip read as a flat plate.
                const float headroom = Util::Max(0.f, lipLevel - dot(p, axisOut));
                if(lift > headroom) {
                    constexpr float Ease = 0.022f;
                    lift = headroom + (Ease * (1.f - SDL_expf(-(lift - headroom) / Ease)));
                }
                facePos[r][c] = Vec3{ p.x + (axisOut.x * lift), p.y + (axisOut.y * lift), p.z + (axisOut.z * lift) };
                faceScreen[r][c] = project(facePos[r][c], center, scale);
                faceCavity[r][c] = sampleMap(VulvaCavityMap, u, v) * weight * mapShare;
                // Which way is out, from the rings' own band normal, to turn
                // the grid's normals the right way.
                faceRef[r][c] = mix(bandNormal[s0][i0], bandNormal[s0][i1], a);
            }
        }
        // Smooth normals from the carved grid, and a colour at each vertex.
        for(int32_t r = 0; r < rows && bands > 0; r += 1) {
            const int32_t b = Util::Min(r / FaceSubRadial, bands - 1);
            const float t = ((float)r / (float)FaceSubRadial) - (float)b;
            const float py = Profile[faceFirstRing + b].y
                + ((Profile[faceFirstRing + b + 1].y - Profile[faceFirstRing + b].y) * t);
            const float fromVulva = std::abs(py - Profile[SlitRing].y) * HalfLength;
            const float vulva = Util::Clamp(1.f - (fromVulva / 0.20f), 0.f, 1.f);
            const float inner = Util::Clamp(1.f - (fromVulva / 0.022f), 0.f, 1.f);
            const float stretch = insertion * Util::Clamp(1.f - (fromVulva / 0.14f), 0.f, 1.f);
            const int32_t rUp = Util::Max(0, r - 1);
            const int32_t rDown = Util::Min(rows - 1, r + 1);
            for(int32_t c = 0; c < cols; c += 1) {
                const int32_t cl = (c + cols - 1) % cols;
                const int32_t cr = (c + 1) % cols;
                const Vec3 round{ facePos[r][cr].x - facePos[r][cl].x, facePos[r][cr].y - facePos[r][cl].y, facePos[r][cr].z - facePos[r][cl].z };
                const Vec3 down{ facePos[rDown][c].x - facePos[rUp][c].x, facePos[rDown][c].y - facePos[rUp][c].y, facePos[rDown][c].z - facePos[rUp][c].z };
                Vec3 n = cross(round, down);
                if(dot(n, n) < 1e-12f) n = faceRef[r][c];
                n = normalize(n);
                if(dot(n, faceRef[r][c]) < 0.f) n = Vec3{ -n.x, -n.y, -n.z };
                faceRef[r][c] = n;

                ImColor base = sleeveColor;
                base.Value.x += (1.00f - base.Value.x) * 0.40f * vulva;
                base.Value.y += (0.58f - base.Value.y) * 0.40f * vulva;
                base.Value.z += (0.72f - base.Value.z) * 0.40f * vulva;
                base.Value.x += (0.86f - base.Value.x) * 0.55f * inner;
                base.Value.y += (0.30f - base.Value.y) * 0.55f * inner;
                base.Value.z += (0.48f - base.Value.z) * 0.55f * inner;
                // Deeper in the creases and the slit.
                const float cavity = faceCavity[r][c];
                base.Value.x += (0.62f - base.Value.x) * 0.60f * cavity;
                base.Value.y += (0.24f - base.Value.y) * 0.60f * cavity;
                base.Value.z += (0.32f - base.Value.z) * 0.60f * cavity;
                base.Value.x += (1.f - base.Value.x) * 0.12f * stretch;
                base.Value.y += (0.70f - base.Value.y) * 0.12f * stretch;
                base.Value.z += (0.80f - base.Value.z) * 0.12f * stretch;
                uint32_t vc = shadeFace(n, 1.f - (0.45f * cavity), false, base, 1.f, true);
                if(stretch > 0.01f) vc = wetten(vc, 0.15f * stretch, n);
                faceColour[r][c] = vc;
            }
        }
        for(int32_t r = 0; r + 1 < rows && bands > 0; r += 1) {
            for(int32_t c = 0; c < cols; c += 1) {
                const float colFacing = faceColFacing[c];
                // The same wedge as the rings: in the cutaway nothing of the
                // sleeve on the near side is drawn.
                if(cutaway && colFacing > 0.f) continue;
                const int32_t c1 = (c + 1) % cols;
                // Whether it faces the camera is decided exactly as for the ring
                // quad it lies on, by that quad's own band normal. The carved
                // normals are only for shading: culling on them, or on a blend
                // of neighbouring bands, dropped quads the rings would show,
                // leaving slivers of background through the face, and kept
                // quads they would drop, leaving fins hanging off the lip.
                const Vec3& n = bandNormal[faceFirstRing + Util::Min(r / FaceSubRadial, bands - 1)][c / FaceSubAround];
                const bool interior = dot(n, viewDir) <= 0.f;
                if(interior && !cutaway) continue;
                Quad& q = quads[quadCount];
                q.p[0] = faceScreen[r][c];
                q.p[1] = faceScreen[r][c1];
                q.p[2] = faceScreen[r + 1][c1];
                q.p[3] = faceScreen[r + 1][c];
                q.depth = (facePos[r][c].z + facePos[r][c1].z + facePos[r + 1][c].z + facePos[r + 1][c1].z) * 0.25f;
                q.layer = colFacing > 0.f ? PartSleeve : -PartSleeve;
                if(interior) {
                    // Seen from behind through the cut: lit from the other side.
                    const int32_t rr[4] = { r, r, r + 1, r + 1 };
                    const int32_t cc[4] = { c, c1, c1, c };
                    for(int32_t k = 0; k < 4; k += 1) {
                        const Vec3& vn = faceRef[rr[k]][cc[k]];
                        q.vcol[k] = shadeFace(Vec3{ -vn.x, -vn.y, -vn.z }, 0.66f, false, sleeveColor, 1.f, true);
                    }
                }
                else {
                    q.vcol[0] = faceColour[r][c];
                    q.vcol[1] = faceColour[r][c1];
                    q.vcol[2] = faceColour[r + 1][c1];
                    q.vcol[3] = faceColour[r + 1][c];
                }
                q.color = q.vcol[0];
                quadCount += 1;
            }
        }
    }

    // The foot of the rod on screen, which the axis readout hangs under.
    ImVec2 rodFootAnchor = center;
    float rodFootLowest = center.y;
    // ---- the rod ------------------------------------------------------------
    // Fixed in the body's frame, so it tilts with the case but does not stroke
    // with it. Rounded at the tip, otherwise a flat disc shows through the slit.
    {
        // Anchored at its base in world space, so the body tilts and slides
        // around it rather than carrying it along. Sampled evenly up the shaft
        // so the bend through the vulva is actually resolved, then a short
        // rounded nose.
        float rodProfile[RodBands + 1][2];
        // The foot sits below the base; its last sample is the base itself,
        // which the shaft starts from.
        for(int32_t i = 0; i < RodFootRings; i += 1) {
            rodProfile[i][0] = rodBaseY - (RodFootHeight * (1.f - RodFootShape[i][0]));
            // The same shape the sleeve seats on, groove and bulge included.
            rodProfile[i][1] = footRadiusAt(rodProfile[i][0]);
        }
        for(int32_t i = 0; i <= RodShaftRings; i += 1) {
            const float t = (float)i / (float)RodShaftRings;
            rodProfile[RodFootRings + i][0] = rodBaseY + ((RodLength - RodNoseLength) * t);
            // Thicker where a pulse is running up it.
            rodProfile[RodFootRings + i][1] = RodRadius * rodThrobScale(rodProfile[RodFootRings + i][0]);
        }
        // Nose rings, running from the shaft up to the tip and reading their
        // radius from the same shape the canal is fitted to.
        for(int32_t i = 1; i <= RodNoseRings; i += 1) {
            const float* sample = RodNoseShape[RodNoseSamples - 1 - i];
            const int32_t at = RodFootRings + RodShaftRings + i;
            rodProfile[at][0] = rodTipY - (RodNoseLength * sample[0]);
            rodProfile[at][1] = RodRadius * sample[1] * rodThrobScale(rodProfile[at][0]);
        }
        for(int32_t s = 0; s <= RodBands; s += 1) rodBandY[s] = rodProfile[s][0];
        // Its own colour, from the settings. The head is a shade redder than
        // the shaft.
        ImColor rodColor = state.RodColor;
        rodColor.Value.w = 1.f;
        ImColor headColor = rodColor;
        headColor.Value.x = Util::Min(1.f, headColor.Value.x * 1.08f);
        headColor.Value.y *= 0.86f;
        headColor.Value.z *= 0.90f;
        // The buried length is in the sleeve's shadow. Only a little -- the cut
        // lets plenty of light in -- but enough that inside and outside read as
        // different places without the rod going murky.
        constexpr float RodShadeInside = 0.72f;

        ImVec2 rodRing[RodBands + 1][Segments];
        float rodDepth[RodBands + 1][Segments];
        // Each vertex in view space, so the normals can be taken from the
        // surface as built, with everything that shapes it around the
        // circumference, rather than from the profile alone.
        static Vec3 rodPos[RodBands + 1][Segments];
        Vec3 rodU[RodBands + 1];
        Vec3 rodV[RodBands + 1];
        Vec3 rodT[RodBands + 1];
        for(int32_t s = 0; s <= RodBands; s += 1) {
            const float ry = rodProfile[s][0];
            // Same centreline the canal was deformed against, so the two agree
            // on where the rod is.
            const Vec3 centre = rodCentreAt(ry);
            rodFrameAt(ry, rodU[s], rodV[s]);
            rodT[s] = rodTangentAt(ry);
            const float rad = rodProfile[s][1];
            // The meatus is cut into the apex: over the top few rings the
            // surface is pulled in and down along the pitch plane (the rod's
            // u axis runs front to back), a slit across the very tip that
            // runs a little way down the front.
            const float below = rodTipY - rodProfile[s][0];
            const float grooveReach = RodNoseLength * 0.16f;
            const float grooveHere = (s >= RodFootRings + RodShaftRings && below < grooveReach)
                ? 1.f - (below / grooveReach) : 0.f;

            // The head is not a body of revolution. The corona sits higher at
            // the back than at the front, so it sweeps down the front towards
            // the frenulum, where it is notched; it also stands a little
            // wider at the back. And a soft ridge runs down the front of the
            // shaft, the urethra under the skin. The front is -u.
            const float noseAlong = Util::Clamp(1.f - (below / RodNoseLength), 0.f, 1.f);
            const bool onNose = s >= RodFootRings + RodShaftRings;
            // Where the corona and sulcus are along the nose, 0 to 1 from tip to
            // shaft: a bell around the corona's own position in the shape table.
            const float coronaAt = SDL_expf(-squared((noseAlong - 0.36f) / 0.13f));
            const bool onShaft = s >= RodFootRings && s < RodFootRings + RodShaftRings;

            for(int32_t i = 0; i < Segments; i += 1) {
                float r = rad;
                float sink = 0.f;
                const float front = -cosA[i];
                if(grooveHere > 0.f) {
                    // Narrow across the slit, and only on the front once past
                    // the apex ring.
                    // Wider and deeper while a spurt is coming out of it.
                    const float across = sinA[i] / (0.28f + (0.22f * meatusDilate));
                    float g = SDL_expf(-across * across) * grooveHere;
                    if(below > 0.f) g *= (cosA[i] > 0.f) ? 0.15f : 0.45f;
                    r *= 1.f - ((0.30f + (0.25f * meatusDilate)) * g);
                    sink = RodRadius * (0.09f + (0.10f * meatusDilate)) * g;
                }
                if(onNose && coronaAt > 0.001f) {
                    // Down the shaft towards the front, up towards the back.
                    sink += RodRadius * 0.30f * coronaAt * (0.5f * front);
                    // Wider at the back, and notched at the front where the
                    // frenulum draws the ridge in.
                    const float notch = SDL_expf(-squared(sinA[i] / 0.32f)) * Util::Max(0.f, front);
                    r *= 1.f + (0.05f * coronaAt * Util::Max(0.f, -front)) - (0.07f * coronaAt * notch);
                }
                if(onShaft || (onNose && noseAlong > 0.7f)) {
                    const float ridge = SDL_expf(-squared(sinA[i] / 0.42f)) * Util::Max(0.f, front);
                    r *= 1.f + (0.06f * ridge);
                }
                const float cu = cosA[i] * r;
                const float sv = sinA[i] * r;
                const Vec3 w = toView(Vec3{
                    centre.x + (rodU[s].x * cu) + (rodV[s].x * sv) - (rodT[s].x * sink),
                    centre.y + (rodU[s].y * cu) + (rodV[s].y * sv) - (rodT[s].y * sink),
                    centre.z + (rodU[s].z * cu) + (rodV[s].z * sv) - (rodT[s].z * sink) });
                rodRing[s][i] = project(w, center, scale);
                rodDepth[s][i] = w.z;
                rodPos[s][i] = w;
            }
        }
        {
            ImVec2 sum(0.f, 0.f);
            float lowest = -std::numeric_limits<float>::max();
            for(int32_t i = 0; i < Segments; i += 1) {
                sum.x += rodRing[0][i].x;
                sum.y += rodRing[0][i].y;
            }
            for(int32_t s = 0; s <= RodFootRings && s <= RodBands; s += 1) {
                for(int32_t i = 0; i < Segments; i += 1) lowest = Util::Max(lowest, rodRing[s][i].y);
            }
            rodFootAnchor = ImVec2(sum.x / (float)Segments, sum.y / (float)Segments);
            rodFootLowest = lowest;
        }

        // The normal of each face, from the surface as built, so the meatus,
        // the corona's sweep and the ridge all shade as what they are. The
        // cross of the face's two edges, turned to point away from the axis.
        // Then each corner takes the average of the faces meeting there, so
        // the fill runs smoothly up the shaft.
        static Vec3 rodBandN[RodBands][Segments];
        for(int32_t s = 0; s < RodBands; s += 1) {
            for(int32_t i = 0; i < Segments; i += 1) {
                const int32_t j = (i + 1) % Segments;
                const Vec3& a = rodPos[s][i];
                const Vec3 up{ rodPos[s + 1][i].x - a.x, rodPos[s + 1][i].y - a.y, rodPos[s + 1][i].z - a.z };
                const Vec3 round{ rodPos[s][j].x - a.x, rodPos[s][j].y - a.y, rodPos[s][j].z - a.z };
                Vec3 n = normalize(cross(round, up));
                // Outward is away from the band's own axis.
                const Vec3 axisAt = toView(rodCentreAt(rodProfile[s][0]));
                const Vec3 out{ a.x - axisAt.x, a.y - axisAt.y, a.z - axisAt.z };
                if(dot(n, out) < 0.f) n = Vec3{ -n.x, -n.y, -n.z };
                if(dot(n, n) < 1e-6f) {
                    // A degenerate face at the very tip: fall back to the axis.
                    n = normalize(toView(rodT[s]));
                }
                rodBandN[s][i] = n;
            }
        }
        auto rodVertexNormal = [&](int32_t ring, int32_t col) noexcept -> Vec3 {
            Vec3 n{ 0.f, 0.f, 0.f };
            if(ring > 0) n = add(n, rodBandN[ring - 1][col]);
            if(ring < RodBands) n = add(n, rodBandN[ring][col]);
            return normalize(n);
        };
        // Hidden, the rod still shapes the sleeve; it is only not drawn.
        for(int32_t s = 0; state.ShowRod && s < RodBands; s += 1) {
            for(int32_t i = 0; i < Segments; i += 1) {
                const Vec3 n = rodBandN[s][i];
                if(dot(n, viewDir) <= 0.f) continue;
                const int32_t j = (i + 1) % Segments;

                Quad& q = quads[quadCount];
                q.p[0] = rodRing[s][i];
                q.p[1] = rodRing[s][j];
                q.p[2] = rodRing[s + 1][j];
                q.p[3] = rodRing[s + 1][i];
                q.depth = (rodDepth[s][i] + rodDepth[s][j]
                    + rodDepth[s + 1][i] + rodDepth[s + 1][j]) * 0.25f;
                q.layer = RodLayer;
                const int32_t cornerRing[4] = { s, s, s + 1, s + 1 };
                const int32_t cornerCol[4] = { i, j, j, i };
                for(int32_t c = 0; c < 4; c += 1) {
                    const int32_t r = cornerRing[c];
                    const int32_t col = cornerCol[c];
                    const Vec3 vn = rodVertexNormal(r, col);
                    const float depthIn = Util::Clamp((rodProfile[r][0] - vulvaWorldY) / 0.10f, 0.f, 1.f);
                    const float lit = 1.f - ((1.f - RodShadeInside) * depthIn);
                    // Shaft colour up to the sulcus, the head's from there on.
                    const float head = Util::Clamp(
                        ((float)(r - (RodFootRings + RodShaftRings)) - 1.f) / 3.f, 0.f, 1.f);
                    ImColor base = rodColor;
                    base.Value.x += (headColor.Value.x - rodColor.Value.x) * head;
                    base.Value.y += (headColor.Value.y - rodColor.Value.y) * head;
                    base.Value.z += (headColor.Value.z - rodColor.Value.z) * head;
                    uint32_t vc = shadeFace(vn, lit, false, base, 1.f, false);
                    if(r < (int32_t)finishRodCoat.size() && finishRodCoat[r] > 0.01f) {
                        vc = wetten(vc, filmAt(finishRodCoat[r], col, (float)r), vn);
                    }
                    q.vcol[c] = vc;
                }
                q.color = q.vcol[0];
                quadCount += 1;
            }
        }

        // The meatus: a dark slit lying in the groove, from just behind the
        // apex over it and a little way down the front, drawn as one more
        // quad sorted just in front of the tip. The front is the rod's -u.
        {
            const Vec3 tipCentre = rodCentreAt(rodTipY);
            const Vec3 axis = rodTangentAt(rodTipY);
            Vec3 u, v;
            rodFrameAt(rodTipY, u, v);
            const Vec3 front{ -u.x, -u.y, -u.z };
            const bool apexSeen = dot(normalize(toView(axis)), viewDir) > 0.15f;
            const bool frontSeen = dot(normalize(toView(front)), viewDir) > -0.1f;
            if(state.ShowRod && (apexSeen || frontSeen)) {
                const float width = RodRadius * (0.045f + (0.11f * meatusDilate));
                // From a little behind the apex, over it, and down the front
                // following the dome.
                auto at = [&](float back, float down, float across) noexcept {
                    return Vec3{ tipCentre.x + (front.x * back) - (axis.x * down) + (v.x * across),
                                 tipCentre.y + (front.y * back) - (axis.y * down) + (v.y * across),
                                 tipCentre.z + (front.z * back) - (axis.z * down) + (v.z * across) };
                };
                Quad& q = quads[quadCount];
                const Vec3 corners[4] = {
                    at(-RodRadius * 0.16f, 0.f, -width), at(-RodRadius * 0.16f, 0.f, width),
                    at(RodRadius * 0.58f, RodRadius * 0.22f, width), at(RodRadius * 0.58f, RodRadius * 0.22f, -width) };
                float depth = 0.f;
                for(int32_t c = 0; c < 4; c += 1) {
                    const Vec3 w = toView(corners[c]);
                    q.p[c] = project(w, center, scale);
                    depth += w.z * 0.25f;
                }
                q.depth = depth - 0.01f;
                q.layer = RodLayer;
                ImColor slit = headColor;
                slit.Value.x *= 0.45f;
                slit.Value.y *= 0.40f;
                slit.Value.z *= 0.40f;
                slit.Value.w = state.GlobalOpacity * 0.9f;
                q.color = ImGui::ColorConvertFloat4ToU32(slit.Value);
                for(int32_t c = 0; c < 4; c += 1) q.vcol[c] = q.color;
                quadCount += 1;
            }
        }
    }


    // Furthest first. The ribbed profile pushes this past a thousand quads, far
    // enough that the insertion sort this used to do became the expensive part
    // of the frame.
    // As large as the quads themselves: it was a ring short, and the rim and
    // meatus quads could write past its end.
    static int32_t order[((BandCount + RodBands + 1) * Segments) + 1 + (FaceRowsMax * FaceColsMax)];
    for(int32_t i = 0; i < quadCount; i += 1) order[i] = i;
    // Innermost shell first, and within a shell furthest first.
    std::sort(order, order + quadCount, [](int32_t a, int32_t b) noexcept {
        if(quads[a].layer != quads[b].layer) return quads[a].layer < quads[b].layer;
        return quads[a].depth > quads[b].depth;
    });
    // With the cutaway, the near walls wait until the fluid of the easter egg
    // has been drawn, so it sits inside the canal with the cut edge of the
    // near wall, ribs and all, in front of it. Drawn after everything, it lay
    // over the ridges as if it were stuck to the outside of the sleeve.
    int32_t nearQuadsStart = 0;
    // A quad filled with a colour at each corner, written straight into the
    // draw list as two triangles: the renderer blends the corners across the
    // face, which is the smooth shading. AddConvexPolyFilled takes one colour.
    auto fillQuad = [&](const Quad& q) noexcept {
        drawList->PrimReserve(6, 4);
        const ImVec2 uv = drawList->_Data->TexUvWhitePixel;
        const ImDrawIdx base = (ImDrawIdx)drawList->_VtxCurrentIdx;
        drawList->PrimWriteVtx(q.p[0], uv, q.vcol[0]);
        drawList->PrimWriteVtx(q.p[1], uv, q.vcol[1]);
        drawList->PrimWriteVtx(q.p[2], uv, q.vcol[2]);
        drawList->PrimWriteVtx(q.p[3], uv, q.vcol[3]);
        drawList->PrimWriteIdx(base);
        drawList->PrimWriteIdx((ImDrawIdx)(base + 1));
        drawList->PrimWriteIdx((ImDrawIdx)(base + 2));
        drawList->PrimWriteIdx(base);
        drawList->PrimWriteIdx((ImDrawIdx)(base + 2));
        drawList->PrimWriteIdx((ImDrawIdx)(base + 3));
    };
    {
        // Without this every quad feathers its own edge, and two quads sharing
        // an edge each cover half of the pixels along it, so the background
        // shows through as a hairline. On a surface made of a thousand quads
        // that is a wireframe drawn over the whole model, and on the canal,
        // where the quads are slivers and mostly edge, it swamps the fill and
        // the colour goes uneven. The seams between them are not real edges
        // and should not be drawn.
        const ImDrawListFlags savedFlags = drawList->Flags;
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        // The face of the cut through the sleeve, drawn solid so the canal,
        // the cervix and the uterus read as hollows in the flesh rather than
        // as shells floating in front of the far wall. It goes down after the
        // far half of the sleeve's outside and before its inside: the walls of
        // every hollow then draw back over it, so looking through the cut one
        // sees into each hollow and nowhere else. The sleeve is turned, so the
        // cut through it at each edge of the removed half is its outline, and
        // the solid part of that is everything between the axis and the outer
        // wall, filled a strip at a time.
        auto drawSection = [&]() noexcept {
            int32_t sleeveFirstRing = RingCount;
            for(int32_t k = 0; k < RingCount; k += 1) {
                if(Profile[k].sleeve) { sleeveFirstRing = k; break; }
            }
            if(sleeveFirstRing >= firstBoreRing) return;
            const uint32_t flesh = shadeFace(viewDir, 0.82f, false, sleeveColor, 1.f, true);
            const int32_t quarter = Segments / 4;
            for(int32_t i = 0; i < Segments; i += 1) {
                const int32_t j = (i + 1) % Segments;
                if((radialFacing[i] > 0.f) == (radialFacing[j] > 0.f)) continue;
                // Where the cut falls between these two columns.
                const float t = radialFacing[i] / (radialFacing[i] - radialFacing[j]);
                ImVec2 lastAxis;
                ImVec2 lastEdge;
                bool haveLast = false;
                for(int32_t k = sleeveFirstRing; k < firstBoreRing; k += 1) {
                    const Vec3& a = bodyPos[k][i];
                    const Vec3& b = bodyPos[k][j];
                    const Vec3 edge{ a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t) };
                    const Vec3& c0 = bodyPos[k][0];
                    const Vec3& c1 = bodyPos[k][quarter];
                    const Vec3& c2 = bodyPos[k][2 * quarter];
                    const Vec3& c3 = bodyPos[k][3 * quarter];
                    const Vec3 axis{ (c0.x + c1.x + c2.x + c3.x) * 0.25f,
                                     (c0.y + c1.y + c2.y + c3.y) * 0.25f,
                                     (c0.z + c1.z + c2.z + c3.z) * 0.25f };
                    const ImVec2 edgeScreen = project(edge, center, scale);
                    const ImVec2 axisScreen = project(axis, center, scale);
                    if(haveLast) {
                        Quad strip{};
                        strip.p[0] = lastAxis;
                        strip.p[1] = lastEdge;
                        strip.p[2] = edgeScreen;
                        strip.p[3] = axisScreen;
                        for(int32_t c = 0; c < 4; c += 1) strip.vcol[c] = flesh;
                        strip.color = flesh;
                        fillQuad(strip);
                    }
                    lastAxis = axisScreen;
                    lastEdge = edgeScreen;
                    haveLast = true;
                }
            }
        };
        // The cervix is solid too, hanging in the hollow of the vault, but the
        // walls of that hollow draw over the section above and the far half of
        // the cervix with them, which left it reading as more hollow. So once
        // the hollows are down its own section goes over them: from its neck
        // at the roof down to its opening, between the axis and its outside,
        // with the narrow cervical canal left open up the middle of it.
        auto drawCervixSection = [&]() noexcept {
            if(cervixFirstRing >= RingCount) return;
            // Its opening: where the profile turns back up the cervical canal.
            int32_t osRing = cervixFirstRing;
            while(osRing + 1 < RingCount && Profile[osRing + 1].y < Profile[osRing].y) osRing += 1;
            int32_t canalTop = osRing;
            while(canalTop + 1 < RingCount && Profile[canalTop + 1].y < ReservoirFloorY) canalTop += 1;
            const uint32_t flesh = shadeFace(viewDir, 0.82f, false, sleeveColor, 1.f, true);
            const uint32_t hollow = shadeFace(viewDir, 0.12f, false, sleeveColor, 1.f, true);
            const int32_t sectionFirst = Util::Max(firstBoreRing, cervixFirstRing - 2);
            const int32_t quarter = Segments / 4;
            auto axisAt = [&](int32_t k) noexcept {
                const Vec3& c0 = bodyPos[k][0];
                const Vec3& c1 = bodyPos[k][quarter];
                const Vec3& c2 = bodyPos[k][2 * quarter];
                const Vec3& c3 = bodyPos[k][3 * quarter];
                return project(Vec3{ (c0.x + c1.x + c2.x + c3.x) * 0.25f,
                                     (c0.y + c1.y + c2.y + c3.y) * 0.25f,
                                     (c0.z + c1.z + c2.z + c3.z) * 0.25f }, center, scale);
            };
            auto strip = [&](ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, uint32_t colour) noexcept {
                Quad q{};
                q.p[0] = a;
                q.p[1] = b;
                q.p[2] = c;
                q.p[3] = d;
                for(int32_t n = 0; n < 4; n += 1) q.vcol[n] = colour;
                q.color = colour;
                fillQuad(q);
            };
            // Where the cut falls between two columns, on each ring of a run.
            ImVec2 sides[2][RingCount];
            int32_t sideCount = 0;
            for(int32_t i = 0; i < Segments && sideCount < 2; i += 1) {
                const int32_t j = (i + 1) % Segments;
                if((radialFacing[i] > 0.f) == (radialFacing[j] > 0.f)) continue;
                const float t = radialFacing[i] / (radialFacing[i] - radialFacing[j]);
                for(int32_t k = sectionFirst; k <= canalTop; k += 1) {
                    const Vec3& a = bodyPos[k][i];
                    const Vec3& b = bodyPos[k][j];
                    sides[sideCount][k] = project(Vec3{ a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t) }, center, scale);
                }
                sideCount += 1;
            }
            for(int32_t side = 0; side < sideCount; side += 1) {
                // From the roof it hangs from, over the fold where the roof turns
                // down onto it, so it is joined to the flesh above rather than
                // cut off from it by the far side of that fold.
                for(int32_t k = sectionFirst; k < osRing; k += 1) {
                    strip(axisAt(k), sides[side][k], sides[side][k + 1], axisAt(k + 1), flesh);
                }
            }
            if(sideCount == 2) {
                for(int32_t k = osRing; k < canalTop; k += 1) {
                    strip(sides[0][k], sides[1][k], sides[1][k + 1], sides[0][k + 1], hollow);
                }
            }
        };
        bool sectionDrawn = !cutaway;
        bool cervixSectionDrawn = !cutaway;
        for(; nearQuadsStart < quadCount; nearQuadsStart += 1) {
            const Quad& q = quads[order[nearQuadsStart]];
            if(!sectionDrawn && q.layer > -PartSleeve) {
                drawSection();
                sectionDrawn = true;
            }
            if(!cervixSectionDrawn && q.layer > -PartCanal) {
                drawCervixSection();
                cervixSectionDrawn = true;
            }
            if(cutaway && q.layer > 0) break;
            fillQuad(q);
        }
        if(!sectionDrawn) drawSection();
        if(!cervixSectionDrawn) drawCervixSection();
        drawList->Flags = savedFlags;
    }

    // ---- easter egg: finishing ---------------------------------------------
    // Stimulation fills with how fast the rod moves while it is inside and
    // drains on its own, so only sustained hard, fast stroking gets there.
    // Past the top the rod pulses a few times, and each pulse sends fluid up
    // the canal from the tip. It gathers at the closed end of the sleeve, is
    // pushed ahead of the rod when the rod comes up under it, and fades.
    if(FinishEasterEgg) {
        const float dt = Util::Clamp(ImGui::GetIO().DeltaTime, 1.f / 480.f, 0.1f);
        if(finishCanalCoat.size() != (size_t)RingCount) finishCanalCoat.assign(RingCount, 0.f);
        if(finishRodCoat.size() != (size_t)(RodBands + 1)) finishRodCoat.assign(RodBands + 1, 0.f);
        const float rodSpeed = finishLastInsertion >= 0.f ? std::abs(insertion - finishLastInsertion) / dt : 0.f;
        if(finishTwitchTime >= 0.f) {
            finishTwitchTime += dt;
            if(finishTwitchTime > 1.5f) finishTwitchTime = -1.f;
        }
        auto twitch = [&](float strength) noexcept {
            finishTwitchTime = 0.f;
            finishTwitchStrength = strength;
        };

        // Tuned so a full stroke twice a second, which moves the insertion by
        // about four a second, finishes in roughly eight seconds, while one
        // stroke every two seconds never fills faster than it drains.
        constexpr float StimulationGain = 0.07f;
        constexpr float StimulationDrain = 0.15f;
        constexpr float EngagedInsertion = 0.15f;
        if(finishLastInsertion >= 0.f && finishClock < 0.f) {
            const float speed = std::abs(insertion - finishLastInsertion) / dt;
            const float fill = insertion > EngagedInsertion ? speed * StimulationGain : 0.f;
            FinishStimulation = Util::Clamp(FinishStimulation + ((fill - StimulationDrain) * dt), 0.f, 1.f);
        }
        finishLastInsertion = insertion;

        // Close to finishing the rod trembles now and then, harder the closer
        // it is; nothing until it is most of the way there.
        const float tremble = finishClock >= 0.f ? 0.f : Util::Max(0.f, (FinishStimulation - 0.7f) / 0.3f);
        finishTremorTimer -= dt;
        if(tremble > 0.f && finishTremorTimer <= 0.f) {
            finishTremorTimer = 0.25f + (0.5f * Util::NextFloat());
            twitch(0.25f + (0.35f * tremble));
        }

        if((FinishStimulation >= 1.f || FinishRequested) && finishClock < 0.f) {
            FinishRequested = false;
            finishClock = 0.f;
            finishPulsesFired = 0;
            FinishStimulation = 0.f;
            FinishCount += 1;
        }

        // Positions along the body's axis, before the tilt. The rod stands
        // outside the body's frame, but through the canal it runs close enough
        // to the axis that its tip can be taken as sitting on it.
        const float tipLocalY = rodTipY - strokeOffset;
        const float ceilingLocalY = [&]() noexcept {
            float ringY = 0.f;
            float ringR = 0.f;
            uterusRing(RingCount - 1, uterusShape, ringY, ringR);
            return ringY * HalfLength;
        }();
        const float canalRadius = CanalRestRadius * 0.85f;

        // The contractions, shared with the rod's throb (FinishPulseTimes).
        // Each one twitches the rod, and each from FinishFirstSpurt on lets a
        // rope out.
        const float* PulseTimes = FinishPulseTimes;
        const float* PulseStrength = FinishPulseStrength;
        constexpr int32_t PulseCount = FinishPulseCount;
        // Each pulse lets out a rope rather than a spray: a string of nodes
        // released one after another from the tip over a fraction of a second,
        // the first thrown furthest, so it stretches out up the canal in one
        // piece instead of arriving as loose drops.
        constexpr float RopeEmitSeconds = 0.26f;
        constexpr int32_t RopeNodes = 24;
        // Scale: the case is about eight inches tall, which makes one model
        // unit near five inches. The first spurt leaves at about eleven miles
        // an hour, and the later, weaker ones at a fraction of that; gravity
        // is the real thing in these units. So the strong ones cross the canal
        // in a blink and hit the closed end, and the weak ones run out of
        // speed short of the opening and slide back down.
        constexpr float MaxLaunchSpeed = 40.f;
        constexpr float Gravity = 80.f;
        // Thickest a little behind the head, tapering off to a tail.
        auto ropeNodeSize = [](float along, float strength) noexcept {
            return (0.55f + (0.75f * SDL_sinf((0.25f + (0.75f * along)) * 3.1416f)))
                * (0.55f + (0.45f * strength));
        };

        // The pile. Every node ends up in it, so the back of the canal fills
        // from the closed end towards the vulva and grows with each spurt,
        // where nodes that simply stuck where they landed all landed on the
        // same spot and piled up on top of each other without ever adding up.
        const float pileRadius = RodRadius * 0.45f;
        // It collects in the reservoir, filling it from the closed end down.
        // Its size at each height is the chamber's at rest, read from the
        // profile rather than from this frame's rings: those are opened up
        // around the rod, and a pile copying them flared out towards the tip
        // into a cone.
        struct ReservoirSlice
        {
            float rx = 0.f, rz = 0.f;
        };
        auto reservoirAtShape = [&](float y, float distension) noexcept {
            ReservoirSlice c;
            for(int32_t s = firstBoreRing; s + 1 < RingCount; s += 1) {
                float p0 = Profile[s].y;
                float r0 = Profile[s].r;
                float p1 = Profile[s + 1].y;
                float r1 = Profile[s + 1].r;
                if(s >= uterusFirstRing) uterusRing(s, distension, p0, r0);
                if(s + 1 >= uterusFirstRing) uterusRing(s + 1, distension, p1, r1);
                const float y0 = p0 * HalfLength;
                const float y1 = p1 * HalfLength;
                if(y1 > y0 && y >= y0 && y <= y1) {
                    const float f = (y - y0) / (y1 - y0);
                    const float r = (r0 + ((r1 - r0) * f)) * Radius;
                    c.rx = r * (Profile[s].sx + ((Profile[s + 1].sx - Profile[s].sx) * f));
                    c.rz = r * (ringSquashZ[s] + ((ringSquashZ[s + 1] - ringSquashZ[s]) * f));
                    return c;
                }
            }
            return c;
        };
        auto reservoirAt = [&](float y) noexcept { return reservoirAtShape(y, uterusShape); };
        // How much the uterus holds at a distension, from where it opens out up
        // to the middle of its top.
        auto capacityAt = [&](float distension) noexcept {
            constexpr int32_t Steps = 20;
            float topY = 0.f;
            float topR = 0.f;
            uterusRing(RingCount - 1, distension, topY, topR);
            const float bottom = ReservoirFloorY * HalfLength;
            const float height = Util::Max(0.01f, (topY * HalfLength) - bottom);
            float volume = 0.f;
            for(int32_t k = 0; k < Steps; k += 1) {
                const ReservoirSlice c = reservoirAtShape(bottom + (height * (((float)k + 0.5f) / (float)Steps)), distension);
                volume += (float)M_PI * c.rx * c.rz * (height / (float)Steps);
            }
            return volume;
        };
        const float reservoirBottomY = ReservoirFloorY * HalfLength;
        const float reservoirHeight = Util::Max(0.01f, ceilingLocalY - reservoirBottomY);
        // Volume from the bottom of the reservoir up to each step, so the level
        // of a pool of any volume is a lookup. It pools at the bottom, by the
        // neck, and rises towards the closed end.
        constexpr int32_t VolumeSteps = 48;
        float volumeBelow[VolumeSteps + 1];
        volumeBelow[0] = 0.f;
        for(int32_t k = 1; k <= VolumeSteps; k += 1) {
            const float step = reservoirHeight / (float)VolumeSteps;
            const ReservoirSlice c = reservoirAt(reservoirBottomY + (step * ((float)k - 0.5f)));
            volumeBelow[k] = volumeBelow[k - 1] + ((float)M_PI * c.rx * c.rz * step);
        }
        const float pileCapacity = Util::Max(1e-6f, volumeBelow[VolumeSteps]);
        // At its most distended the uterus holds what this many finishes let
        // out. Its capacity scales with its width squared and its height.
        constexpr float UterusFullFinishes = 10.f;
        const float uterusFullCapacity = Util::Max(1e-6f, capacityAt(1.f));
        auto pileFront = [&]() noexcept {
            if(FinishPileVolume <= 0.f) return reservoirBottomY;
            for(int32_t k = 1; k <= VolumeSteps; k += 1) {
                if(volumeBelow[k] >= FinishPileVolume) {
                    const float span = Util::Max(1e-9f, volumeBelow[k] - volumeBelow[k - 1]);
                    const float f = (FinishPileVolume - volumeBelow[k - 1]) / span;
                    return reservoirBottomY + (reservoirHeight * (((float)(k - 1) + f) / (float)VolumeSteps));
                }
            }
            return ceilingLocalY;
        };

        if(finishClock >= 0.f) {
            finishClock += dt;
            while(finishPulsesFired < PulseCount && finishClock >= PulseTimes[finishPulsesFired]) {
                if(finishPulsesFired == 0) {
                    // Worth of one node, so the finish as a whole adds its share
                    // of the canal whatever the rope sizes come to.
                    float totalSizeSquared = 0.f;
                    for(int32_t pulse = FinishFirstSpurt; pulse < PulseCount; pulse += 1) {
                        for(int32_t k = 0; k < RopeNodes; k += 1) {
                            const float size = ropeNodeSize((float)k / (float)(RopeNodes - 1), PulseStrength[pulse]);
                            totalSizeSquared += size * size;
                        }
                    }
                    finishNodeVolume = (uterusFullCapacity / UterusFullFinishes) / Util::Max(1e-3f, totalSizeSquared);
                }
                // Every contraction comes with a twitch.
                twitch(0.6f + (0.4f * PulseStrength[finishPulsesFired]));
                if(finishPulsesFired >= FinishFirstSpurt) {
                    finishEmitStrength = PulseStrength[finishPulsesFired];
                    finishEmitIndex = 0;
                    finishEmitTimer = 0.f;
                    finishRopeId += 1;
                    finishRopeWobble = Util::NextFloat() * 6.2832f;
                }
                else {
                    // Dry: nothing to let out.
                    finishEmitStrength = 0.f;
                    finishEmitIndex = RopeNodes;
                }
                finishPulsesFired += 1;
            }
            if(finishPulsesFired >= PulseCount && finishEmitIndex >= RopeNodes
                && finishClock > PulseTimes[PulseCount - 1] + 1.f) {
                finishClock = -1.f;
                }
        }
        // The opening at the tip dilates while a rope is coming out of it, as
        // flesh: a damped spring towards open while it is, and closed after.
        {
            const float want = (finishEmitStrength > 0.f && finishEmitIndex < RopeNodes)
                ? 0.6f + (0.4f * finishEmitStrength) : 0.f;
            meatusDilateVel = (meatusDilateVel + ((220.f * (want - meatusDilate)) * dt))
                / (1.f + (14.f * dt) + (220.f * dt * dt));
            meatusDilate = Util::Clamp(meatusDilate + (meatusDilateVel * dt), 0.f, 1.2f);
            if(!std::isfinite(meatusDilate) || !std::isfinite(meatusDilateVel)) {
                meatusDilate = 0.f;
                meatusDilateVel = 0.f;
            }
        }
        if(finishEmitStrength > 0.f && finishEmitIndex < RopeNodes) {
            finishEmitTimer += dt;
            const float interval = RopeEmitSeconds / (float)RopeNodes;
            while(finishEmitIndex < RopeNodes && finishEmitTimer >= interval * (float)finishEmitIndex) {
                const float along = (float)finishEmitIndex / (float)(RopeNodes - 1);
                FinishDrop node;
                node.rope = finishRopeId;
                node.y = tipLocalY;
                // Where the tip is decides how far it gets. Outside the vulva
                // most of its speed is spent getting in; partway up the canal it
                // flies as far as its strength takes it; bottomed out against
                // the cervix it goes straight through the opening into the
                // uterus, and all of it lands there.
                const float toCervix = (CervixTipY * HalfLength) - tipLocalY;
                const bool kissing = toCervix < 0.02f;
                const bool atLip = tipLocalY < vulvaY + 0.03f;
                // A slow wander about the axis, continuous along the rope, so
                // it curls as it goes rather than jittering node to node.
                const float wobble = finishRopeWobble + (along * 2.5f);
                // From the opening at the tip itself, not beside it.
                node.x = SDL_cosf(wobble) * RodRadius * 0.04f;
                node.z = SDL_sinf(wobble) * RodRadius * 0.04f;
                {
                    const float launch = MaxLaunchSpeed
                        * (0.15f + (0.85f * finishEmitStrength * finishEmitStrength));
                    // The head of the rope is the fastest, the tail lags.
                    if(kissing) {
                        // Straight from the tip through the opening of the cervix
                        // and up its canal into the uterus, where it can be seen
                        // going; even the weakest spurt makes it through.
                        node.vy = Util::Max(launch * 0.45f, 7.f) * (1.f - (0.45f * along));
                    }
                    else {
                        // Short of the cervix it only rises as far as the room
                        // above the tip, and falls back onto it: at the lip
                        // hardly into the canal at all.
                        const float apex = atLip ? 0.05f : Util::Clamp(toCervix - 0.03f, 0.03f, 0.16f);
                        node.vy = SDL_sqrtf(2.f * Gravity * apex)
                            * (0.6f + (0.4f * finishEmitStrength)) * (1.f - (0.4f * along));
                    }
                }
                node.vx = SDL_cosf(wobble) * (kissing ? 0.35f : 0.10f);
                node.vz = SDL_sinf(wobble) * (kissing ? 0.35f : 0.10f);
                node.size = ropeNodeSize(along, finishEmitStrength);
                FinishDrops.push_back(node);
                finishEmitIndex += 1;
                // The tip is wet from the moment anything leaves it.
                for(int32_t b = RodFootRings + RodShaftRings; b <= RodBands && b < (int32_t)finishRodCoat.size(); b += 1) {
                    finishRodCoat[b] = 1.f;
                }
            }
        }

        // Motion. A rope shoots up the canal, slowing but never stopping short,
        // into the reservoir and against its closed end. There each node clings
        // a moment, spattered across it, then lets go and falls to the pool at
        // the bottom of the reservoir, adding its volume as it lands.
        constexpr float DropSlowing = 1.9f;
        // Something this thick slides down a wall slowly, and it takes a real
        // shove from the tip to throw it back up.
        constexpr float SlideSpeed = 1.8f;
        constexpr float ThrustLaunch = 2.5f;
        constexpr float AbsorbSeconds = 0.45f;
constexpr float FallAcceleration = 4.5f;
        constexpr float MaxFallSpeed = 2.4f;
        constexpr size_t MaxDrops = 900;
        const float surface = pileFront();
        const float clingY = ceilingLocalY - 0.012f;
        const float openingY = CervixTipY * HalfLength;
        // How fast the tip is moving up the canal, for what it throws ahead.
        const float tipVel = finishLastTipY > -1e8f ? (tipLocalY - finishLastTipY) / dt : 0.f;
        finishLastTipY = tipLocalY;
        for(auto& drop : FinishDrops) {
            if(drop.rope == -2) {
                // A drop running down the shaft outside the vulva, in the rod's
                // own frame: slowly, as something thick does, until it reaches
                // the foot or thins out.
                drop.vy = Util::Max(-0.35f, drop.vy - (0.5f * dt));
                drop.y += drop.vy * dt;
                drop.life -= dt / 3.5f;
                if(drop.y < rodBaseY) drop.life = 0.f;
                continue;
            }
            if(drop.rope == -3) {
                // Overflow running down the wall of the canal to the vulva,
                // wetting it on the way, then dripping from the lip.
                drop.y -= 0.45f * dt;
                if(drop.y <= vulvaY) {
                    drop.life = 0.f;
                }
                continue;
            }
            if(drop.rope < 0) {
                // Running out of the vulva once the reservoir can take no more.
                drop.y += drop.vy * dt;
                drop.vy -= 0.8f * dt;
                drop.life -= dt / 1.4f;
                continue;
            }
            switch(drop.phase) {
                case 0: {
                    drop.y += drop.vy * dt;
                    drop.x += drop.vx * dt;
                    drop.z += drop.vz * dt;
                    drop.vy = (drop.vy - (Gravity * dt)) * SDL_expf(-DropSlowing * dt);
                    drop.vx *= SDL_expf(-DropSlowing * dt);
                    drop.vz *= SDL_expf(-DropSlowing * dt);
                    if(drop.vy <= 0.f && drop.y < clingY) {
                        // Spent. Inside the bulb it drops to the pool; short
                        // of the opening it falls back down the canal.
                        drop.phase = drop.y > openingY ? 2 : 4;
                        drop.vy = 0.f;
                        break;
                    }
                    if(drop.y >= clingY) {
                    drop.y = clingY;
                        drop.phase = 1;
                        drop.timer = 0.10f + (0.35f * Util::NextFloat());
                        const ReservoirSlice c = reservoirAt(clingY);
                        const float angle = Util::NextFloat() * 6.2832f;
                        const float spread = SDL_sqrtf(Util::NextFloat()) * 0.8f;
                        drop.x = SDL_cosf(angle) * c.rx * spread;
                        drop.z = SDL_sinf(angle) * c.rz * spread;
                        drop.vx = 0.f;
                        drop.vy = 0.f;
                        drop.vz = 0.f;
                    }
                    // Pushed ahead of the rod rather than passing through it.
                    else if(drop.y < tipLocalY && tipLocalY - drop.y < RodNoseLength) {
                        drop.y = std::min(tipLocalY, ceilingLocalY);
                    }
                    break;
                }
                case 1:
                    drop.timer -= dt;
                    if(drop.timer <= 0.f) {
                        drop.phase = 2;
                        drop.vy = 0.f;
                    }
                    break;
                case 2:
                    drop.vy = Util::Max(-MaxFallSpeed, drop.vy - (FallAcceleration * dt));
                    drop.y += drop.vy * dt;
                    if(drop.y <= surface) {
                        drop.y = surface;
                        drop.phase = 3;
                        FinishPileVolume += finishNodeVolume * drop.size * drop.size;
                        finishPileHeave += 0.6f * drop.size;
                    }
                    break;
                case 4:
                    // Sliding back down the wall of the canal until it meets
                    // the tip of the rod, or the vulva if the rod is out.
                    drop.vy = Util::Max(-SlideSpeed, drop.vy - (Gravity * 0.15f * dt));
                    drop.y += drop.vy * dt;
                    if(drop.y <= tipLocalY + 0.01f && tipLocalY >= vulvaY) {
                        drop.y = tipLocalY + 0.01f;
                        drop.phase = 5;
                        drop.vy = 0.f;
                    }
                    else if(drop.y <= vulvaY) {
                        // Out of the vulva: it drools from the middle of the opening.
                        drop.rope = -1;
                        drop.x = 0.f;
                        drop.z = 0.f;
                        drop.y = vulvaY;
                        drop.vy = -0.12f;
                        drop.life = 1.f;
                    }
                    break;
                case 5:
                    // Come to rest on the tip: it joins the pool the tip holds up
                    // in the canal.
                    FinishCanalVolume += finishNodeVolume * drop.size * drop.size;
                    drop.life = 0.f;
                    break;
                default:
                    // Merging into the pool: it rides the surface as it fades.
                    drop.y = surface;
                    drop.life -= dt / AbsorbSeconds;
                    break;
            }
            // Held inside the canal, or inside the reservoir once up in it.
            const float lateral = SDL_sqrtf((drop.x * drop.x) + (drop.z * drop.z));
            float held = pileRadius * 0.8f;
            if(drop.y > ReservoirStartY * HalfLength) {
                // Through the opening and the bulb it is the wall itself that
                // holds them: narrow through the opening, wide in the bulb.
                held = Util::Max(0.004f, reservoirAt(drop.y).rx * 0.85f);
            }
            if(lateral > held) {
                drop.x *= held / lateral;
                drop.z *= held / lateral;
            }
        }

        // The uterus distends to hold what is in it, easing out after each
        // spurt, up to its most after ten finishes; past that the surplus
        // drains away. The distension that holds the volume is found by
        // halving.
        // The pool the tip holds up in the canal. Bottomed out against the
        // cervix it is pumped through into the uterus; with the tip back at the
        // vulva it drools out of the middle of the opening.
        if(FinishCanalVolume > 0.f) {
            if((CervixTipY * HalfLength) - tipLocalY < 0.02f) {
                const float pumped = Util::Min(FinishCanalVolume, uterusFullCapacity * 0.08f * dt);
                FinishCanalVolume -= pumped;
                FinishPileVolume += pumped;
            }
            else if(tipLocalY < vulvaY + 0.02f) {
                finishDripTimer += dt;
                if(finishDripTimer >= 0.22f) {
                    finishDripTimer = 0.f;
                    FinishCanalVolume = Util::Max(0.f, FinishCanalVolume - (uterusFullCapacity * 0.004f));
                    FinishDrop drool;
                    drool.rope = -1;
                    drool.y = vulvaY;
                    drool.vy = -0.12f;
                    drool.size = 0.8f + (0.4f * Util::NextFloat());
                    FinishDrops.push_back(drool);
                }
            }
        }
        FinishPileVolume = Util::Min(FinishPileVolume, uterusFullCapacity);
        {
            float lo = 0.f;
            float hi = 1.f;
            for(int32_t k = 0; k < 12; k += 1) {
                const float mid = (lo + hi) * 0.5f;
                if(capacityAt(mid) < FinishPileVolume) lo = mid; else hi = mid;
            }
            const float target = FinishPileVolume > 0.f ? hi : 0.f;
            // Swelling as flesh does, with a little give past where it settles.
            uterusDistensionVel = (uterusDistensionVel + ((25.f * (target - UterusDistension)) * dt))
                / (1.f + (6.5f * dt) + (25.f * dt * dt));
            UterusDistension = Util::Clamp(UterusDistension + (uterusDistensionVel * dt), 0.f, 1.08f);
        }
        FinishPileFraction = Util::Clamp(FinishPileVolume / Util::Max(1e-6f, pileCapacity), 0.f, 1.f);
        finishPileHeave *= SDL_expf(-dt * 4.f);

        // ---- slosh ----
        // The pools settle level with the world, so as the body pitches and
        // rolls their surfaces tip across it; the body thrown sideways throws
        // them the other way, and stroking along it makes them bob. Each is a
        // damped spring towards where it would settle, stepped implicitly so a
        // slow frame cannot blow it up.
        {
            const Vec3 down = unrotate(Vec3{ 0.f, -1.f, 0.f });
            float targetX = 0.f;
            float targetZ = 0.f;
            if(down.y < -0.2f) {
                targetX = down.x / -down.y;
                targetZ = down.z / -down.y;
            }
            float strokeAccel = 0.f;
            const float tipAccel = poolMotionPrimed ? (tipVel - poolLastTipVel) / dt : 0.f;
            if(poolMotionPrimed) {
                const float strokeVel = (strokeOffset - poolLastStroke) / dt;
                const float swayVel = (worldOffset.x - poolLastSway) / dt;
                const float surgeVel = (worldOffset.z - poolLastSurge) / dt;
                strokeAccel = (strokeVel - poolLastStrokeVel) / dt;
                const Vec3 across = unrotate(Vec3{ (swayVel - poolLastSwayVel) / dt, 0.f, (surgeVel - poolLastSurgeVel) / dt });
                targetX -= across.x * 0.012f;
                targetZ -= across.z * 0.012f;
                poolLastStrokeVel = strokeVel;
                poolLastSwayVel = swayVel;
                poolLastSurgeVel = surgeVel;
            }
            poolLastStroke = strokeOffset;
            poolLastSway = worldOffset.x;
            poolLastSurge = worldOffset.z;
            poolLastTipVel = tipVel;
            poolMotionPrimed = true;
            targetX = Util::Clamp(targetX, -1.2f, 1.2f);
            targetZ = Util::Clamp(targetZ, -1.2f, 1.2f);

            constexpr float TiltK = 90.f;
            constexpr float TiltDamp = 7.f;
            auto spring = [&](float& x, float& v, float target, float push, float k, float damp) noexcept {
                v = (v + (((k * (target - x)) + push) * dt)) / (1.f + (damp * dt) + (k * dt * dt));
                x += v * dt;
                if(!std::isfinite(x) || !std::isfinite(v)) { x = target; v = 0.f; }
            };
            spring(poolTiltX, poolTiltVelX, targetX, 0.f, TiltK, TiltDamp);
            spring(poolTiltZ, poolTiltVelZ, targetZ, 0.f, TiltK, TiltDamp);
            spring(poolBob, poolBobVel, 0.f, -Util::Clamp(strokeAccel, -400.f, 400.f) * 0.00025f, 140.f, 9.f);
            spring(canalBob, canalBobVel, 0.f, -Util::Clamp(tipAccel, -400.f, 400.f) * 0.00035f, 160.f, 8.f);
            poolBob = Util::Clamp(poolBob, -0.02f, 0.02f);
            canalBob = Util::Clamp(canalBob, -0.025f, 0.025f);
        }

        // ---- coating ----
        // Ropes on their way up wet the wall they pass, and the pool keeps the
        // chamber it stands in wet. The rod, stroking through a wet canal,
        // trades film with the wall as it goes, and what it carries out of the
        // vulva runs down the shaft.
        {
            const float dry = SDL_expf(-dt / 30.f);
            for(float& c : finishCanalCoat) c *= dry;
            for(int32_t s = firstBoreRing; s < RingCount; s += 1) {
                const float localY = Profile[s].y * HalfLength;
                float wet = 0.f;
                for(const auto& drop : FinishDrops) {
                    if(drop.rope == -1 || drop.rope == -2) continue;
                    const float d = (localY - drop.y) / 0.03f;
                    if(d > 3.f || d < -3.f) continue;
                    wet += drop.size * SDL_expf(-d * d);
                }
                // Standing in the pool.
                if(localY >= reservoirBottomY - 0.01f && localY <= surface + 0.005f) wet += 3.f;
                if(wet > 0.f) finishCanalCoat[s] = Util::Min(1.f, finishCanalCoat[s] + (wet * 2.5f * dt));
            }

            // Anything sitting on the tip soaks into the head.
            for(const auto& drop : FinishDrops) {
                if(drop.rope < 0 || drop.phase != 5) continue;
                for(int32_t b = RodFootRings + RodShaftRings; b <= RodBands; b += 1) {
                    finishRodCoat[b] = Util::Min(1.f, finishRodCoat[b] + (drop.size * 0.8f * dt));
                }
            }

            // Faster the faster the rod moves: a rod sitting still in a wet
            // canal wets only slowly, a stroking one is smeared at once.
            const float carry = Util::Min(1.f, (0.15f * dt) + (rodSpeed * 2.f * dt));
            for(int32_t b = 0; b <= RodBands; b += 1) {
                float& rc = finishRodCoat[b];
                const float local = rodBandY[b] - strokeOffset;
                if(local < vulvaY) {
                    // Outside, drying in the air.
                    rc *= SDL_expf(-dt / 45.f);
                    continue;
                }
                int32_t nearest = -1;
                float best = std::numeric_limits<float>::max();
                for(int32_t s = firstBoreRing; s < RingCount; s += 1) {
                    const float d = std::abs((Profile[s].y * HalfLength) - local);
                    if(d < best) { best = d; nearest = s; }
                }
                if(nearest < 0) continue;
                float& wc = finishCanalCoat[nearest];
                // The wetter side gives to the drier. The wall has more to
                // give than the rod can take, so it loses less than the rod gains.
                const float x = (wc - rc) * carry;
                rc = Util::Clamp(rc + x, 0.f, 1.f);
                wc = Util::Clamp(wc - (x * 0.35f), 0.f, 1.f);
            }

            // The pool on the tip, carried up and down the canal, smears onto
            // the shaft inside the canal as the rod strokes through it: a little
            // more with every stroke, more the fuller the pool, and using the
            // pool up as it goes. The trade with the wall above spreads it on.
            if(FinishCanalVolume > 0.f && tipLocalY > vulvaY) {
                const float share = Util::Clamp(FinishCanalVolume / (uterusFullCapacity * 0.03f), 0.f, 1.f);
                const float smear = Util::Min(1.f, rodSpeed * 1.5f * dt) * share;
                const float span = Util::Max(1e-3f, tipLocalY - vulvaY);
                for(int32_t b = 0; b <= RodBands && b < (int32_t)finishRodCoat.size(); b += 1) {
                    const float local = rodBandY[b] - strokeOffset;
                    if(local <= vulvaY || local > tipLocalY) continue;
                    // Wettest near the head, where the pool is.
                    const float nearHead = 0.35f + (0.65f * ((local - vulvaY) / span));
                    finishRodCoat[b] = Util::Min(1.f, finishRodCoat[b] + (smear * nearHead));
                }
                FinishCanalVolume = Util::Max(0.f, FinishCanalVolume - (uterusFullCapacity * 0.002f * smear));
            }
        }

        FinishDrops.erase(std::remove_if(FinishDrops.begin(), FinishDrops.end(),
            [](const FinishDrop& drop) noexcept { return drop.life <= 0.f; }), FinishDrops.end());
        if(FinishDrops.size() > MaxDrops) {
            FinishDrops.erase(FinishDrops.begin(), FinishDrops.begin() + (FinishDrops.size() - MaxDrops));
        }

        // Drawn over the model with the cutaway, which shows the inside of the
        // sleeve. A closed case is opaque, so with it closed only what has
        // come out of the vulva is drawn: drips from the lip and runs down the
        // shaft.
        const float visibility = cutaway ? 1.f : 0.f;
        auto toScreen = [&](float x, float y, float z) noexcept {
            return project(toWorld(Vec3{ x, y + strokeOffset, z }), center, scale);
        };
        auto nodeRadius = [&](const FinishDrop& d) noexcept { return Util::Max(1.2f, scale * 0.022f * d.size); };
        // Where a drop is on screen. Drips on the rod live in the rod's frame,
        // on its surface, at the angle kept in anchorY; everything else is in
        // the body's frame.
        auto rodPoint = [&](float worldY, float angle) noexcept {
            const Vec3 c = rodCentreAt(worldY);
            Vec3 u, v;
            rodFrameAt(worldY, u, v);
            const float ca = SDL_cosf(angle) * RodRadius;
            const float sa = SDL_sinf(angle) * RodRadius;
            return project(toView(Vec3{ c.x + (u.x * ca) + (v.x * sa),
                                        c.y + (u.y * ca) + (v.y * sa),
                                        c.z + (u.z * ca) + (v.z * sa) }), center, scale);
        };
        auto dropScreen = [&](const FinishDrop& d) noexcept {
            if(d.rope == -2) return rodPoint(d.y, d.anchorY);
            return toScreen(d.x, d.y, d.z);
        };
        // A drop running down a surface leaves a run behind it: the tail from
        // where it started, or as far back as a run stays wet, down to where it
        // is now. False for anything that is not running.
        constexpr float RunTail = 0.22f;
        auto runTail = [&](const FinishDrop& d, ImVec2& from, ImVec2& to) noexcept -> bool {
            if(d.rope == -2) {
                const float start = Util::Min(d.z, d.y + RunTail);
                if(start - d.y < 0.01f) return false;
                from = rodPoint(start, d.anchorY);
                to = rodPoint(d.y, d.anchorY);
                return true;
            }
            if(d.rope == -3) {
                const float start = Util::Min(d.anchorY, d.y + RunTail);
                if(start - d.y < 0.01f) return false;
                from = toScreen(d.x, start, d.z);
                to = toScreen(d.x, d.y, d.z);
                return true;
            }
            return false;
        };
        auto nodeAlpha = [&](const FinishDrop& d) noexcept {
            const bool outside = d.rope == -1 || d.rope == -2;
            if(d.rope == -2 && !state.ShowRod) return 0.f;
            return Util::Clamp(d.life, 0.f, 1.f) * (outside ? 1.f : visibility) * state.GlobalOpacity;
        };
        auto colour = [](ImVec4 c, float a) noexcept {
            c.w *= a;
            return ImGui::ColorConvertFloat4ToU32(c);
        };
        auto shaded = [](ImVec4 c, float k) noexcept {
            return ImVec4(c.x * k, c.y * k, c.z * k, c.w);
        };
        // Pearly and a little translucent, rather than flat cream: the body
        // lets some of what is behind it through, the edge is a soft grey
        // rather than a brown outline, and the light catches it as a small
        // sharp highlight over a paler core, with a darker crescent on the
        // side away from the light.
        const ImVec4 bodyColour(0.93f, 0.93f, 0.89f, 0.80f);
        const ImVec4 coreColour(0.99f, 0.99f, 0.97f, 0.45f);
        const ImVec4 shadeColour(0.62f, 0.62f, 0.60f, 0.30f);
        const ImVec4 edgeColour(0.50f, 0.50f, 0.48f, 0.45f);
        const ImVec4 sheenColour(1.f, 1.f, 1.f, 1.f);

        // ---- drawing ----
        // In layers across everything at once, rather than a piece at a time:
        // every outline first, then every body over them, then the sheen. Where
        // ropes and the pile run together, the bodies cover every outline that
        // falls inside the mass, and what shows is one outline around the
        // perimeter of the whole of it. Outlined piece by piece, every node and
        // every slice of the pile carried its own ring.
        constexpr float MaxStretch = 0.90f;
        const float pileAlpha = visibility * state.GlobalOpacity;

        // A rope is drawn as beads: circles laid close together along a curve
        // through its nodes, the radius easing from one node's to the next
        // with a waist between them that narrows as they pull apart. Beads
        // overlap into one smooth strand, where a straight line from node to
        // node met each node at a corner. The strand from node i to the next
        // node of the same rope is handed to bead one circle at a time; none
        // once they are too far apart for one.
        auto strandBeads = [&](size_t i, auto&& bead) noexcept {
            const auto& d = FinishDrops[i];
            if(d.rope < 0 || i + 1 >= FinishDrops.size() || FinishDrops[i + 1].rope != d.rope) return;
            const auto& n = FinishDrops[i + 1];
            const float dx = n.x - d.x;
            const float dy = n.y - d.y;
            const float dz = n.z - d.z;
            const float gap = SDL_sqrtf((dx * dx) + (dy * dy) + (dz * dz));
            if(gap >= MaxStretch) return;
            const float slack = Util::Clamp(1.f - (gap / MaxStretch), 0.15f, 1.f);
            // Only a gentle waist: something this thick stays a strand, not a
            // string of pearls, even stretched.
            const float waist = 0.72f + (0.28f * slack);
            const ImVec2 p1 = dropScreen(d);
            const ImVec2 p2 = dropScreen(n);
            const ImVec2 p0 = (i > 0 && FinishDrops[i - 1].rope == d.rope) ? dropScreen(FinishDrops[i - 1]) : p1;
            const ImVec2 p3 = (i + 2 < FinishDrops.size() && FinishDrops[i + 2].rope == d.rope) ? dropScreen(FinishDrops[i + 2]) : p2;
            const float r1 = nodeRadius(d);
            const float r2 = nodeRadius(n);
            const float a1 = nodeAlpha(d);
            const float a2 = nodeAlpha(n);
            const float len = SDL_sqrtf(((p2.x - p1.x) * (p2.x - p1.x)) + ((p2.y - p1.y) * (p2.y - p1.y)));
            // Beads close enough to overlap however long the strand is drawn;
            // capped at sixteen they separated into a dotted line on a long one.
            const int32_t count = Util::Clamp((int32_t)(len / Util::Max(1.f, Util::Min(r1, r2) * 0.5f)) + 1, 1, 96);
            for(int32_t k = 1; k < count; k += 1) {
                const float t = (float)k / (float)count;
                const float t2 = t * t;
                const float t3 = t2 * t;
                // Catmull-Rom through the neighbouring nodes.
                const ImVec2 p(
                    0.5f * ((2.f * p1.x) + ((p2.x - p0.x) * t)
                        + (((2.f * p0.x) - (5.f * p1.x) + (4.f * p2.x) - p3.x) * t2)
                        + ((-p0.x + (3.f * p1.x) - (3.f * p2.x) + p3.x) * t3)),
                    0.5f * ((2.f * p1.y) + ((p2.y - p0.y) * t)
                        + (((2.f * p0.y) - (5.f * p1.y) + (4.f * p2.y) - p3.y) * t2)
                        + ((-p0.y + (3.f * p1.y) - (3.f * p2.y) + p3.y) * t3)));
                const float r = (r1 + ((r2 - r1) * t)) * (1.f - ((1.f - waist) * SDL_sinf(t * IM_PI)));
                bead(p, r, a1 + ((a2 - a1) * t));
            }
        };
        // The run behind a drip, as beads too: thin where it started, the
        // drop's own size where it is now.
        auto runBeads = [&](const FinishDrop& d, auto&& bead) noexcept {
            ImVec2 from, to;
            if(!runTail(d, from, to)) return;
            const float r = nodeRadius(d);
            const float a = nodeAlpha(d);
            const float len = SDL_sqrtf(((to.x - from.x) * (to.x - from.x)) + ((to.y - from.y) * (to.y - from.y)));
            const int32_t count = Util::Clamp((int32_t)(len / Util::Max(1.f, r * 0.4f)) + 1, 1, 24);
            for(int32_t k = 0; k < count; k += 1) {
                const float t = (float)k / (float)count;
                // Slim and faint where it has thinned out behind the drop,
                // swelling only over the last stretch into the bead itself.
                const float swell = t * t * t;
                bead(ImVec2(from.x + ((to.x - from.x) * t), from.y + ((to.y - from.y) * t)),
                    r * (0.22f + (0.60f * swell)), a * (0.30f + (0.60f * t)));
            }
        };

        auto convexHull = [](std::vector<ImVec2> pts) noexcept {
            std::sort(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b) noexcept {
                return a.x < b.x || (a.x == b.x && a.y < b.y);
            });
            auto turn = [](const ImVec2& o, const ImVec2& a, const ImVec2& b) noexcept {
                return ((a.x - o.x) * (b.y - o.y)) - ((a.y - o.y) * (b.x - o.x));
            };
            std::vector<ImVec2> hull(pts.size() * 2);
            size_t h = 0;
            for(size_t i = 0; i < pts.size(); i += 1) {
                while(h >= 2 && turn(hull[h - 2], hull[h - 1], pts[i]) <= 0.f) h -= 1;
                hull[h++] = pts[i];
            }
            for(size_t i = pts.size() - 1, lower = h + 1; i-- > 0;) {
                while(h >= lower && turn(hull[h - 2], hull[h - 1], pts[i]) <= 0.f) h -= 1;
                hull[h++] = pts[i];
            }
            hull.resize(h > 0 ? h - 1 : 0);
            return hull;
        };
        auto fillShape = [&](const std::vector<ImVec2>& shape, ImU32 c) noexcept {
            if(shape.size() >= 3) drawList->AddConvexPolyFilled(shape.data(), (int)shape.size(), c);
        };

        // The pile: the reservoir filled from its bottom up to the level,
        // as wide as the chamber at each height less a film of wall. The
        // chamber is round and smooth, so the whole of it is one convex shape.
        std::vector<ImVec2> pileBody;
        std::vector<ImVec2> pileOutline;
        std::vector<ImVec2> frontGobs;
        std::vector<ImVec2> sideSheen;
        ImVec2 frontCentre(0.f, 0.f);
        const float gob = Util::Max(1.5f, scale * 0.022f);
        // The gobs on the surface, sized to the chamber there.
        float frontGob = gob;
        if(FinishPileVolume > 0.f) {
            // From the bottom of the reservoir up to the level.
            const float top = Util::Min(surface, ceilingLocalY);
            const float front = reservoirBottomY;
            if(top - front > 0.002f) {
                constexpr int32_t RingPoints = 20;
                constexpr float WallFilm = 0.94f;
                // The surface sloshes: tipped by poolTiltX and poolTiltZ and
                // bobbing by poolBob, level with the world when all is still.
                auto surfaceAt = [&](float x, float z) noexcept {
                    return Util::Clamp(top + poolBob + (poolTiltX * x) + (poolTiltZ * z), front, ceilingLocalY);
                };
                // A little uneven, as something poured rather than turned, and
                // heaving where a rope has just landed, settling after.
                auto lumpAt = [&](float y, float a, float grow) noexcept {
                    const float lump = 0.97f + (0.03f * SDL_sinf((y * 57.f) + (a * 3.f)));
                    const float heave = 1.f + (0.05f * Util::Min(1.f, finishPileHeave)
                        * (0.6f + (0.4f * SDL_sinf((y * 40.f) + (a * 2.f)))));
                    return WallFilm * lump * grow * heave;
                };
                const ReservoirSlice atTop = reservoirAt(top);
                const float reach = (std::abs(poolTiltX) * atTop.rx) + (std::abs(poolTiltZ) * atTop.rz) + std::abs(poolBob);
                const float highest = Util::Min(ceilingLocalY, top + reach);
                const int32_t slices = Util::Clamp((int32_t)SDL_ceilf((highest - front) / 0.012f), 2, 60);
                for(int32_t i = 0; i <= slices; i += 1) {
                    const float y = front + ((highest - front) * (float)i / (float)slices);
                    const ReservoirSlice c = reservoirAt(y);
                    if(c.rx <= 0.f) continue;
                    for(int32_t k = 0; k < RingPoints; k += 1) {
                        const float a = (2.f * (float)M_PI * (float)k) / (float)RingPoints;
                        const float px = SDL_cosf(a) * c.rx;
                        const float pz = SDL_sinf(a) * c.rz;
                        if(y > surfaceAt(px, pz)) continue;
                        const float k1 = lumpAt(y, a, 1.f);
                        const float k2 = lumpAt(y, a, 1.06f);
                        pileBody.push_back(toScreen(px * k1, y, pz * k1));
                        pileOutline.push_back(toScreen(px * k2, y, pz * k2));
                    }
                    if(i % 3 == 1 && y <= surfaceAt(0.f, 0.f)) sideSheen.push_back(toScreen(-c.rx * 0.45f, y, -c.rz * 0.45f));
                }
                // The rim of the surface, at the height it stands at all round.
                for(int32_t k = 0; k < RingPoints; k += 1) {
                    const float a = (2.f * (float)M_PI * (float)k) / (float)RingPoints;
                    float sy = surfaceAt(SDL_cosf(a) * atTop.rx, SDL_sinf(a) * atTop.rz);
                    const ReservoirSlice c = reservoirAt(sy);
                    if(c.rx <= 0.f) continue;
                    sy = surfaceAt(SDL_cosf(a) * c.rx, SDL_sinf(a) * c.rz);
                    const ReservoirSlice rim = reservoirAt(sy);
                    if(rim.rx <= 0.f) continue;
                    const float px = SDL_cosf(a) * rim.rx;
                    const float pz = SDL_sinf(a) * rim.rz;
                    const float k1 = lumpAt(sy, a, 1.f);
                    const float k2 = lumpAt(sy, a, 1.06f);
                    pileBody.push_back(toScreen(px * k1, sy, pz * k1));
                    pileOutline.push_back(toScreen(px * k2, sy, pz * k2));
                    if(k % 2 == 0) frontGobs.push_back(toScreen(px * WallFilm * 0.85f, sy, pz * WallFilm * 0.85f));
                }
                pileBody = convexHull(pileBody);
                pileOutline = convexHull(pileOutline);

                const float midSurface = surfaceAt(0.f, 0.f);
                frontCentre = toScreen(0.f, midSurface, 0.f);
                const ImVec2 side = toScreen(atTop.rx, midSurface, 0.f);
                const float dx = side.x - frontCentre.x;
                const float dy = side.y - frontCentre.y;
                frontGob = Util::Clamp(SDL_sqrtf((dx * dx) + (dy * dy)) * 0.22f, 1.f, gob);
            }
        }
        const bool havePile = pileBody.size() >= 3;

        // The pool held up in the canal on the tip: a round-topped column as
        // wide as the canal opens around it, up to the cervix at the most.
        std::vector<ImVec2> canalBody;
        std::vector<ImVec2> canalOutline;
        if(FinishCanalVolume > 0.f && tipLocalY > vulvaY) {
            // As wide as the canal it stands in, not the rod below it: spread
            // over the rod's width a finish's worth was a film too thin to see.
            const float rc = RodRadius * 0.40f;
            const float room = Util::Max(0.004f, (CervixTipY * HalfLength) - tipLocalY);
            const float h = Util::Clamp(FinishCanalVolume / ((float)M_PI * rc * rc), 0.004f, room);
            constexpr int32_t Around = 16;
            for(int32_t i = 0; i <= 6; i += 1) {
                const float f = (float)i / 6.f;
                const float yy = tipLocalY + 0.004f + (h * f);
                const float over = Util::Max(0.f, (f - 0.6f) / 0.4f);
                const float rr = rc * SDL_sqrtf(Util::Max(0.f, 1.f - (over * over)));
                for(int32_t k = 0; k < Around; k += 1) {
                    const float ang = (2.f * (float)M_PI * (float)k) / (float)Around;
                    const float cx = SDL_cosf(ang) * rr;
                    const float cz = SDL_sinf(ang) * rr;
                    // Its top tips with the pools and wobbles as the rod is
                    // thrust and drawn back, its base riding the tip.
                    const float sy = yy + (((poolTiltX * cx) + (poolTiltZ * cz) + canalBob) * f);
                    canalBody.push_back(toScreen(cx, sy, cz));
                    canalOutline.push_back(toScreen(cx * 1.06f, sy, cz * 1.06f));
                }
            }
            canalBody = convexHull(canalBody);
            canalOutline = convexHull(canalOutline);
        }

        // 1. Every outline, under everything. The pile's outline takes in its
        // gobs, so the edge follows the lumpy front too.
        for(size_t i = 0; i < FinishDrops.size(); i += 1) {
            const auto& d = FinishDrops[i];
            if(nodeAlpha(d) <= 0.f) continue;
            auto edgeBead = [&](ImVec2 p, float r, float a) noexcept {
                drawList->AddCircleFilled(p, r + 0.8f, colour(edgeColour, a), 10);
            };
            strandBeads(i, edgeBead);
            runBeads(d, edgeBead);
            drawList->AddCircleFilled(dropScreen(d), nodeRadius(d) + 0.8f,
                colour(edgeColour, nodeAlpha(d)), 12);
        }
        if(havePile) {
            fillShape(pileOutline, colour(edgeColour, pileAlpha));
            for(const auto& at : frontGobs) drawList->AddCircleFilled(at, frontGob + 1.f, colour(edgeColour, pileAlpha), 10);
            drawList->AddCircleFilled(frontCentre, (frontGob * 1.4f) + 1.f, colour(edgeColour, pileAlpha), 12);
        }

        fillShape(canalOutline, colour(edgeColour, pileAlpha));

        // 2. Every body, solid, so where two overlap they merge instead of
        // darkening into a line.
        if(havePile) {
            fillShape(pileBody, colour(bodyColour, pileAlpha));
            for(const auto& at : frontGobs) drawList->AddCircleFilled(at, frontGob, colour(bodyColour, pileAlpha), 10);
            drawList->AddCircleFilled(frontCentre, frontGob * 1.4f, colour(bodyColour, pileAlpha), 12);
        }
        for(size_t i = 0; i < FinishDrops.size(); i += 1) {
            const auto& d = FinishDrops[i];
            if(nodeAlpha(d) <= 0.f) continue;
            auto bodyBead = [&](ImVec2 p, float r, float a) noexcept {
                drawList->AddCircleFilled(p, r, colour(bodyColour, a), 10);
            };
            strandBeads(i, bodyBead);
            runBeads(d, bodyBead);
            drawList->AddCircleFilled(dropScreen(d), nodeRadius(d), colour(bodyColour, nodeAlpha(d)), 12);
        }
        for(size_t i = 0; i < FinishDrops.size(); i += 1) {
            const auto& d = FinishDrops[i];
            const float a = nodeAlpha(d);
            if(a <= 0.f) continue;
            const ImVec2 at = dropScreen(d);
            const float radius = nodeRadius(d);
            if(radius < 2.f) continue;
            // Round rather than flat: darker away from the light, paler
            // towards it.
            drawList->AddCircleFilled(at + ImVec2(radius * 0.28f, radius * 0.28f), radius * 0.70f,
                colour(shadeColour, a), 12);
            drawList->AddCircleFilled(at - ImVec2(radius * 0.22f, radius * 0.22f), radius * 0.62f,
                colour(coreColour, a), 12);
        }

        fillShape(canalBody, colour(bodyColour, pileAlpha));

        // 3. The sheen.
        if(havePile) {
            drawList->AddCircleFilled(frontCentre - ImVec2(frontGob * 0.5f, frontGob * 0.5f), frontGob * 0.45f,
                colour(sheenColour, pileAlpha * 0.7f), 8);
            for(const auto& at : sideSheen) {
                drawList->AddCircleFilled(at, Util::Max(1.f, gob * 0.35f), colour(sheenColour, pileAlpha * 0.45f), 6);
            }
        }
        for(size_t i = 0; i < FinishDrops.size(); i += 1) {
            const auto& d = FinishDrops[i];
            if(nodeAlpha(d) <= 0.f) continue;
            // The highlight runs along the strand, up and to the left of its
            // centre, as one continuous line of smaller beads.
            auto sheenBead = [&](ImVec2 p, float r, float a) noexcept {
                drawList->AddCircleFilled(p - ImVec2(r * 0.40f, r * 0.40f), Util::Max(0.6f, r * 0.18f),
                    colour(sheenColour, a * 0.30f), 8);
            };
            strandBeads(i, sheenBead);
            runBeads(d, sheenBead);
            const float radius = nodeRadius(d);
            drawList->AddCircleFilled(dropScreen(d) - ImVec2(radius * 0.38f, radius * 0.38f), Util::Max(0.7f, radius * 0.20f),
                colour(sheenColour, nodeAlpha(d) * 0.90f), 8);
        }
    }
    else {
        // Turned off: the canal is emptied, so turning it on again starts clean.
        FinishPileVolume = 0.f;
        UterusDistension = 0.f;
        meatusDilate = 0.f;
        meatusDilateVel = 0.f;
        uterusDistensionVel = 0.f;
        FinishRequested = false;
        FinishCanalVolume = 0.f;
        poolTiltX = poolTiltZ = poolTiltVelX = poolTiltVelZ = 0.f;
        poolBob = poolBobVel = canalBob = canalBobVel = 0.f;
        poolMotionPrimed = false;
        FinishPileFraction = 0.f;
        finishPileHeave = 0.f;
        finishTwitchTime = -1.f;
        finishLastTipY = -1e9f;
        finishCanalCoat.clear();
        finishRodCoat.clear();
    }

    // The near walls, over the fluid. Empty unless the model is cut away.
    {
        const ImDrawListFlags savedFlags = drawList->Flags;
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for(int32_t k = nearQuadsStart; k < quadCount; k += 1) {
            const Quad& q = quads[order[k]];
            fillQuad(q);
        }
        drawList->Flags = savedFlags;
    }

        // Which way the body's front faces, drawn so it reads from any angle.
        // A triangle lying flat on the closed end was seen nearly edge on
        // from a level camera. Instead: an arrow in screen space at the rim of
        // the closed end, pointing the way the front points on screen, with a
        // spoke from the centre, and outlined so it stands out against the
        // case and the video. When the front points straight at the camera or
        // away from it there is no direction on screen to show, so it becomes
        // a dot, filled facing the camera and hollow facing away. Drawn after
        // the model, so the case does not cover it when it lies over the case.
        if(state.ShowTwistIndicator) {
            float caseRadius = 0.f;
            for (const auto& profileRing : Profile) caseRadius = Util::Max(caseRadius, profileRing.r * Radius);
            const float topY = Profile[0].y * HalfLength;
            const float lidY = strokeOffset + topY;
            const ImVec2 hub = project(toWorld(Vec3{ 0.f, lidY, 0.f }), center, scale);
            const ImVec2 rim = project(toWorld(Vec3{ 0.f, lidY, -caseRadius * 1.08f }), center, scale);
            const Vec3 frontView = normalize(orient(Vec3{ 0.f, 0.f, -1.f }));
            const float size = Util::Max(7.f, scale * 0.07f);
            const ImU32 fill = GetColor(accent, state.GlobalOpacity);
            const ImU32 edge = IM_COL32(16, 16, 20, (int)(235.f * state.GlobalOpacity));
            const float edgeThick = Util::Max(1.5f, size * 0.16f);
            ImVec2 dir = rim - hub;
            const float len = SDL_sqrtf((dir.x * dir.x) + (dir.y * dir.y));
            // Mostly toward or away from the camera, an arrow would point up
            // or down the screen and read as the wrong direction entirely.
            const float toward = dot(frontView, viewDir);
            if(std::abs(toward) < 0.75f && len > size * 0.3f) {
                dir = ImVec2(dir.x / len, dir.y / len);
                const ImVec2 across(-dir.y, dir.x);
                const ImVec2 tip = rim + (dir * (size * 1.7f));
                const ImVec2 left = rim + (across * (size * 0.8f));
                const ImVec2 right = rim - (across * (size * 0.8f));
                drawList->AddLine(hub, rim, edge, edgeThick * 2.2f);
                drawList->AddLine(hub, rim, fill, edgeThick);
                drawList->AddTriangleFilled(tip, left, right, fill);
                drawList->AddTriangle(tip, left, right, edge, edgeThick);
            }
            else if(toward > 0.f) {
                drawList->AddCircleFilled(rim, size * 0.7f, fill, 20);
                drawList->AddCircle(rim, size * 0.7f, edge, 20, edgeThick);
            }
            else {
                drawList->AddCircle(rim, size * 0.7f, edge, 20, edgeThick * 2.2f);
                drawList->AddCircle(rim, size * 0.7f, fill, 20, edgeThick);
            }
        }
    // The highlight ring around the opening is emitted with the body's quads.

    if(!state.ShowAxisReadout) return;

    // Under the foot of the rod, centred on it, on a plate of its own, one
    // axis to a line. Beside the model it floated over whatever the video
    // showed there, in dim line colours, and was hard to read against it.
    static const char* AxisNames[(int32_t)Axis::Count] = {
        "stroke", "surge", "sway", "twist", "roll", "pitch"
    };
    int32_t mappedCount = 0;
    for(int32_t i = 0; i < (int32_t)Axis::Count; i += 1) {
        if(axes.mapped[i]) mappedCount += 1;
    }
    if(mappedCount == 0) return;

    const float font = ImGui::GetFontSize();
    const float nameWidth = ImGui::CalcTextSize("stroke:").x;
    const float valueWidth = ImGui::CalcTextSize("100").x;
    const float gap = font * 0.5f;
    const float pad = font * 0.5f;
    const float lineHeight = font * 1.25f;
    const ImVec2 plateSize(nameWidth + gap + valueWidth + (pad * 2.f),
        (lineHeight * (float)mappedCount) - (font * 0.25f) + (pad * 2.f));
    const ImVec2 plateMin(rodFootAnchor.x - (plateSize.x * 0.5f), rodFootLowest + (font * 0.6f));
    drawList->AddRectFilled(plateMin, plateMin + plateSize,
        IM_COL32(14, 14, 18, (int)(215.f * state.GlobalOpacity)), font * 0.4f);

    ImColor valueColour = accent;
    valueColour.Value.x = Util::Clamp(valueColour.Value.x * 1.45f, 0.f, 1.f);
    valueColour.Value.y = Util::Clamp(valueColour.Value.y * 1.45f, 0.f, 1.f);
    valueColour.Value.z = Util::Clamp(valueColour.Value.z * 1.45f, 0.f, 1.f);
    char name[16];
    char value[8];
    int32_t line = 0;
    for(int32_t i = 0; i < (int32_t)Axis::Count; i += 1) {
        if(!axes.mapped[i]) continue;
        const ImVec2 at(plateMin.x + pad, plateMin.y + pad + (lineHeight * (float)line));
        stbsp_snprintf(name, sizeof(name), "%s:", AxisNames[i]);
        drawList->AddText(at, GetColor(state.Text, state.GlobalOpacity * 0.70f), name);
        stbsp_snprintf(value, sizeof(value), "%.0f", axes.value[i]);
        // Right aligned, so the digits line up down the column.
        const float width = ImGui::CalcTextSize(value).x;
        drawList->AddText(ImVec2(at.x + nameWidth + gap + (valueWidth - width), at.y),
            GetColor(valueColour, state.GlobalOpacity), value);
        line += 1;
    }
}

void ScriptSimulator::drawAxisBar(ImDrawList* frontDraw, const SimulatorState& state,
    const AxisBar& axis, float currentTime, bool showLabel) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    char tmp[8];

    const ImVec2 barP1 = axis.p1;
    const ImVec2 barP2 = axis.p2;
    ImVec2 direction = Normalize(barP1 - barP2);
    const float distance = Distance(barP1, barP2);
    auto perpendicular = ImVec2(-direction.y, direction.x);

    // BACKGROUND
    frontDraw->AddLine(
        barP1 + direction,
        barP2 - direction,
        GetColor(state.Back, state.GlobalOpacity),
        state.Width - state.BorderWidth + 1.f
    );

    // FRONT BAR
    const float percent = axis.position / 100.f;
    frontDraw->AddLine(
        barP2 + ((direction * distance) * percent),
        barP2,
        GetColor(axis.frontColor, state.GlobalOpacity),
        state.Width - state.BorderWidth + 1.f
    );

    // BORDER
    if (state.BorderWidth > 0.f) {
        auto borderOffset = perpendicular * (state.Width / 2.f);
        auto edge = direction * (state.BorderWidth / 2.f);
        frontDraw->AddQuad(
            barP1 + edge - borderOffset, barP1 + edge + borderOffset,
            barP2 - edge + borderOffset, barP2 - edge - borderOffset,
            GetColor(state.Border, state.GlobalOpacity),
            state.BorderWidth
        );
    }

    auto heightLineAt = [&](float pos, float thickness) noexcept {
        auto indicator1 =
            barP2
            + (direction * distance * (pos / 100.f))
            - (perpendicular * (state.Width / 2.f))
            + (perpendicular * (state.BorderWidth / 2.f));
        auto indicator2 =
            barP2
            + (direction * distance * (pos / 100.f))
            + (perpendicular * (state.Width / 2.f))
            - (perpendicular * (state.BorderWidth / 2.f));
        frontDraw->AddLine(indicator1, indicator2,
            GetColor(state.ExtraLines, state.GlobalOpacity), thickness);
    };

    // HEIGHT LINES
    if (state.EnableHeightLines) {
        for (int i = 1; i < 10; i++) {
            heightLineAt(i * 10.f, state.LineWidth);
        }
    }
    if (state.ExtraLinesCount > 0) {
        for (int i = -state.ExtraLinesCount; i < 1; ++i) {
            heightLineAt(i * 10.f, state.ExtraLineWidth);
        }
        for (int i = 10; i < (11 + state.ExtraLinesCount); ++i) {
            heightLineAt(i * 10.f, state.ExtraLineWidth);
        }
    }

    // INDICATORS
    if (state.EnableIndicators && axis.script != nullptr) {
        auto script = axis.script;
        auto previousAction = script->GetActionAtTime(currentTime, 0.02f);
        if (previousAction == nullptr) {
            previousAction = script->GetPreviousActionBehind(currentTime);
        }
        auto nextAction = script->GetNextActionAhead(currentTime);
        if (previousAction != nullptr && nextAction == previousAction) {
            nextAction = script->GetNextActionAhead(previousAction->atS);
        }

        auto drawIndicator = [&](const FunscriptAction* action) noexcept {
            if (action == nullptr) return;
            if (action->pos <= 0 || action->pos >= 100) return;
            auto indicator1 =
                barP2
                + (direction * distance * (action->pos / 100.f))
                - (perpendicular * (state.Width / 2.f))
                + (perpendicular * (state.BorderWidth / 2.f));
            auto indicator2 =
                barP2
                + (direction * distance * (action->pos / 100.f))
                + (perpendicular * (state.Width / 2.f))
                - (perpendicular * (state.BorderWidth / 2.f));
            auto indicatorCenter = barP2 + (direction * distance * (action->pos / 100.f));
            frontDraw->AddLine(indicator1, indicator2,
                GetColor(state.Indicator, state.GlobalOpacity), state.LineWidth);
            stbsp_snprintf(tmp, sizeof(tmp), "%d", action->pos);
            auto textOffset = ImGui::CalcTextSize(tmp);
            textOffset /= 2.f;
            frontDraw->AddText(indicatorCenter - textOffset,
                GetColor(state.Text, state.GlobalOpacity), tmp);
        };
        drawIndicator(previousAction);
        drawIndicator(nextAction);
    }

    // POSITION TEXT
    if (state.EnablePosition) {
        stbsp_snprintf(tmp, sizeof(tmp), "%.0f", axis.position);
        ImGui::PushFont(OFS_DynFontAtlas::DefaultFont2);
        auto textOffset = ImGui::CalcTextSize(tmp);
        textOffset /= 2.f;
        frontDraw->AddText(
            barP2 + direction * distance * 0.5f - textOffset,
            GetColor(state.Text, state.GlobalOpacity),
            tmp
        );
        ImGui::PopFont();
    }

    // AXIS LABEL
    if (showLabel && !axis.label.empty()) {
        auto textSize = ImGui::CalcTextSize(axis.label.c_str());
        // Just past the far end of the bar, centred across its width.
        auto labelPos = barP1 + (direction * (state.Width * 0.35f + textSize.y));
        labelPos -= ImVec2(textSize.x / 2.f, textSize.y / 2.f);
        frontDraw->AddText(labelPos,
            GetColor(axis.isActive ? state.Text : state.ExtraLines, state.GlobalOpacity),
            axis.label.c_str());
    }
}

ScriptSimulator::Mode ScriptSimulator::CurrentMode() noexcept
{
    if (EnableVanilla) return Mode::Slider;
    return SimulatorState::State(stateHandle).ShowMultiAxis ? Mode::Model3D : Mode::Bar;
}

void ScriptSimulator::SetMode(Mode mode) noexcept
{
    auto& state = SimulatorState::State(stateHandle);
    switch (mode) {
        case Mode::Bar:
            state.ShowMultiAxis = false;
            EnableVanilla = false;
            break;
        case Mode::Model3D:
            state.ShowMultiAxis = true;
            EnableVanilla = false;
            break;
        case Mode::Slider:
            // ShowMultiAxis is left as it was, so leaving the slider returns
            // to whichever of bar or model was in use before it.
            EnableVanilla = true;
            break;
        default:
            break;
    }
}

void ScriptSimulator::CycleMode() noexcept
{
    // Between 2D and 3D, the two the selector offers.
    SetMode(CurrentMode() == Mode::Model3D ? Mode::Bar : Mode::Model3D);
}

void ScriptSimulator::DrawModeSelector(const char* id) noexcept
{
    // 2D and 3D. The plain slider is no longer offered here: it duplicated the
    // bar with less in it, and a third choice made the pick harder to read.
    static constexpr const char* labels[] = { "2D", "3D" };
    static constexpr const char* tips[] = {
        "A bar for the active script, drawn over the app wherever you place it. "
        "Unlock it to drag its ends or its middle.",
        "A model of the stroker, driven by every loaded axis at once: stroke, surge, "
        "sway, twist, roll and pitch.",
    };
    const int32_t current = CurrentMode() == Mode::Model3D ? 1 : 0;
    const int32_t picked = OFS::SegmentedControl(id, labels, tips, 2, current);
    if (picked >= 0) SetMode(picked == 1 ? Mode::Model3D : Mode::Bar);
}

// A row of plain buttons that share its width equally, so they read as one
// strip of controls rather than a wall of full width buttons. highlighted
// gives a button the selected look, for a toggle that is on.
//
// The width is worked out when the first button of the row is placed, since
// after SameLine the space left is no longer the whole row.
static bool rowButton(const char* label, int32_t index, int32_t count, bool highlighted = false) noexcept
{
    static float width = 0.f;
    if (index == 0) {
        const auto& style = ImGui::GetStyle();
        width = (ImGui::GetContentRegionAvail().x - (style.ItemSpacing.x * (count - 1))) / count;
    }
    else {
        ImGui::SameLine();
    }

    if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::Role().OnFill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::Role().OnFillHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::Role().OnFillHi);
        ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::Role().OnText);
    }
    const bool clicked = ImGui::Button(label, ImVec2(width, 0.f));
    if (highlighted) ImGui::PopStyleColor(4);
    return clicked;
}

// Whether the simulator's frame may take the mouse from whatever window is
// under it. Called from inside the simulator's own panel window.
static bool simulatorCanTakeMouse(ImGuiID simId, bool refuseAlt) noexcept
{
    const auto& io = ImGui::GetIO();
    if (io.KeyShift || (refuseAlt && io.KeyAlt)) return false;
    // Drawn under popups, so it does not take clicks meant for one: a menu
    // item over its frame would otherwise start a drag of the simulator.
    if (GImGui->OpenPopupStack.Size > 0) return false;
    const ImGuiID active = ImGui::GetActiveID();
    if (active != 0 && active != simId) return false;
    if (ImGuiWindow* hoveredWindow = GImGui->HoveredWindow) {
        const ImGuiWindow* own = ImGui::GetCurrentWindowRead();
        if (hoveredWindow == own && GImGui->HoveredIdPreviousFrame != 0
            && GImGui->HoveredIdPreviousFrame != own->ID) return false;
    }
    return true;
}

void ScriptSimulator::ShowSimulator(bool* open, std::shared_ptr<Funscript>& activeScript, float currentTime, bool splineMode) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);
    float currentPos = 0;

    if (positionOverride >= 0.f) {
        currentPos = positionOverride;
        positionOverride = -1.f;
    }
    else {
        currentPos = splineMode
            ? activeScript->SplineClamped(currentTime)
            : activeScript->GetPositionAtTime(currentTime);
    }

    if (EnableVanilla) {
        // The same window and flags as the other modes. Opening it with
        // NoDocking and NoBackground pulled an already docked panel out of its
        // dock as a see through floating window, laid over whatever was
        // docked beside it, with both panels' text showing through each other.
        ImGui::Begin(TR_ID(WindowId, Tr::SIMULATOR), open, ImGuiWindowFlags_None);
        // The mode switch stays reachable here, or the only way back from the
        // slider would be to close its window.
        DrawModeSelector("##simModeSlider");
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        // Along the panel's longer side. Docked in a short wide panel a
        // vertical slider came out as a thin bar with its value squeezed in.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y >= avail.x) {
            ImGui::VSliderFloat("##simPosition", avail, &currentPos, 0, 100, "%.0f");
        }
        else {
            ImGui::SetNextItemWidth(-1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(ImGui::GetStyle().FramePadding.x, std::max(0.f, (avail.y - ImGui::GetTextLineHeight()) * 0.5f)));
            ImGui::SliderFloat("##simPosition", &currentPos, 0, 100, "%.0f");
            ImGui::PopStyleVar();
        }
        ImGui::PopItemFlag();
        ImGui::End();
        if (!*open) {
            EnableVanilla = false;
            *open = true;
        }
        return;
    }

    ImGui::Begin(TR_ID(WindowId, Tr::SIMULATOR), open, ImGuiWindowFlags_None);
    char tmp[4];
    // Drawn into a see through window of its own that covers the main
    // viewport and takes no input, rather than onto the foreground draw list.
    // The foreground list sits over every window, so the simulator was drawn
    // across any floating panel that overlapped it, special functions or
    // chapters included. This window sits above the docked layout, so the
    // simulator still floats over the video and timeline, while a floating
    // panel opened over it covers it as a window should. Taking no input it
    // is never the hovered window, so grabbing the bar works as before.
    ImDrawList* frontDraw = nullptr;
    {
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->Pos);
        ImGui::SetNextWindowSize(mainViewport->Size);
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        // Not NoBringToFrontOnFocus: ImGui creates such a window at the very
        // back of the display order, under the dockspace and so under the
        // video, where the simulator could not be seen at all. Created without
        // it the window starts in front, and taking no input and no focus it
        // stays there until some other window is focused over it.
        ImGui::Begin("##SimulatorOverlay", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);
        frontDraw = ImGui::GetWindowDrawList();
        // Ended straight away, before anything is drawn into it. Its clip
        // rects are popped by End, which leaves the draw list clipping to the
        // full viewport for everything added after, so the early returns
        // below have nothing to balance.
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    auto& style = ImGui::GetStyle();
    auto& state = SimulatorState::State(stateHandle);

    // What is drawn, first, since everything below it depends on the choice.
    DrawModeSelector("##simMode");
    ImGui::Spacing();

    // Placement. Lock is a toggle, so it takes the selected look while on.
    // Short, because the row splits a narrow panel three ways and "Unlocked"
    // did not fit; the lit look already says whether it is on.
    if (rowButton(state.LockedPosition ? ICON_LINK " Lock###simLock" : ICON_UNLINK " Lock###simLock", 0, 3, state.LockedPosition)) {
        state.LockedPosition = !state.LockedPosition;
    }
    OFS::Tooltip(state.LockedPosition
        ? "The simulator is fixed in place. Click to unlock it, then drag its ends to "
          "resize it or its middle to move it."
        : "Drag the simulator's ends to resize it, or its middle to move it. Click to "
          "lock it in place.");
    if (rowButton(TR(CENTER), 1, 3)) { CenterSimulator(); }
    OFS::Tooltip("Put the simulator back in the middle of the window.");
    if (rowButton(TR(INVERT), 2, 3)) {
        auto tmp = state.P1;
        state.P1 = state.P2;
        state.P2 = tmp;
    }
    OFS::Tooltip("Swap the two ends, so 100 is drawn where 0 was.");

    ImGui::Checkbox("Fit to video", &state.FitToPlayer);
    OFS::Tooltip("Keep the simulator inside the video player, sized to it, and follow the "
                 "player as its window is moved or resized. Moving or resizing the simulator "
                 "by hand turns this off.");
    if (state.FitToPlayer) {
        static constexpr const char* labels[3] = { "Left", "Centre", "Right" };
        static constexpr const char* tips[3] = {
            "Against the left side of the video.",
            "In the middle of the video.",
            "Against the right side of the video.",
        };
        const int32_t picked = OFS::SegmentedControl("##simFitAnchor", labels, tips, 3, Util::Clamp(state.FitAnchor, 0, 2));
        if (picked >= 0) state.FitAnchor = picked;
        int32_t percent = (int32_t)std::round(state.FitSize * 100.f);
        if (OFS::StepperInt("Height (% of the video)", "##simFitSize", &percent, 5, 20, 100)) {
            state.FitSize = (float)percent / 100.f;
        }
        OFS::Tooltip("How much of the video's height the simulator takes up.");
    }

    ImGui::Spacing();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);
    ImGui::Spacing();

    // Only what applies to the mode in use. Not localized: English only for
    // now, and no strings exist for most of these in the translation set.
    if (state.ShowMultiAxis) {
        OFS::StepperFloat("Model scale", "##ModelScale", &state.ModelScale, 0.1f, 0.1f, 4.f, "%.2fx");
        OFS::Tooltip("Size of the model relative to the simulator's length.");
        {
            static constexpr const char* labels[3] = { "Front", "3/4", "Side" };
            static constexpr const char* tips[3] = {
                "From the front, a little above so the floor shows. Roll and sway read best.",
                "From three quarters, a little above: every axis can be seen, pitch and surge included.",
                "From the side. Pitch and surge read best.",
            };
            static constexpr float yaws[3] = { 0.f, 35.f, 90.f };
            // Not quite level for the front: level, the floor is edge on and
            // so is the arrow on it that says where the front is.
            static constexpr float heights[3] = { 12.f, 20.f, 10.f };
            int32_t current = -1;
            for (int32_t i = 0; i < 3; i += 1) {
                if (std::abs(state.CameraYaw - yaws[i]) < 0.5f && std::abs(state.CameraElevation - heights[i]) < 0.5f) current = i;
            }
            ImGui::TextDisabled("View");
            const int32_t picked = OFS::SegmentedControl("##CameraView", labels, tips, 3, current);
            if (picked >= 0) {
                state.CameraYaw = yaws[picked];
                state.CameraElevation = heights[picked];
            }
        }
        OFS::StepperFloat("Camera turn (degrees)", "##CameraTurn", &state.CameraYaw, 15.f, -90.f, 90.f, "%.0f");
        OFS::Tooltip("Turns the viewpoint around the model. Hold Alt and drag over the model to turn it by hand.");
        OFS::StepperFloat("Camera height (degrees)", "##CameraAngle", &state.CameraElevation, 5.f, -25.f, 75.f, "%.0f");
        OFS::Tooltip("Raises the viewpoint. 0 looks straight on. Raise it to tip the "
                     "underside into view, which is where the orifice is.");

        ImGui::Checkbox("Cutaway case", &state.CutawayCase);
        OFS::Tooltip("Remove the near half of the case so the sleeve inside is visible.");
        ImGui::Checkbox("Highlight ring", &state.ShowRim);
        OFS::Tooltip("Outline the opening of the vulva where it comes round, inside the lips.");
        ImGui::SameLine();
        ImGui::Checkbox("Show rod", &state.ShowRod);
        OFS::Tooltip("Draw the rod. Hidden, it still strokes and shapes the sleeve, so the vulva and canal can be watched deforming with nothing in the way.");
        ImGui::SameLine();
        ImGui::ColorEdit3("Rod colour", &state.RodColor.Value.x, ImGuiColorEditFlags_NoInputs);
        OFS::Tooltip("The colour of the rod in the 3D model.");
        OFS::SameLineIfCheckboxFits("Twist direction");
        ImGui::Checkbox("Twist direction", &state.ShowTwistIndicator);
        OFS::Tooltip("Mark which way the front of the case faces, so twist can be read at a glance.");
        OFS::SameLineIfCheckboxFits("Axis readout");
        ImGui::Checkbox("Axis readout", &state.ShowAxisReadout);
        OFS::Tooltip("List each axis that has a script, with its current value, beside the model.");

        if (ImGui::Checkbox("Easter egg", &FinishEasterEgg) && !FinishEasterEgg) {
            FinishDrops.clear();
            FinishStimulation = 0.f;
        }
        OFS::Tooltip("Stroke hard and fast for long enough and the rod finishes inside the "
                     "sleeve. Best seen with the cutaway on.");
        if (FinishEasterEgg) {
            ImGui::SameLine();
            // How close it is, as a small bar rather than a number.
            ImGui::ProgressBar(FinishStimulation, ImVec2(-1.f, ImGui::GetFrameHeight() * 0.5f), "");
        }

        int32_t mapped = 0;
        for (auto& script : OpenFunscripter::ptr->LoadedFunscripts()) {
            if (AxisFromLabel(AxisLabel(script.get())) >= 0) mapped += 1;
        }
        ImGui::Spacing();
        if (mapped <= 1) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Only the stroke axis is loaded, so the model only moves up "
                                "and down. Add .surge, .sway, .twist, .roll or .pitch scripts "
                                "to the project, from Project > Add, to drive the rest.");
            ImGui::PopTextWrapPos();
        }
        else {
            ImGui::TextDisabled("%d of 6 axes driven by a script", mapped);
        }
    }
    else {
        // Wrapped rather than forced onto one row, which ran off the side of
        // the panel at its default width.
        ImGui::Checkbox(TR(INDICATOR), &state.EnableIndicators);
        OFS::Tooltip("Mark the previous and next point on the bar, with their positions.");
        OFS::SameLineIfCheckboxFits(TR(LINES));
        ImGui::Checkbox(TR(LINES), &state.EnableHeightLines);
        OFS::Tooltip("A line across the bar at every 10.");
        OFS::SameLineIfCheckboxFits(TR(SHOW_POSITION));
        ImGui::Checkbox(TR(SHOW_POSITION), &state.EnablePosition);
        OFS::Tooltip("The current position as a number in the middle of the bar.");

        OFS::StepperInt("Extra lines", "##ExtraLines", &state.ExtraLinesCount, 1, 0, 10);
        OFS::Tooltip("Lines continuing past both ends of the bar, 10 apart.");
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        ImGui::ColorEdit4(TR(FRONT), &state.Front.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit4(TR(BACK), &state.Back.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit4(TR(BORDER), &state.Border.Value.x, ImGuiColorEditFlags_NoInputs);

        ImGui::ColorEdit4(TR(TEXT), &state.Text.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit4(TR(INDICATOR), &state.Indicator.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit4(TR(LINES), &state.ExtraLines.Value.x, ImGuiColorEditFlags_NoInputs);

        ImGui::Spacing();
        OFS::StepperFloat("Width (px)", "##SimWidth", &state.Width, 5.f, 0.f, 1000.f, "%.0f");
        OFS::StepperFloat("Border width (px)", "##SimBorderWidth", &state.BorderWidth, 1.f, 0.f, 1000.f, "%.1f");
        OFS::StepperFloat("Line width (px)", "##SimLineWidth", &state.LineWidth, 1.f, 0.5f, 1000.f, "%.1f");
        OFS::StepperFloat("Extra line width (px)", "##SimExtraLineWidth", &state.ExtraLineWidth, 1.f, 0.5f, 1000.f, "%.1f");
        OFS::StepperFloat("Opacity", "##SimOpacity", &state.GlobalOpacity, 0.1f, 0.f, 1.f, "%.2f");
        ImGui::PopItemWidth();

        ImGui::Spacing();
        // The saved default is one config shared by every project, so loading
        // and saving it is kept together with resetting, away from the
        // everyday controls above.
        if (rowButton(TR(LOAD_CONFIG), 0, 3)) {
            auto& dState = SimulatorDefaultConfigState::StaticStateSlow();
            state = dState.defaultState;
        }
        OFS::Tooltip("Replace these settings with the ones saved as the default.");
        if (rowButton(TR(SAVE_CONFIG), 1, 3)) {
            Util::YesNoCancelDialog(TR(SAVE_SIMULATOR_CONFIG),
                TR(SAVE_SIMULATOR_CONFIG_MSG),
                [this](Util::YesNoCancel result) {
                    if(result == Util::YesNoCancel::Yes) {
                        auto& dState = SimulatorDefaultConfigState::StaticStateSlow();
                        auto& state = SimulatorState::State(stateHandle);
                        dState.defaultState = state;
                    }
                }
            );
        }
        OFS::Tooltip("Save these settings as the default that new projects start from.");
        if (rowButton(TR(RESET_TO_DEFAULTS), 2, 3)) {
            state = SimulatorState();
        }
        OFS::Tooltip("Go back to the settings OFS ships with.");
    }
        
    // Not before anything is open, where it was an empty bar drawn across the
    // message saying how to open something.
    //
    // It stays while a popup is open. It used to be hidden then, from when it
    // was drawn over everything and a menu opened under it lost half its
    // entries; but its window is an ordinary one now, and popups, menus and
    // colour pickers are shown above it, so hiding it only made it vanish
    // whenever a right click menu or a colour picker was up.
    if (!OpenFunscripter::ptr->player->VideoLoaded()) {
        ImGui::End();
        return;
    }

    // First sight of the video after the default layout was built: centre
    // over it now that its window exists to centre over.
    if (CenterWhenVideoShows) {
        const ImGuiWindow* video = ImGui::FindWindowByName(OFS_VideoplayerWindow::WindowId);
        if (video != nullptr && video->WasActive && video->Size.y > 0.f) {
            CenterSimulator();
            CenterWhenVideoShows = false;
        }
    }

    LastDrawnFrame = ImGui::GetFrameCount();
    auto simId = ImGui::GetID("ActualSimulator");
    ImGui::KeepAliveID(simId);
    auto offset = ImGui::GetWindowViewport()->Pos;

    // Fitted to the video player: placed and sized from the player's window
    // every frame, so it follows the player however that is moved or resized
    // and never strays off it. Which end is which is kept, for Invert.
    if (state.FitToPlayer) {
        const ImGuiWindow* video = ImGui::FindWindowByName(OFS_VideoplayerWindow::WindowId);
        if (video != nullptr && video->WasActive
            && video->InnerRect.GetWidth() > 0.f && video->InnerRect.GetHeight() > 0.f) {
            // The picture, not the player's window: letterboxed, the window
            // runs on either side of it, and fitted to the window the
            // simulator sat out in the black. Clipped to the window, for a
            // picture zoomed past its edges.
            ImRect rect = video->InnerRect;
            // In 2D only: the 3D view, with its floor and wall, is a scene of its
            // own rather than something laid over the picture, so it takes the
            // whole player.
            if (const auto& playerWindow = OpenFunscripter::ptr->playerWindow; playerWindow && !state.ShowMultiAxis) {
                if (playerWindow->VideoScreenFrame >= ImGui::GetFrameCount() - 2) {
                    ImRect picture(playerWindow->VideoScreenMin, playerWindow->VideoScreenMax);
                    picture.ClipWithFull(rect);
                    if (picture.GetWidth() > 1.f && picture.GetHeight() > 1.f) rect = picture;
                }
            }
            const float margin = 0.04f * Util::Min(rect.GetWidth(), rect.GetHeight());
            const float usableW = Util::Max(1.f, rect.GetWidth() - (margin * 2.f));
            const float usableH = Util::Max(1.f, (rect.GetHeight() - (margin * 2.f)) * Util::Clamp(state.FitSize, 0.2f, 1.f));
            const int32_t anchor = Util::Clamp(state.FitAnchor, 0, 2);
            const bool inverted = state.P1.y > state.P2.y;
            bool placed = false;
            ImVec2 top, bottom;
            if (state.ShowMultiAxis) {
                // The whole frame, floor and wall included, fitted inside.
                if (fitRelValid) {
                    const ImVec2 relSize(Util::Max(1e-3f, fitRelMax.x - fitRelMin.x), Util::Max(1e-3f, fitRelMax.y - fitRelMin.y));
                    const float s = Util::Min(usableH / relSize.y, usableW / relSize.x);
                    ImVec2 c;
                    c.y = rect.GetCenter().y - ((fitRelMin.y + fitRelMax.y) * 0.5f * s);
                    if (anchor == 0) c.x = rect.Min.x + margin - (fitRelMin.x * s);
                    else if (anchor == 2) c.x = rect.Max.x - margin - (fitRelMax.x * s);
                    else c.x = rect.GetCenter().x - ((fitRelMin.x + fitRelMax.x) * 0.5f * s);
                    // The model's size goes with the bar's length plus its border.
                    const float wanted = (2.f * s) / Util::Clamp(state.ModelScale, 0.1f, 4.f);
                    const float length = Util::Max(20.f, wanted - state.BorderWidth);
                    top = (c - offset) - ImVec2(0.f, length / 2.f);
                    bottom = (c - offset) + ImVec2(0.f, length / 2.f);
                    placed = true;
                }
            }
            else {
                const float length = Util::Max(20.f, usableH - state.BorderWidth);
                float x = rect.GetCenter().x;
                if (anchor == 0) x = rect.Min.x + margin + (state.Width / 2.f);
                else if (anchor == 2) x = rect.Max.x - margin - (state.Width / 2.f);
                const float cy = rect.GetCenter().y;
                top = ImVec2(x, cy - (length / 2.f)) - offset;
                bottom = ImVec2(x, cy + (length / 2.f)) - offset;
                placed = true;
            }
            if (placed) {
                state.P1 = inverted ? bottom : top;
                state.P2 = inverted ? top : bottom;
            }
        }
    } 
    
    ImVec2 direction = state.P1 - state.P2;
    direction = Normalize(direction);
    ImVec2 barP1 = offset + state.P1 - (direction * (state.BorderWidth / 2.f));
    ImVec2 barP2 = offset + state.P2 + (direction * (state.BorderWidth / 2.f));
    float distance = Distance(barP1, barP2);
    auto perpendicular = Normalize(state.P1 - state.P2);
    perpendicular = ImVec2(-perpendicular.y, perpendicular.x);

    auto app = OpenFunscripter::ptr;
    const auto& scripts = app->LoadedFunscripts();

    const ImVec2 modelCenter = barP2 + (Normalize(barP1 - barP2) * (distance / 2.f));
    const float modelScale = Util::Max(1e-3f, (distance / 2.f) * Util::Clamp(state.ModelScale, 0.1f, 4.f));

    if(state.ShowMultiAxis) {
        // Every loaded script feeds one degree of freedom of a single model.
        MultiAxisValues axes;
        for(size_t i = 0; i < scripts.size(); i += 1) {
            const auto& script = scripts[i];
            const int32_t axisIdx = AxisFromLabel(AxisLabel(script.get()));
            if(axisIdx < 0) continue;
            // An axis with no points yet holds neutral. Read as a position it
            // came out as 0, full deflection, so adding an empty twist or roll
            // script tipped the whole model over before anything was scripted.
            if(script->Actions().empty()) continue;

            axes.value[axisIdx] = splineMode
                ? script->SplineClamped(currentTime)
                : script->GetPositionAtTime(currentTime);
            axes.mapped[axisIdx] = true;
        }
        // The active script always drives its axis from the same value the
        // single axis view would show, including any position override.
        {
            const int32_t activeIdx = AxisFromLabel(AxisLabel(activeScript.get()));
            if(activeIdx >= 0) {
                axes.value[activeIdx] = currentPos;
                axes.mapped[activeIdx] = true;
            }
        }

        for(int32_t i = 0; i < (int32_t)Axis::Count; i += 1) {
            if(TestAxisOverride[i] >= 0.f) {
                axes.value[i] = TestAxisOverride[i];
                axes.mapped[i] = true;
            }
        }

        ModelDrawnValid = false;
        drawMultiAxisModel(frontDraw, state, axes, modelCenter, modelScale);
        if (ModelDrawnValid) {
            fitRelMin = (ModelDrawnMin - modelCenter) / modelScale;
            fitRelMax = (ModelDrawnMax - modelCenter) / modelScale;
            fitRelValid = true;
        }
    }
    else {
        AxisBar bar;
        bar.p1 = barP1;
        bar.p2 = barP2;
        bar.position = currentPos;
        bar.script = activeScript.get();
        bar.frontColor = state.Front;
        bar.isActive = true;
        drawAxisBar(frontDraw, state, bar, currentTime, false);
    }

    // The frame: around the model as drawn in 3D, around the bar in 2D.
    if(state.ShowMultiAxis && ModelDrawnValid) {
        FrameMin = ModelDrawnMin;
        FrameMax = ModelDrawnMax;
    }
    else {
        const ImVec2 across = perpendicular * (state.Width / 2.f);
        FrameMin = ImMin(ImMin(barP1 - across, barP1 + across), ImMin(barP2 - across, barP2 + across));
        FrameMax = ImMax(ImMax(barP1 - across, barP1 + across), ImMax(barP2 - across, barP2 + across));
    }

    // Alt and drag over the 3D model turns the camera around it and raises or
    // lowers it, to see pitch and surge from the side. Locked or not: it only
    // changes the view. Not the right mouse button, which opens the video's
    // menu wherever it is released.
    if (state.ShowMultiAxis && ModelDrawnValid) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overVideo = simulatorCanTakeMouse(simId, false);
        const bool inFrame = mouse.x >= FrameMin.x && mouse.x <= FrameMax.x
            && mouse.y >= FrameMin.y && mouse.y <= FrameMax.y;
        if (!orbiting && overVideo && inFrame && ImGui::GetIO().KeyAlt) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetHoveredID(ImGui::GetCurrentWindowRead()->ID);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImGui::SetActiveID(simId, ImGui::GetCurrentWindowRead());
                orbiting = true;
                orbitStartYaw = state.CameraYaw;
                orbitStartElevation = state.CameraElevation;
            }
        }
        if (orbiting) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
            state.CameraYaw = Util::Clamp(orbitStartYaw - (delta.x * 0.4f), -90.f, 90.f);
            state.CameraElevation = Util::Clamp(orbitStartElevation - (delta.y * 0.3f), -25.f, 75.f);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                orbiting = false;
                if (ImGui::GetActiveID() == simId) ImGui::ClearActiveID();
            }
        }
    }
    else {
        orbiting = false;
    }

    if (!state.LockedPosition)
    {
        const ImVec2 mouse = ImGui::GetMousePos();

        // Unlocked, the simulator takes the mouse wherever its frame is, over
        // any window. Limited to the video, a frame that had strayed off the
        // player could not be grabbed at all to bring it back. Not with Shift
        // held, which places points on the timeline and moved the simulator
        // instead when it lay over it; not with Alt, which turns the camera;
        // not while something else is being dragged; and not over a control in
        // its own panel, so Lock can always be reached.
        const bool canGrab = simulatorCanTakeMouse(simId, true);

        // How close counts as on an edge or an end, in pixels.
        const float grip = Util::Max(8.f, ImGui::GetFontSize() * 0.6f);
        const ImVec2 barAxis = Normalize(barP1 - barP2);
        const ImVec2 barAcross(-barAxis.y, barAxis.x);

        // What is under the mouse. The whole of what is drawn is a handle:
        // grabbing the model or the bar anywhere moves it, and its edges, or
        // in 2D its ends, resize it. They used to be three invisible circles,
        // at the bar's ends and its middle, which in 3D were wherever the bar
        // would have been rather than anywhere on the model.
        SimHandle hovered = SimHandle::None;
        if (canGrab) {
            if (state.ShowMultiAxis) {
                if (ModelDrawnValid
                    && mouse.x >= FrameMin.x - grip && mouse.x <= FrameMax.x + grip
                    && mouse.y >= FrameMin.y - grip && mouse.y <= FrameMax.y + grip) {
                    const bool left = std::abs(mouse.x - FrameMin.x) <= grip;
                    const bool right = !left && std::abs(mouse.x - FrameMax.x) <= grip;
                    const bool top = std::abs(mouse.y - FrameMin.y) <= grip;
                    const bool bottom = !top && std::abs(mouse.y - FrameMax.y) <= grip;
                    if (top && left) hovered = SimHandle::TopLeft;
                    else if (top && right) hovered = SimHandle::TopRight;
                    else if (bottom && left) hovered = SimHandle::BottomLeft;
                    else if (bottom && right) hovered = SimHandle::BottomRight;
                    else if (top) hovered = SimHandle::Top;
                    else if (bottom) hovered = SimHandle::Bottom;
                    else if (left) hovered = SimHandle::Left;
                    else if (right) hovered = SimHandle::Right;
                    else if (mouse.x > FrameMin.x && mouse.x < FrameMax.x
                        && mouse.y > FrameMin.y && mouse.y < FrameMax.y) hovered = SimHandle::Move;
                }
            }
            else {
                const ImVec2 fromP2 = mouse - barP2;
                const float along = (fromP2.x * barAxis.x) + (fromP2.y * barAxis.y);
                const float side = (fromP2.x * barAcross.x) + (fromP2.y * barAcross.y);
                if (std::abs(side) <= (state.Width / 2.f) + (grip * 0.5f)
                    && along >= -grip && along <= distance + grip) {
                    // A fifth of the bar at each end, within reason, resizes.
                    const float endZone = Util::Clamp(distance * 0.2f, grip, grip * 4.f);
                    if (along <= endZone) hovered = SimHandle::End2;
                    else if (along >= distance - endZone) hovered = SimHandle::End1;
                    else hovered = SimHandle::Move;
                }
            }
        }

        const bool barUpright = std::abs(barAxis.y) >= std::abs(barAxis.x);
        auto cursorFor = [barUpright](SimHandle handle) noexcept {
            switch (handle) {
                case SimHandle::Move: return ImGuiMouseCursor_ResizeAll;
                case SimHandle::End1:
                case SimHandle::End2: return barUpright ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW;
                case SimHandle::Top:
                case SimHandle::Bottom: return ImGuiMouseCursor_ResizeNS;
                case SimHandle::Left:
                case SimHandle::Right: return ImGuiMouseCursor_ResizeEW;
                case SimHandle::TopLeft:
                case SimHandle::BottomRight: return ImGuiMouseCursor_ResizeNWSE;
                case SimHandle::TopRight:
                case SimHandle::BottomLeft: return ImGuiMouseCursor_ResizeNESW;
                default: return ImGuiMouseCursor_Arrow;
            }
        };

        if (activeHandle == SimHandle::None && hovered != SimHandle::None && !orbiting && !ImGui::GetIO().KeyAlt) {
            ImGui::SetMouseCursor(cursorFor(hovered));
            ImGui::SetHoveredID(ImGui::GetCurrentWindowRead()->ID);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImGui::SetActiveID(simId, ImGui::GetCurrentWindowRead());
                activeHandle = hovered;
                // Placed by hand now, so no longer fitted to the player.
                state.FitToPlayer = false;
                startDragP1 = state.P1;
                startDragP2 = state.P2;
                dragStartFrameMin = FrameMin;
                dragStartFrameMax = FrameMax;
            }
        }
        if (activeHandle == SimHandle::None && !orbiting && ImGui::GetActiveID() == simId) {
            ImGui::ClearActiveID();
        }

        if (activeHandle != SimHandle::None) {
            ImGui::SetMouseCursor(cursorFor(activeHandle));
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
            switch (activeHandle) {
                case SimHandle::Move:
                    state.P1 = startDragP1 + delta;
                    state.P2 = startDragP2 + delta;
                    break;
                case SimHandle::End1:
                    state.P1 = startDragP1 + delta;
                    break;
                case SimHandle::End2:
                    state.P2 = startDragP2 + delta;
                    break;
                default: {
                    // An edge or a corner of the 3D frame. The model keeps its
                    // proportions, so it is scaled evenly, about the side or
                    // corner opposite the one held, which stays where it is.
                    const ImVec2 oldMin = dragStartFrameMin;
                    const ImVec2 oldMax = dragStartFrameMax;
                    const ImVec2 oldSize(Util::Max(1.f, oldMax.x - oldMin.x), Util::Max(1.f, oldMax.y - oldMin.y));
                    const ImVec2 oldMid = (oldMin + oldMax) * 0.5f;
                    const bool l = activeHandle == SimHandle::Left || activeHandle == SimHandle::TopLeft || activeHandle == SimHandle::BottomLeft;
                    const bool r = activeHandle == SimHandle::Right || activeHandle == SimHandle::TopRight || activeHandle == SimHandle::BottomRight;
                    const bool t = activeHandle == SimHandle::Top || activeHandle == SimHandle::TopLeft || activeHandle == SimHandle::TopRight;
                    const bool b = activeHandle == SimHandle::Bottom || activeHandle == SimHandle::BottomLeft || activeHandle == SimHandle::BottomRight;
                    float kx = 0.f, ky = 0.f;
                    if (l) kx = (oldSize.x - delta.x) / oldSize.x;
                    if (r) kx = (oldSize.x + delta.x) / oldSize.x;
                    if (t) ky = (oldSize.y - delta.y) / oldSize.y;
                    if (b) ky = (oldSize.y + delta.y) / oldSize.y;
                    float k = (l || r) && (t || b) ? Util::Max(kx, ky) : ((l || r) ? kx : ky);
                    // No smaller than a few dozen pixels tall.
                    k = Util::Max(k, 40.f / oldSize.y);
                    const ImVec2 anchor(
                        l ? oldMax.x : (r ? oldMin.x : oldMid.x),
                        t ? oldMax.y : (b ? oldMin.y : oldMid.y));

                    // The model's size goes with the length between the two
                    // points plus the border, and its centre is their middle.
                    const ImVec2 startMid = offset + ((startDragP1 + startDragP2) * 0.5f);
                    const ImVec2 dir = Normalize(startDragP1 - startDragP2);
                    const float startLength = Distance(startDragP1, startDragP2);
                    const float newLength = Util::Max(20.f, (k * (startLength + state.BorderWidth)) - state.BorderWidth);
                    const ImVec2 newMid = anchor + ((startMid - anchor) * k);
                    state.P1 = (newMid - offset) + (dir * (newLength / 2.f));
                    state.P2 = (newMid - offset) - (dir * (newLength / 2.f));
                    break;
                }
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                activeHandle = SimHandle::None;
            }
        }
        IsMovingSimulator = activeHandle == SimHandle::Move;

        // Shown while the mouse is on it or it is being dragged, so what will
        // be grabbed is visible before the click.
        const SimHandle shown = activeHandle != SimHandle::None ? activeHandle : hovered;
        if (shown != SimHandle::None) {
            const ImU32 frameColour = IM_COL32(0xE8, 0x54, 0x8A, 0xD0);
            const ImU32 gripColour = IM_COL32(0xF4, 0xA6, 0xC3, 0xFF);
            const ImU32 gripHot = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
            const float thick = 1.5f;
            if (state.ShowMultiAxis) {
                frontDraw->AddRect(FrameMin, FrameMax, frameColour, 4.f, 0, thick);
                const float g = grip * 0.45f;
                auto square = [&](ImVec2 at, SimHandle handle) noexcept {
                    const ImU32 c = shown == handle ? gripHot : gripColour;
                    frontDraw->AddRectFilled(at - ImVec2(g, g), at + ImVec2(g, g), c, 2.f);
                };
                const ImVec2 mid = (FrameMin + FrameMax) * 0.5f;
                square(FrameMin, SimHandle::TopLeft);
                square(ImVec2(FrameMax.x, FrameMin.y), SimHandle::TopRight);
                square(ImVec2(FrameMin.x, FrameMax.y), SimHandle::BottomLeft);
                square(FrameMax, SimHandle::BottomRight);
                auto bar = [&](ImVec2 a, ImVec2 b2, SimHandle handle) noexcept {
                    frontDraw->AddLine(a, b2, shown == handle ? gripHot : gripColour, 4.f);
                };
                bar(ImVec2(mid.x - g * 2.f, FrameMin.y), ImVec2(mid.x + g * 2.f, FrameMin.y), SimHandle::Top);
                bar(ImVec2(mid.x - g * 2.f, FrameMax.y), ImVec2(mid.x + g * 2.f, FrameMax.y), SimHandle::Bottom);
                bar(ImVec2(FrameMin.x, mid.y - g * 2.f), ImVec2(FrameMin.x, mid.y + g * 2.f), SimHandle::Left);
                bar(ImVec2(FrameMax.x, mid.y - g * 2.f), ImVec2(FrameMax.x, mid.y + g * 2.f), SimHandle::Right);
            }
            else {
                const ImVec2 across = barAcross * ((state.Width / 2.f) + 3.f);
                frontDraw->AddQuad(barP1 - across, barP1 + across, barP2 + across, barP2 - across, frameColour, thick);
                const ImVec2 gripAcross = barAcross * (state.Width * 0.3f);
                frontDraw->AddLine(barP1 - gripAcross, barP1 + gripAcross,
                    shown == SimHandle::End1 ? gripHot : gripColour, 5.f);
                frontDraw->AddLine(barP2 - gripAcross, barP2 + gripAcross,
                    shown == SimHandle::End2 ? gripHot : gripColour, 5.f);
            }
        }
    }
    else { activeHandle = SimHandle::None; IsMovingSimulator = false; }
    ImGui::End();
}

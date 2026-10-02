#pragma once

#include "imgui.h"
#include "OFS_Reflection.h"
#include "OFS_BinarySerialization.h"
#include "OFS_Event.h"

#include <memory>
#include <string>
#include <vector>

class ScriptSimulator {
private:
	ImVec2 startDragP1;
	ImVec2 startDragP2;
	ImVec2* dragging = nullptr;
	float mouseValue;
	uint32_t stateHandle = 0xFFFF'FFFF;
	bool IsMovingSimulator = false;
	// Which part of the simulator is being dragged: its body moves it, its
	// ends (2D) or the edges and corners of its frame (3D) resize it.
	enum class SimHandle : int32_t
	{
		None, Move, End1, End2,
		Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight
	};
	SimHandle activeHandle = SimHandle::None;
	ImVec2 dragStartFrameMin;
	ImVec2 dragStartFrameMax;
	// Turning the camera by holding Alt and dragging over the 3D model.
	bool orbiting = false;
	// The 3D frame relative to the model's centre, in its own units, as last
	// drawn, which fitting to the player sizes the model from.
	ImVec2 fitRelMin;
	ImVec2 fitRelMax;
	bool fitRelValid = false;
	float orbitStartYaw = 0.f;
	float orbitStartElevation = 0.f;
	bool EnableVanilla = false;
	bool MouseOnSimulator = false;
public:
	static constexpr const char* WindowId = "###SIMULATOR";

	// What the simulator draws. Stored as ShowMultiAxis in the saved state and
	// EnableVanilla here, as it always was, so existing configs load unchanged;
	// this is only the one place that reads them as a single choice.
	enum class Mode : int32_t
	{
		Bar,
		Model3D,
		Slider,
		Count
	};
	Mode CurrentMode() noexcept;
	void SetMode(Mode mode) noexcept;
	void CycleMode() noexcept;

	float positionOverride = -1.f;

	// The frame the simulator is moved and resized by, on screen, updated
	// every frame. For the 3D model, the largest box it can take up whatever
	// its axes do, from the current camera, set while drawing it.
	ImVec2 FrameMin;
	ImVec2 FrameMax;
	ImVec2 ModelDrawnMin;
	ImVec2 ModelDrawnMax;
	bool ModelDrawnValid = false;
	inline uint32_t StateHandle() const noexcept { return stateHandle; }
	// The frame the simulator was last drawn on.
	int32_t LastDrawnFrame = -1;

	// Set when the default layout is built. The video window does not exist
	// yet at that point, so centring then puts the simulator in the middle of
	// the whole window, across the timeline; this centres it over the video
	// the first time the video is on screen instead.
	bool CenterWhenVideoShows = false;

	void MouseMovement(const OFS_SDL_Event* ev) noexcept;

	inline float getMouseValue() const { return mouseValue; }

	// One bar of the simulator. Used for the ordinary single axis view.
	struct AxisBar
	{
		ImVec2 p1;
		ImVec2 p2;
		float position = 0.f;
		// Non-const: the action lookups used for the indicators are non-const.
		class Funscript* script = nullptr;
		ImColor frontColor;
		std::string label;
		bool isActive = false;
	};
	void drawAxisBar(struct ImDrawList* drawList, const struct SimulatorState& state,
		const AxisBar& axis, float currentTime, bool showLabel) noexcept;

	// The six TCode axes, in the order they are laid out for the readout.
	enum class Axis : int32_t
	{
		Stroke = 0, // L0
		Surge,      // L1
		Sway,       // L2
		Twist,      // R0
		Roll,       // R1
		Pitch,      // R2
		Count
	};
	struct MultiAxisValues
	{
		// Live position of each axis, 0..100, 50 being neutral.
		float value[(int32_t)Axis::Count];
		// Whether a script is actually driving that axis.
		bool mapped[(int32_t)Axis::Count];
		MultiAxisValues() noexcept
		{
			for(int32_t i = 0; i < (int32_t)Axis::Count; i += 1) {
				value[i] = 50.f;
				mapped[i] = false;
			}
		}
	};
	// Ribs inside the canal are dragged along by the rod rather than sitting
	// still, so they need to persist between frames: how far they are currently
	// pulled, and where the rod was last frame to work out which way it moved.
	float ribDrag = 0.f;
	// Flesh in motion: how fast the ridges are being dragged, in ridge periods
	// a second; how far the soft sleeve about the opening is jiggling as the
	// case strokes, and how fast; how far the cervix is squashed and how fast
	// that is changing; and how fast the uterus is distending.
	float ribDragVel = 0.f;
	float fleshJiggle = 0.f;
	float fleshJiggleVel = 0.f;
	float fleshLastStroke = 0.f;
	float fleshLastStrokeVel = 0.f;
	bool fleshPrimed = false;
	float cervixPress = 0.f;
	float cervixPressVel = 0.f;
	float uterusDistensionVel = 0.f;
	float lastInsertion = -1.f;
	// How far each ring of the lips is held open beyond its rest, and how fast
	// that is changing: the lips open to the rod as a spring, so they lag a
	// touch behind it going in and settle back as it withdraws.
	std::vector<float> lipGape;
	std::vector<float> lipGapeVel;

	// Easter egg for the 3D model: stroke hard and fast for long enough and the
	// rod finishes inside the sleeve. Off by default and deliberately not
	// saved, so it never turns up in a session that did not ask for it.
	bool FinishEasterEgg = false;
	// 0 to 1. Fills with how fast the rod is moving while it is inside, and
	// drains on its own, so slow strokes never get there.
	float FinishStimulation = 0.f;
	int32_t FinishCount = 0;
	struct FinishDrop
	{
		// Along the body's own axis and across it, in the body's frame, so the
		// fluid rides with the sleeve as it strokes rather than hanging in the air.
		float y = 0.f;
		float x = 0.f;
		float z = 0.f;
		float vy = 0.f;
		float vx = 0.f;
		float vz = 0.f;
		float life = 1.f;
		float size = 1.f;
		bool settled = false;
		// Which rope this node belongs to. Nodes of one rope are released in
		// order and stay next to each other in the list, which is what the
		// strands between them are drawn from.
		int32_t rope = 0;
		// 0 flying up the canal, 1 clinging to the closed end of the reservoir,
		// 2 falling to the pool, 3 merging into it, 4 spent short of the
		// reservoir and sliding back down the canal, 5 sitting on the tip of
		// the rod, from where a hard enough thrust throws it up again. How
		// long it has left to cling.
		int32_t phase = 0;
		float timer = 0.f;
		// Where it stuck, and how far it has since drooped from there on a
		// thread.
		float anchorY = 0.f;
		float sag = 0.f;
	};
	std::vector<FinishDrop> FinishDrops;
	float finishLastInsertion = -1.f;
	// The rope currently being let out: how far through its nodes, how long
	// since it started, how hard, and the phase of its curl.
	int32_t finishEmitIndex = 0;
	float finishEmitTimer = 0.f;
	float finishEmitStrength = 0.f;
	int32_t finishRopeId = 0;
	float finishRopeWobble = 0.f;
	// How far the opening at the tip of the rod is dilated as a spurt comes out
	// of it, 0 to 1, and how fast that is changing: flesh, so it springs.
	float meatusDilate = 0.f;
	float meatusDilateVel = 0.f;
	// Everything let out gathers into one pile at the closed end of the canal
	// and stays there, so it fills a finish at a time. The volume is in the
	// model's own units; the fraction is how full the canal is.
	float FinishPileVolume = 0.f;
	float FinishPileFraction = 0.f;
	// How far the uterus has distended, 0 collapsed and empty to 1 at its most,
	// which it reaches holding ten finishes. It follows what it holds, easing
	// out after each spurt.
	float UterusDistension = 0.f;
	// Set to finish at once rather than waiting to be stroked up to it, for
	// trying the easter egg out.
	bool FinishRequested = false;
	// For the UI tests: an axis held at a position, 0 to 100, as if a script
	// drove it, or below zero to leave it to its script. Lets a test tilt the
	// model with no roll or pitch script loaded.
	float TestAxisOverride[(int32_t)Axis::Count] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
	// What the tip holds up in the canal, fallen back onto it short of the
	// cervix, in the same units as the pile.
	float FinishCanalVolume = 0.f;
	// The pools slosh. How far the surface of each is tipped across the body, as
	// a slope along its x and z, and how fast; how far the pool in the uterus
	// bobs along the body and the pool on the tip bobs on it, and how fast; and
	// what the body and the tip were doing last frame, to tell how they are
	// being thrown about.
	float poolTiltX = 0.f;
	float poolTiltZ = 0.f;
	float poolTiltVelX = 0.f;
	float poolTiltVelZ = 0.f;
	float poolBob = 0.f;
	float poolBobVel = 0.f;
	float canalBob = 0.f;
	float canalBobVel = 0.f;
	float poolLastStroke = 0.f;
	float poolLastStrokeVel = 0.f;
	float poolLastSway = 0.f;
	float poolLastSwayVel = 0.f;
	float poolLastSurge = 0.f;
	float poolLastSurgeVel = 0.f;
	float poolLastTipVel = 0.f;
	bool poolMotionPrimed = false;
	// How much pile one rope node is worth, set at the start of each finish
	// so that a whole finish adds the same share of the canal.
	float finishNodeVolume = 0.f;
	float finishDripTimer = 0.f;
	// Seconds since the current finish began, or below zero when none is under way.
	float finishClock = -1.f;
	int32_t finishPulsesFired = 0;
	// A film of what has been let out: on the wall of the canal, ring by ring,
	// and on the rod, band by band, 0 to 1. Laid down where the ropes pass and
	// where the pool stands, carried between wall and rod as the rod strokes
	// through the wet canal, and drying off slowly.
	std::vector<float> finishCanalCoat;
	std::vector<float> finishRodCoat;
	// The pool heaves as each part of a rope lands in it, and settles.
	float finishPileHeave = 0.f;
	float finishRodDripTimer = 0.f;
	// A twitch of the rod: seconds since it started, or below zero for none,
	// and how hard. Every spurt comes with one, and close to finishing the rod
	// trembles now and then.
	float finishTwitchTime = -1.f;
	float finishTwitchStrength = 0.f;
	// Where the tip was last frame, along the body, for how fast it is moving.
	float finishLastTipY = -1e9f;
	float finishTremorTimer = 0.f;

	void drawMultiAxisModel(struct ImDrawList* drawList, const struct SimulatorState& state,
		const MultiAxisValues& axes, ImVec2 center, float scale) noexcept;

	void Init() noexcept;
	void CenterSimulator() noexcept;
	// Bar | 3D | Slider, shared by the panel and the toolbar.
	void DrawModeSelector(const char* id) noexcept;
	void ShowSimulator(bool* open, std::shared_ptr<class Funscript>& activeScript, float currentTime, bool splineMode) noexcept;
};


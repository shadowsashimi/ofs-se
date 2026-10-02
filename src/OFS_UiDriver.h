#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Drives the app from a script, a frame at a time, and writes down what came
// of it: screenshots, app state, and where every window is.
//
// Enabled by setting OFS_UITEST_SCRIPT to a script file, and OFS_UITEST_OUT to
// the directory results go to; tools\ui-test.ps1 sets both, along with a
// throwaway profile. Without them nothing here is created and the app runs as
// normal.
//
// A scripted run draws off screen, since a window appearing takes the mouse
// and the keyboard from whatever the user is doing. OFS_UITEST_SHOW=1 puts it
// back on the screen, to watch a run.
//
// Input goes in through ImGui's event queue after the SDL backend has queued
// the real input for the frame, so the script wins. The mouse position is sent
// again every frame for the same reason: the backend reports the real cursor
// each frame while the window has focus. Modifiers are also set on SDL, since
// parts of the timeline ask SDL rather than ImGui whether Ctrl is held.
//
// Script, one command per line, # for comments. Coordinates are pixels from the
// top left of the window. $MEDIA is replaced by OFS_UITEST_MEDIA, and $OUT by
// the output directory, for files a test writes and reads back.
//
//   size W H                 window size, restored if maximized
//   wait N                   let N frames pass
//   open PATH                open a media or project file
//   newblank SECONDS PATH    start a script with no video at PATH, skipping the save dialog
//   move X Y                 put the mouse at X Y
//   lanemove SECONDS POS     put the mouse at that time and position in the active lane
//   pointmove I [DX DY]      put the mouse on point I of the active script, plus a pixel offset
//   tap [left|right|middle]  click wherever the mouse is
//   click X Y [left|right|middle]
//   dclick X Y               double click
//   drag X1 Y1 X2 Y2 [STEPS] press at the first point, move to the second, release
//   press left|right|middle  / release left|right|middle
//   wheel DY                 scroll at the mouse
//   key CHORD                e.g. ctrl+z, shift+delete, space, f10
//   hold shift|ctrl|alt      / let shift|ctrl|alt
//   type TEXT                text input
//   action ID                run a keybinding action by id, e.g. undo
//   seek SECONDS             / pause / play
//   shot NAME                screenshot to NAME.png
//   state NAME               app state to NAME.txt
//   windows NAME             every visible window's rectangle to NAME.txt
//   expect KEY VALUE         compare against the state dump; logged as PASS or FAIL
//   note TEXT                written to the log
//   quit
//
// The script ends with quit whether it says so or not.
//
// Lines starting #! are comments here too; they tell tools\ui-test-all.ps1
// how to run the script: which media, and whether it is a slow sweep.
class OFS_UiDriver
{
public:
    static std::unique_ptr<OFS_UiDriver> FromEnvironment() noexcept;
    static bool Enabled() noexcept;
    // Whether a scripted run puts its window on the screen. Off by default:
    // the window would take focus from whatever the user is doing. Set
    // OFS_UITEST_SHOW=1 to watch a run.
    static bool ShowsWindow() noexcept;

    // Before ImGui::NewFrame, after the backend has queued the real input.
    void BeforeNewFrame() noexcept;
    // After the frame is rendered and before it is swapped to the screen.
    void AfterRender() noexcept;

private:
    enum class OpType : int32_t
    {
        Frames,
        WaitUntil,
        Size,
        Open,
        NewBlank,
        MouseMove,
        LaneMove,
        PointMove,
        MouseButton,
        Wheel,
        Key,
        Modifier,
        Text,
        Action,
        SimAxis,
        Seek,
        Pause,
        Play,
        Shot,
        State,
        Windows,
        Expect,
        Note,
        Quit,
    };

    struct Op
    {
        OpType type = OpType::Frames;
        float x = 0.f;
        float y = 0.f;
        int32_t n = 0;
        bool down = false;
        std::string text;
        std::string text2;
        int32_t line = 0;
    };

    std::vector<Op> ops;
    // Frames spent on the current waituntil so far.
    int32_t waitUntilFrames = 0;
    bool conditionMet(const Op& op) noexcept;
    size_t nextOp = 0;
    int32_t waitFrames = 0;
    uint64_t frame = 0;
    // How long the app spent building and drawing each of the last frames,
    // from before the frame starts to after it is rendered. The frame limiter
    // sleeps outside that span, so this is the work rather than the pace.
    uint64_t frameStartCounter = 0;
    std::vector<float> frameMilliseconds;

    float mouseX = -1.f;
    float mouseY = -1.f;
    uint32_t heldModifiers = 0;

    std::string pendingShot;
    // The size the script asked for, in pixels, held to on every frame. A
    // window manager that snaps new windows into a zone, as PowerToys
    // FancyZones does, moves the window after the script has sized it.
    int32_t wantWidth = 0;
    int32_t wantHeight = 0;
    void holdWindowSize() noexcept;
    std::filesystem::path outDir;
    int32_t passed = 0;
    int32_t failed = 0;
    bool finished = false;

    bool parse(const std::string& script, const std::string& media) noexcept;
    void execute(const Op& op) noexcept;
    void log(const std::string& message) noexcept;
    void writeText(const std::string& name, const std::string& content) noexcept;
    std::vector<std::pair<std::string, std::string>> collectState() noexcept;
    std::string collectWindows() noexcept;
    void finish() noexcept;
};

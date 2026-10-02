#include "OFS_UiDriver.h"

#include "OpenFunscripter.h"
#include "OFS_Util.h"
#include "OFS_GL.h"

#include "state/OpenFunscripterState.h"
#include "state/SimulatorState.h"
#include "state/states/KeybindingState.h"
#include "state/states/BaseOverlayState.h"
#include "state/states/ChapterState.h"
#include "state/ProjectState.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "SDL.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

static constexpr int32_t ModShift = 1 << 0;
static constexpr int32_t ModCtrl = 1 << 1;
static constexpr int32_t ModAlt = 1 << 2;

bool OFS_UiDriver::Enabled() noexcept
{
    const char* script = SDL_getenv("OFS_UITEST_SCRIPT");
    return script != nullptr && script[0] != '\0';
}

bool OFS_UiDriver::ShowsWindow() noexcept
{
    const char* show = SDL_getenv("OFS_UITEST_SHOW");
    return show != nullptr && show[0] != 0 && show[0] != '0';
}

std::unique_ptr<OFS_UiDriver> OFS_UiDriver::FromEnvironment() noexcept
{
    if (!Enabled()) return nullptr;

    // Copied straight away: on Windows SDL_getenv hands back the same buffer
    // every call, so a second lookup overwrites the first.
    auto readEnv = [](const char* name) {
        const char* value = SDL_getenv(name);
        return std::string(value != nullptr ? value : "");
    };
    const std::string scriptPath = readEnv("OFS_UITEST_SCRIPT");
    const std::string outPath = readEnv("OFS_UITEST_OUT");
    const std::string mediaPath = readEnv("OFS_UITEST_MEDIA");

    auto driver = std::unique_ptr<OFS_UiDriver>(new OFS_UiDriver());
    driver->outDir = Util::PathFromString(outPath.empty() ? std::string(".") : outPath);
    Util::CreateDirectories(driver->outDir);

    std::ifstream file(Util::PathFromString(scriptPath));
    if (!file) {
        driver->log("could not read script " + scriptPath);
        driver->ops.push_back(Op{ OpType::Quit });
        return driver;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (!driver->parse(buffer.str(), mediaPath)) {
        // Parse errors are already logged. Running half a script would only
        // produce results that look meaningful and are not.
        driver->ops.clear();
        driver->ops.push_back(Op{ OpType::Quit });
    }
    driver->log(std::string("script ") + scriptPath + ", " + std::to_string(driver->ops.size()) + " steps");
    return driver;
}

static bool parseButton(const std::string& name, int32_t& out) noexcept
{
    if (name.empty() || name == "left") { out = ImGuiMouseButton_Left; return true; }
    if (name == "right") { out = ImGuiMouseButton_Right; return true; }
    if (name == "middle") { out = ImGuiMouseButton_Middle; return true; }
    return false;
}

static bool parseModifier(const std::string& name, int32_t& out) noexcept
{
    if (name == "shift") { out = ModShift; return true; }
    if (name == "ctrl") { out = ModCtrl; return true; }
    if (name == "alt") { out = ModAlt; return true; }
    return false;
}

static bool parseKey(const std::string& name, ImGuiKey& out) noexcept
{
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') { out = (ImGuiKey)(ImGuiKey_A + (c - 'a')); return true; }
        if (c >= '0' && c <= '9') { out = (ImGuiKey)(ImGuiKey_0 + (c - '0')); return true; }
    }
    // The keypad carries OFS's default bindings for placing a point at a
    // position, keypad0 for 0 through keypad9 for 90 and keypaddivide for 100.
    if (name.size() == 7 && name.compare(0, 6, "keypad") == 0 && name[6] >= '0' && name[6] <= '9') {
        out = (ImGuiKey)(ImGuiKey_Keypad0 + (name[6] - '0'));
        return true;
    }
    if (name == "keypaddivide") { out = ImGuiKey_KeypadDivide; return true; }
    if (name == "keypadmultiply") { out = ImGuiKey_KeypadMultiply; return true; }
    if (name == "keypadadd") { out = ImGuiKey_KeypadAdd; return true; }
    if (name == "keypadsubtract") { out = ImGuiKey_KeypadSubtract; return true; }
    if (name == "keypadenter") { out = ImGuiKey_KeypadEnter; return true; }
    if (name.size() >= 2 && name[0] == 'f') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) { out = (ImGuiKey)(ImGuiKey_F1 + n - 1); return true; }
    }
    static const std::pair<const char*, ImGuiKey> named[] = {
        { "space", ImGuiKey_Space }, { "enter", ImGuiKey_Enter }, { "escape", ImGuiKey_Escape },
        { "delete", ImGuiKey_Delete }, { "backspace", ImGuiKey_Backspace }, { "tab", ImGuiKey_Tab },
        { "left", ImGuiKey_LeftArrow }, { "right", ImGuiKey_RightArrow },
        { "up", ImGuiKey_UpArrow }, { "down", ImGuiKey_DownArrow },
        { "home", ImGuiKey_Home }, { "end", ImGuiKey_End },
        { "pageup", ImGuiKey_PageUp }, { "pagedown", ImGuiKey_PageDown },
        { "minus", ImGuiKey_Minus }, { "equal", ImGuiKey_Equal },
        { "comma", ImGuiKey_Comma }, { "period", ImGuiKey_Period },
    };
    for (auto& entry : named) {
        if (name == entry.first) { out = entry.second; return true; }
    }
    return false;
}

bool OFS_UiDriver::parse(const std::string& script, const std::string& media) noexcept
{
    std::istringstream lines(script);
    std::string line;
    int32_t lineNumber = 0;
    bool ok = true;

    auto fail = [&](const std::string& why) {
        log("line " + std::to_string(lineNumber) + ": " + why + ": " + line);
        ok = false;
    };
    auto push = [&](Op op) {
        op.line = lineNumber;
        ops.push_back(std::move(op));
    };
    auto frames = [&](int32_t n) {
        Op op;
        op.type = OpType::Frames;
        op.n = n;
        push(op);
    };
    auto mouseMove = [&](float x, float y) {
        Op op;
        op.type = OpType::MouseMove;
        op.x = x;
        op.y = y;
        push(op);
    };
    auto mouseButton = [&](int32_t button, bool down) {
        Op op;
        op.type = OpType::MouseButton;
        op.n = button;
        op.down = down;
        push(op);
    };

    while (std::getline(lines, line)) {
        lineNumber += 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const auto mediaAt = line.find("$MEDIA");
        if (mediaAt != std::string::npos) line.replace(mediaAt, 6, media);
        // The run's own output directory, for files a test writes and reads
        // back, so no test carries a path from one machine.
        const auto outAt = line.find("$OUT");
        if (outAt != std::string::npos) line.replace(outAt, 4, outDir.u8string());

        std::istringstream words(line);
        std::string command;
        if (!(words >> command) || command[0] == '#') continue;

        // The rest of the line, for commands that take free text.
        std::string rest;
        std::getline(words >> std::ws, rest);
        std::istringstream args(rest);

        if (command == "wait") {
            int32_t n = 0;
            if (!(args >> n) || n < 0) { fail("wait needs a frame count"); continue; }
            frames(n);
        }
        else if (command == "waituntil") {
            // waituntil KEY =|>=|<= VALUE [MAXFRAMES]: hold the script until the
            // state dump agrees, for things that take however long they take.
            Op op;
            op.type = OpType::WaitUntil;
            std::string cmp, value;
            if (!(args >> op.text >> cmp >> value) || (cmp != "=" && cmp != ">=" && cmp != "<=")) {
                fail("waituntil needs KEY =|>=|<= VALUE [MAXFRAMES]");
                continue;
            }
            int32_t maxFrames = 3000;
            if (args >> maxFrames) op.n = maxFrames;
            else op.n = 3000;
            op.text2 = cmp + " " + value;
            push(op);
        }
        else if (command == "size") {
            Op op;
            op.type = OpType::Size;
            if (!(args >> op.x >> op.y)) { fail("size needs a width and height"); continue; }
            push(op);
            frames(10);
        }
        else if (command == "open") {
            if (rest.empty()) { fail("open needs a path"); continue; }
            Op op;
            op.type = OpType::Open;
            op.text = rest;
            push(op);
            frames(2);
        }
        else if (command == "newblank") {
            Op op;
            op.type = OpType::NewBlank;
            if (!(args >> op.x)) { fail("newblank needs SECONDS PATH"); continue; }
            std::getline(args >> std::ws, op.text);
            if (op.text.empty()) { fail("newblank needs SECONDS PATH"); continue; }
            push(op);
            frames(2);
        }
        else if (command == "move") {
            float x, y;
            if (!(args >> x >> y)) { fail("move needs X Y"); continue; }
            mouseMove(x, y);
            frames(2);
        }
        else if (command == "lanemove") {
            Op op;
            op.type = OpType::LaneMove;
            if (!(args >> op.x >> op.y)) { fail("lanemove needs seconds and a position"); continue; }
            push(op);
            frames(2);
        }
        else if (command == "pointmove") {
            Op op;
            op.type = OpType::PointMove;
            if (!(args >> op.n)) { fail("pointmove needs an action index"); continue; }
            // Optional pixel offset from the point; zero when left out.
            if (!(args >> op.x >> op.y)) { op.x = 0.f; op.y = 0.f; }
            push(op);
            frames(2);
        }
        else if (command == "tap") {
            std::string buttonName;
            args >> buttonName;
            int32_t button;
            if (!parseButton(buttonName, button)) { fail("unknown mouse button"); continue; }
            mouseButton(button, true);
            frames(1);
            mouseButton(button, false);
            frames(2);
        }
        else if (command == "click" || command == "dclick") {
            float x, y;
            std::string buttonName;
            if (!(args >> x >> y)) { fail("click needs X Y"); continue; }
            args >> buttonName;
            int32_t button;
            if (!parseButton(buttonName, button)) { fail("unknown mouse button"); continue; }
            mouseMove(x, y);
            frames(2);
            const int32_t clicks = command == "dclick" ? 2 : 1;
            for (int32_t i = 0; i < clicks; i += 1) {
                mouseButton(button, true);
                frames(1);
                mouseButton(button, false);
                frames(1);
            }
            frames(2);
        }
        else if (command == "drag") {
            float x1, y1, x2, y2;
            int32_t steps = 12;
            if (!(args >> x1 >> y1 >> x2 >> y2)) { fail("drag needs X1 Y1 X2 Y2"); continue; }
            args >> steps;
            if (steps < 1) steps = 1;
            mouseMove(x1, y1);
            frames(2);
            mouseButton(ImGuiMouseButton_Left, true);
            frames(2);
            for (int32_t i = 1; i <= steps; i += 1) {
                const float t = (float)i / (float)steps;
                mouseMove(x1 + ((x2 - x1) * t), y1 + ((y2 - y1) * t));
                frames(1);
            }
            frames(1);
            mouseButton(ImGuiMouseButton_Left, false);
            frames(2);
        }
        else if (command == "press" || command == "release") {
            std::string buttonName;
            args >> buttonName;
            int32_t button;
            if (!parseButton(buttonName, button)) { fail("unknown mouse button"); continue; }
            mouseButton(button, command == "press");
            frames(1);
        }
        else if (command == "wheel") {
            Op op;
            op.type = OpType::Wheel;
            if (!(args >> op.y)) { fail("wheel needs an amount"); continue; }
            push(op);
            frames(2);
        }
        else if (command == "key") {
            std::string chord;
            args >> chord;
            std::vector<std::string> parts;
            std::stringstream chordStream(chord);
            std::string part;
            while (std::getline(chordStream, part, '+')) parts.push_back(part);
            if (parts.empty()) { fail("key needs a key"); continue; }

            ImGuiKey key;
            if (!parseKey(parts.back(), key)) { fail("unknown key " + parts.back()); continue; }
            std::vector<int32_t> mods;
            bool modsOk = true;
            for (size_t i = 0; i + 1 < parts.size(); i += 1) {
                int32_t mod;
                if (!parseModifier(parts[i], mod)) { modsOk = false; break; }
                mods.push_back(mod);
            }
            if (!modsOk) { fail("unknown modifier"); continue; }

            for (auto mod : mods) {
                Op op;
                op.type = OpType::Modifier;
                op.n = mod;
                op.down = true;
                push(op);
            }
            if (!mods.empty()) frames(1);
            Op down;
            down.type = OpType::Key;
            down.n = key;
            down.down = true;
            push(down);
            frames(1);
            Op up = down;
            up.down = false;
            push(up);
            frames(1);
            for (auto mod : mods) {
                Op op;
                op.type = OpType::Modifier;
                op.n = mod;
                op.down = false;
                push(op);
            }
            frames(2);
        }
        else if (command == "hold" || command == "let") {
            std::string name;
            args >> name;
            Op op;
            op.type = OpType::Modifier;
            if (!parseModifier(name, op.n)) { fail("unknown modifier"); continue; }
            op.down = command == "hold";
            push(op);
            frames(1);
        }
        else if (command == "type") {
            Op op;
            op.type = OpType::Text;
            op.text = rest;
            push(op);
            frames(2);
        }
        else if (command == "action") {
            Op op;
            op.type = OpType::Action;
            if (!(args >> op.text)) { fail("action needs an id"); continue; }
            push(op);
            frames(2);
        }
        else if (command == "simaxis") {
            // Holds one of the simulator's axes at a position, 0 to 100, as if
            // a script drove it; -1 hands it back to its script.
            Op op;
            op.type = OpType::SimAxis;
            if (!(args >> op.text >> op.x)) { fail("simaxis needs an axis name and a position, or -1"); continue; }
            push(op);
            frames(2);
        }
        else if (command == "seek") {
            Op op;
            op.type = OpType::Seek;
            if (!(args >> op.x)) { fail("seek needs seconds"); continue; }
            push(op);
            frames(10);
        }
        else if (command == "pause" || command == "play") {
            Op op;
            op.type = command == "pause" ? OpType::Pause : OpType::Play;
            push(op);
            frames(2);
        }
        else if (command == "shot" || command == "state" || command == "windows") {
            Op op;
            op.type = command == "shot" ? OpType::Shot : (command == "state" ? OpType::State : OpType::Windows);
            if (!(args >> op.text)) { fail(command + " needs a name"); continue; }
            push(op);
            frames(1);
        }
        else if (command == "expect") {
            Op op;
            op.type = OpType::Expect;
            if (!(args >> op.text)) { fail("expect needs a key"); continue; }
            std::getline(args >> std::ws, op.text2);
            push(op);
        }
        else if (command == "note") {
            Op op;
            op.type = OpType::Note;
            op.text = rest;
            push(op);
        }
        else if (command == "quit") {
            push(Op{ OpType::Quit });
        }
        else {
            fail("unknown command " + command);
        }
    }
    return ok;
}

// Every coordinate in a script is a pixel of the size it asked for, so the
// window is put back to that size whenever it is not. Checked every frame,
// not only when the script sizes it: FancyZones snapped the window into half
// of the screen after it had been sized, and every click landed somewhere
// else. Sizes are asked for in pixels and the window is sized in display
// units, which differ under display scaling, so it is sized by the ratio.
void OFS_UiDriver::holdWindowSize() noexcept
{
    if (wantWidth <= 0 || wantHeight <= 0) return;
    auto app = OpenFunscripter::ptr;
    int pixelsW = 0, pixelsH = 0;
    SDL_GL_GetDrawableSize(app->window, &pixelsW, &pixelsH);
    if (pixelsW == wantWidth && pixelsH == wantHeight) return;

    int unitsW = 0, unitsH = 0;
    SDL_GetWindowSize(app->window, &unitsW, &unitsH);
    const float scale = pixelsW > 0 && unitsW > 0 ? (float)pixelsW / (float)unitsW : 1.f;
    SDL_RestoreWindow(app->window);
    SDL_SetWindowPosition(app->window, 40, 40);
    SDL_SetWindowSize(app->window,
        (int)std::round((float)wantWidth / scale), (int)std::round((float)wantHeight / scale));
}

void OFS_UiDriver::BeforeNewFrame() noexcept
{
    auto app = OpenFunscripter::ptr;
    ImGuiIO& io = ImGui::GetIO();
    frame += 1;
    frameStartCounter = SDL_GetPerformanceCounter();

    // A scripted run is never idle; idling drops to ten frames a second and
    // makes every wait ten times as long.
    app->IdleTimer = SDL_GetTicks();
    app->setIdle(false);
    holdWindowSize();
    // Every frame, because a run has no business on the screen and several
    // things put it back there: restoring it to set its size, maximizing it at
    // startup, and the backend raising it. Doing it once at startup was not
    // enough, and the windows appeared anyway.
    if (!ShowsWindow() && (SDL_GetWindowFlags(app->window) & SDL_WINDOW_SHOWN)) {
        SDL_HideWindow(app->window);
    }

    // A scripted run behaves as if it always has focus. ImGui lets go of every
    // held button when the window loses it, and with several runs side by side
    // each new window takes focus as it opens, which dropped clicks mid-press.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = g.InputEventsQueue.Size - 1; i >= 0; i -= 1) {
        if (g.InputEventsQueue[i].Type == ImGuiInputEventType_Focus) {
            g.InputEventsQueue.erase(g.InputEventsQueue.Data + i);
        }
    }
    io.AppFocusLost = false;

    if (mouseX >= 0.f) io.AddMousePosEvent(mouseX, mouseY);

    if (finished) return;
    if (waitFrames > 0) {
        waitFrames -= 1;
        return;
    }

    while (nextOp < ops.size()) {
        const Op& op = ops[nextOp];
        nextOp += 1;
        if (op.type == OpType::WaitUntil) {
            if (!conditionMet(op)) {
                if (waitUntilFrames < op.n) {
                    waitUntilFrames += 1;
                    nextOp -= 1;
                    return;
                }
                log("line " + std::to_string(op.line) + ": FAIL waituntil " + op.text + " " + op.text2
                    + " timed out after " + std::to_string(op.n) + " frames");
                failed += 1;
            }
            waitUntilFrames = 0;
            continue;
        }
        if (op.type == OpType::Frames) {
            // This frame counts as the first of them.
            waitFrames = op.n > 0 ? op.n - 1 : 0;
            if (op.n > 0) return;
            continue;
        }
        execute(op);
        if (finished) return;
    }
    finish();
}

void OFS_UiDriver::execute(const Op& op) noexcept
{
    auto app = OpenFunscripter::ptr;
    ImGuiIO& io = ImGui::GetIO();

    switch (op.type) {
        case OpType::Size:
            wantWidth = (int32_t)op.x;
            wantHeight = (int32_t)op.y;
            holdWindowSize();
            break;
        case OpType::Open:
            log("open " + op.text);
            app->openFile(op.text);
            break;
        case OpType::NewBlank:
            log("newblank " + op.text);
            app->startStandaloneProject(op.text, op.x);
            break;
        case OpType::MouseMove:
            mouseX = op.x;
            mouseY = op.y;
            io.AddMousePosEvent(mouseX, mouseY);
            break;
        case OpType::LaneMove:
        case OpType::PointMove: {
            const auto& lanes = app->scriptTimeline.Lanes;
            const int32_t laneIdx = (int32_t)app->LoadedProject->ActiveIdx();
            if (laneIdx < 0 || laneIdx >= (int32_t)lanes.size() || lanes[laneIdx].canvasSize.x <= 0.f
                || lanes[laneIdx].visibleTime <= 0.f) {
                log("line " + std::to_string(op.line) + ": FAIL the active script has no lane on screen");
                failed += 1;
                break;
            }
            const auto& lane = lanes[laneIdx];

            float atS = op.x;
            float pos = op.y;
            float offsetX = 0.f;
            float offsetY = 0.f;
            if (op.type == OpType::PointMove) {
                auto script = app->ActiveFunscript();
                if (op.n < 0 || op.n >= (int32_t)script->Actions().size()) {
                    log("line " + std::to_string(op.line) + ": FAIL no point " + std::to_string(op.n)
                        + ", the script has " + std::to_string(script->Actions().size()));
                    failed += 1;
                    break;
                }
                atS = script->Actions()[op.n].atS;
                pos = script->Actions()[op.n].pos;
                offsetX = op.x;
                offsetY = op.y;
            }

            const float targetX = lane.canvasPos.x + ((atS - lane.offsetTime) / lane.visibleTime) * lane.canvasSize.x + offsetX;
            const float targetY = lane.canvasPos.y + (lane.canvasSize.y * (1.f - (pos / 100.f))) + offsetY;

            // A time outside the span the lane is showing lands off the lane,
            // usually off the window, and the click that follows silently hits
            // nothing. That once passed for a menu bug, so it fails loudly.
            if (targetX < lane.canvasPos.x || targetX > lane.canvasPos.x + lane.canvasSize.x) {
                char span[96];
                stbsp_snprintf(span, sizeof(span), "%.2fs is off the lane, which shows %.2fs to %.2fs",
                    atS, lane.offsetTime, lane.offsetTime + lane.visibleTime);
                log("line " + std::to_string(op.line) + ": FAIL " + span + "; seek first");
                failed += 1;
                break;
            }

            mouseX = targetX;
            mouseY = targetY;
            io.AddMousePosEvent(mouseX, mouseY);
            break;
        }
        case OpType::MouseButton:
            io.AddMouseButtonEvent(op.n, op.down);
            break;
        case OpType::Wheel:
            io.AddMouseWheelEvent(0.f, op.y);
            // The timeline zooms from the SDL wheel event, not from ImGui.
            {
                SDL_Event ev;
                SDL_zero(ev);
                ev.type = SDL_MOUSEWHEEL;
                ev.wheel.y = (Sint32)op.y;
                SDL_PushEvent(&ev);
            }
            break;
        case OpType::Key:
            io.AddKeyEvent((ImGuiKey)op.n, op.down);
            break;
        case OpType::Modifier: {
            if (op.down) heldModifiers |= op.n;
            else heldModifiers &= ~op.n;
            if (op.n == ModShift) io.AddKeyEvent(ImGuiMod_Shift, op.down);
            if (op.n == ModCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, op.down);
            if (op.n == ModAlt) io.AddKeyEvent(ImGuiMod_Alt, op.down);
            int sdlMods = KMOD_NONE;
            if (heldModifiers & ModShift) sdlMods |= KMOD_LSHIFT;
            if (heldModifiers & ModCtrl) sdlMods |= KMOD_LCTRL;
            if (heldModifiers & ModAlt) sdlMods |= KMOD_LALT;
            SDL_SetModState((SDL_Keymod)sdlMods);
            break;
        }
        case OpType::Text:
            io.AddInputCharactersUTF8(op.text.c_str());
            break;
        case OpType::Action:
            if (!app->keys->Invoke(op.text.c_str())) {
                log("line " + std::to_string(op.line) + ": FAIL no action " + op.text);
                failed += 1;
            }
            break;
        case OpType::SimAxis: {
            static const char* names[] = { "stroke", "surge", "sway", "twist", "roll", "pitch" };
            int32_t idx = -1;
            for (int32_t i = 0; i < 6; i += 1) {
                if (op.text == names[i]) idx = i;
            }
            if (idx < 0) {
                log("line " + std::to_string(op.line) + ": FAIL no simulator axis " + op.text);
                failed += 1;
            }
            else {
                app->simulator.TestAxisOverride[idx] = op.x;
            }
            break;
        }
        case OpType::Seek:
            app->player->SetPositionExact(op.x);
            break;
        case OpType::Pause:
            app->player->SetPaused(true);
            break;
        case OpType::Play:
            app->player->SetPaused(false);
            break;
        case OpType::Shot:
            pendingShot = op.text;
            break;
        case OpType::State: {
            std::string content;
            for (auto& entry : collectState()) content += entry.first + "=" + entry.second + "\n";
            writeText(op.text + ".state", content);
            break;
        }
        case OpType::Windows:
            writeText(op.text + ".windows", collectWindows());
            break;
        case OpType::Expect: {
            std::string actual = "(missing)";
            for (auto& entry : collectState()) {
                if (entry.first == op.text) { actual = entry.second; break; }
            }
            const bool pass = actual == op.text2;
            if (pass) passed += 1;
            else failed += 1;
            log("line " + std::to_string(op.line) + ": " + (pass ? "PASS " : "FAIL ") + op.text
                + " want \"" + op.text2 + "\" got \"" + actual + "\"");
            break;
        }
        case OpType::Note:
            log("note " + op.text);
            break;
        case OpType::Quit:
            finish();
            break;
        case OpType::Frames:
        case OpType::WaitUntil:
            break;
    }
}

bool OFS_UiDriver::conditionMet(const Op& op) noexcept
{
    const auto space = op.text2.find(' ');
    const std::string cmp = op.text2.substr(0, space);
    const std::string want = space == std::string::npos ? std::string() : op.text2.substr(space + 1);
    for (auto& entry : collectState()) {
        if (entry.first != op.text) continue;
        if (cmp == "=") return entry.second == want;
        const double have = std::atof(entry.second.c_str());
        const double target = std::atof(want.c_str());
        if (cmp == ">=") return have >= target;
        if (cmp == "<=") return have <= target;
        return false;
    }
    return false;
}

void OFS_UiDriver::AfterRender() noexcept
{
    if (frameStartCounter != 0) {
        const uint64_t elapsed = SDL_GetPerformanceCounter() - frameStartCounter;
        const float ms = (float)((double)elapsed * 1000.0 / (double)SDL_GetPerformanceFrequency());
        // A window of the last few seconds, so a report says what the app is
        // doing now rather than averaging in how long it took to start.
        if (frameMilliseconds.size() >= 600) frameMilliseconds.erase(frameMilliseconds.begin());
        frameMilliseconds.push_back(ms);
    }
    if (pendingShot.empty()) return;
    auto app = OpenFunscripter::ptr;

    int width = 0;
    int height = 0;
    SDL_GL_GetDrawableSize(app->window, &width, &height);
    if (width <= 0 || height <= 0) return;

    std::vector<uint8_t> pixels((size_t)width * (size_t)height * 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    const auto path = (outDir / (pendingShot + ".png")).u8string();
    // The default level spends about a tenth of a second on each shot, which
    // is most of the run time of a test that takes a few hundred. Size does
    // not matter here, so the lightest level is used, for these files only.
    const int savedLevel = stbi_write_png_compression_level;
    stbi_write_png_compression_level = 1;
    Util::SavePNG(path, pixels.data(), width, height, 3, true);
    stbi_write_png_compression_level = savedLevel;
    log("shot " + pendingShot + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    pendingShot.clear();
}

std::vector<std::pair<std::string, std::string>> OFS_UiDriver::collectState() noexcept
{
    auto app = OpenFunscripter::ptr;
    std::vector<std::pair<std::string, std::string>> state;
    auto add = [&state](const char* key, const std::string& value) { state.emplace_back(key, value); };
    auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    auto number = [](double value, const char* fmt = "%.3f") {
        char buf[64];
        stbsp_snprintf(buf, sizeof(buf), fmt, value);
        return std::string(buf);
    };

    const auto& ofsState = OpenFunscripterState::State(app->stateHandle);
    add("project_valid", flag(app->LoadedProject->IsValid()));
    add("scripts", std::to_string(app->LoadedFunscripts().size()));
    // The axis each script drives, in order, as the simulator and the device
    // link read it from the file names.
    {
        std::string axes;
        for (auto& each : app->LoadedFunscripts()) {
            if (!axes.empty()) axes += ",";
            axes += each->AxisName();
        }
        add("script_axes", axes);
    }
    add("active_index", std::to_string(app->LoadedProject->ActiveIdx()));

    auto script = app->ActiveFunscript();
    add("active_title", script->Title());
    add("actions", std::to_string(script->Actions().size()));
    // The ends of the script, for checks on a script with too many points to
    // write out one by one.
    if (!script->Actions().empty()) {
        add("first_position", std::to_string((int32_t)script->Actions().front().pos));
        add("last_position", std::to_string((int32_t)script->Actions().back().pos));
    }
    // Every position in order on one line, so a whole stroke can be checked
    // with a single expect.
    {
        std::string positions;
        for (const auto& action : script->Actions()) {
            if (!positions.empty()) positions += ",";
            positions += std::to_string(action.pos);
        }
        add("positions", positions);
    }
    add("selection", std::to_string(script->SelectionSize()));
    add("copied", std::to_string(app->CopiedSelection.size()));
    add("undo_empty", flag(app->undoSystem->UndoEmpty()));
    add("redo_empty", flag(app->undoSystem->RedoEmpty()));

    add("player_time", number(app->player->CurrentTime()));
    add("duration", number(app->player->Duration()));
    add("blank", flag(app->player->IsBlank()));
    add("audio_only", flag(app->player->IsAudioOnly()));
    if (frameMilliseconds.size() >= 30) {
        auto sorted = frameMilliseconds;
        std::sort(sorted.begin(), sorted.end());
        double total = 0.0;
        for (float ms : sorted) total += ms;
        add("frame_ms", number((float)(total / (double)sorted.size()), "%.2f"));
        add("frame_ms_worst", number(sorted[(size_t)(sorted.size() * 0.95f)], "%.2f"));
        // What the frame actually asked the GPU to draw, which is the other
        // half of why a frame costs what it does.
        add("render_vertices", std::to_string(ImGui::GetIO().MetricsRenderVertices));
    }
    add("waveform_shown", flag(app->scriptTimeline.WaveformShown()));
    add("waveform_busy", flag(app->scriptTimeline.WaveformBusy()));
    add("waveform_bass", flag(app->scriptTimeline.WaveformIsBass()));
    add("beat_ticks", std::to_string(app->scriptTimeline.BeatTickCount()));
    add("standalone", flag(app->LoadedProject->IsValid() && app->LoadedProject->IsStandalone()));
    add("project_path", app->LoadedProject->Path());
    add("paused", flag(app->player->IsPaused()));
    add("speed", number(app->player->CurrentSpeed(), "%.2f"));
    // How many seconds the active lane shows, which the mouse wheel zooms.
    {
        const auto& lanes = app->scriptTimeline.Lanes;
        const int32_t laneIdx = (int32_t)app->LoadedProject->ActiveIdx();
        add("visible_time", laneIdx >= 0 && laneIdx < (int32_t)lanes.size()
            ? number(lanes[laneIdx].visibleTime, "%.2f") : std::string());
    }

    add("scripting_mode", std::to_string((int32_t)app->scripting->ActiveMode()));
    add("overlay_mode", std::to_string((int32_t)app->scripting->ActiveOverlay()));
    add("tempo_division", std::to_string(TempoOverlayState::State(
        OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName)).measureIndex));
    const auto& overlay = BaseOverlay::State();
    add("snap_grid", flag(overlay.SnapToGrid));
    add("snap_position_step", std::to_string(overlay.SnapPositionStep));
    add("spline", flag(overlay.SplineMode));
    add("show_lines", flag(BaseOverlay::ShowLines));
    add("show_points", flag(BaseOverlay::ShowPoints));

    // Every binding of a few actions, to check that a rebinding took.
    {
        const auto& keysState = OFS_KeybindingState::StateSlow();
        for (const char* id : { "action_0", "action_10", "action_50", "action_100", "remove_action" }) {
            std::string text;
            for (const auto& trigger : keysState.Triggers) {
                if (trigger.MappedActionId != id) continue;
                if (!text.empty()) text += "|";
                if (trigger.Mod & ImGuiMod_Ctrl) text += "Ctrl+";
                if (trigger.Mod & ImGuiMod_Shift) text += "Shift+";
                if (trigger.Mod & ImGuiMod_Alt) text += "Alt+";
                text += ImGui::GetKeyName(trigger.ImKey());
            }
            add((std::string("bindings:") + id).c_str(), text);
        }
    }
    add("simulator_mode", std::to_string((int32_t)app->simulator.CurrentMode()));
    {
        const auto& sim = SimulatorState::State(app->simulator.StateHandle());
        add("sim_locked", flag(sim.LockedPosition));
        add("sim_drawn", flag(app->simulator.LastDrawnFrame >= ImGui::GetFrameCount() - 1));
        add("sim_fit", flag(sim.FitToPlayer));
        add("sim_p1", number(sim.P1.x, "%.0f") + "," + number(sim.P1.y, "%.0f"));
        add("sim_p2", number(sim.P2.x, "%.0f") + "," + number(sim.P2.y, "%.0f"));
        const auto& fmin = app->simulator.FrameMin;
        const auto& fmax = app->simulator.FrameMax;
        add("sim_frame", number(fmin.x, "%.0f") + "," + number(fmin.y, "%.0f") + ","
            + number(fmax.x, "%.0f") + "," + number(fmax.y, "%.0f"));
        add("camera_yaw", number(sim.CameraYaw, "%.0f"));
        add("camera_elevation", number(sim.CameraElevation, "%.0f"));
    }
    add("finish_enabled", flag(app->simulator.FinishEasterEgg));
    add("finish_stimulation", number(app->simulator.FinishStimulation, "%.2f"));
    add("finish_distension", number(app->simulator.UterusDistension, "%.2f"));
    add("finish_canal", number(app->simulator.FinishCanalVolume * 10000.f, "%.2f"));
    add("finish_tilt", number(app->simulator.poolTiltX, "%.2f") + "," + number(app->simulator.poolTiltZ, "%.2f"));
    add("finish_count", std::to_string(app->simulator.FinishCount));
    add("finish_drops", std::to_string(app->simulator.FinishDrops.size()));
    add("finish_pile", number(app->simulator.FinishPileFraction, "%.2f"));
    // Every drop, for looking into where one has got to: kind, phase, height.
    for (auto& drop : app->simulator.FinishDrops) {
        add("finish_drop", std::to_string(drop.rope) + "," + std::to_string(drop.phase) + "," + number(drop.y, "%.3f"));
    }

    // Chapters, for the tempo detection tests: how many, and each one's
    // measured tempo, rounded, or "break" for a stretch with no music.
    {
        auto& chapters = app->chapterMgr->State().chapters;
        add("chapters", std::to_string(chapters.size()));
        for (const auto& chapter : chapters) {
            add("chapter", chapter.isBreak
                ? std::string("break")
                : number(chapter.bpm, "%.0f") + "," + number(chapter.startTime, "%.1f") + "," + number(chapter.endTime, "%.1f"));
            add("chapter_name", chapter.name);
        }
    }
    add("show_toolbar", flag(ofsState.showToolbar));
    add("show_simulator", flag(ofsState.showSimulator));
    add("show_statistics", flag(ofsState.showStatistics));
    add("show_special_functions", flag(ofsState.showSpecialFunctions));
    add("show_action_editor", flag(ofsState.showActionEditor));
    add("show_chapter_manager", flag(ofsState.showChapterManager));
    add("show_script_check", flag(ofsState.showScriptCheck));
    add("show_devices", flag(ofsState.showDevices));
    add("device_status", std::to_string((int32_t)app->deviceLink->CurrentStatus()));
    add("device_count", std::to_string(app->deviceLink->DeviceCount()));
    add("device_moves", std::to_string(app->deviceLink->MovesSent()));
    add("device_channels", app->deviceLink->ChannelsSent());
    add("device_axes", app->deviceLink->AxisReport());
    add("device_axes_home", flag(app->deviceLink->AllAxesHome()));
    add("device_stroke", std::to_string(app->deviceLink->StrokeValue()));
    if (app->scriptCheck->HasChecked()) {
        add("check_problems", std::to_string(app->scriptCheck->ProblemCount()));
    }
    add("preferences_open", flag(app->preferences->ShowWindow));

    auto modal = ImGui::GetTopMostPopupModal();
    add("modal", modal != nullptr ? std::string(modal->Name) : std::string());
    ImGuiContext& g = *GImGui;
    add("hovered_window", g.HoveredWindow != nullptr ? std::string(g.HoveredWindow->Name) : std::string());
    add("open_popups", std::to_string(g.OpenPopupStack.Size));
    // Keyboard navigation state, which decides whether ImGui listens to the
    // mouse at all: while navigation has disabled mouse hover, nothing can be
    // hovered and so nothing can be clicked.
    add("nav_window", g.NavWindow != nullptr ? std::string(g.NavWindow->Name) : std::string());
    add("nav_layer", std::to_string((int32_t)g.NavLayer));
    add("nav_disable_mouse_hover", flag(g.NavDisableMouseHover));
    add("hovered_id", std::to_string(g.HoveredId));
    add("active_id", std::to_string(g.ActiveId));
    add("mouse", number(g.IO.MousePos.x, "%.0f") + "," + number(g.IO.MousePos.y, "%.0f"));
    // Modifiers as ImGui holds them. A binding only fires when these match its
    // own exactly, so a modifier that never let go blocks every plain key.
    add("key_mods", std::to_string((int32_t)g.IO.KeyMods));
    add("want_capture_keyboard", flag(g.IO.WantCaptureKeyboard));

    for (size_t i = 0; i < app->LoadedFunscripts().size(); i += 1) {
        auto& each = app->LoadedFunscripts()[i];
        add("script", each->Title() + "," + flag(each->Enabled) + "," + std::to_string(each->Actions().size()) + "," + flag(each->Targeted));
        // Every script's positions by title, so what a tool wrote into a
        // script other than the active one can be checked.
        std::string positions;
        for (const auto& action : each->Actions()) {
            if (!positions.empty()) positions += ",";
            positions += std::to_string(action.pos);
        }
        add(("positions:" + each->Title()).c_str(), positions);
    }
    // Enough points to check an edit landed where it should, without the dump
    // becoming the size of a long script.
    constexpr size_t MaxListed = 64;
    for (size_t i = 0; i < script->Actions().size() && i < MaxListed; i += 1) {
        const auto& action = script->Actions()[i];
        add("action", number(action.atS) + "," + std::to_string(action.pos));
    }
    for (size_t i = 0; i < script->Selection().size() && i < MaxListed; i += 1) {
        const auto& action = script->Selection()[i];
        add("selected", number(action.atS) + "," + std::to_string(action.pos));
    }
    return state;
}

std::string OFS_UiDriver::collectWindows() noexcept
{
    std::string out;
    char buf[512];
    ImGuiContext& g = *GImGui;
    for (ImGuiWindow* window : g.Windows) {
        if (!window->WasActive || window->Hidden) continue;
        stbsp_snprintf(buf, sizeof(buf), "%.0f %.0f %.0f %.0f\t%s%s\n",
            window->Pos.x, window->Pos.y, window->Size.x, window->Size.y, window->Name,
            (window->Flags & ImGuiWindowFlags_Popup) ? "\t(popup)" : "");
        out += buf;
    }
    return out;
}

void OFS_UiDriver::log(const std::string& message) noexcept
{
    const auto path = (outDir / "driver.log").u8string();
    FILE* file = std::fopen(path.c_str(), "a");
    if (file == nullptr) return;
    std::fprintf(file, "[%6llu] %s\n", (unsigned long long)frame, message.c_str());
    std::fclose(file);
}

void OFS_UiDriver::writeText(const std::string& name, const std::string& content) noexcept
{
    const auto path = (outDir / (name + ".txt")).u8string();
    FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) return;
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
}

void OFS_UiDriver::finish() noexcept
{
    if (finished) return;
    finished = true;
    log("done: " + std::to_string(passed) + " passed, " + std::to_string(failed) + " failed");
    SDL_SetModState(KMOD_NONE);
    // Forced, so an unsaved test project cannot stop the run on a prompt.
    OpenFunscripter::ptr->exitApp(true);
}

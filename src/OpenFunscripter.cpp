#include "OpenFunscripter.h"
#include "OFS_GitVersion.h"
#include "OFS_CrashHandler.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"
#include "OFS_ImGui.h"
#include "GradientBar.h"
#include "FunscriptHeatmap.h"
#include "OFS_DownloadFfmpeg.h"
#include "OFS_Shader.h"
#include "OFS_MpvLoader.h"
#include "OFS_Localization.h"

#include "state/OpenFunscripterState.h"
#include "api/OFS_TCodeMachines.h"
#include "state/states/VideoplayerWindowState.h"
#include "state/states/BaseOverlayState.h"
#include "state/states/ChapterState.h"
#include "state/SimulatorState.h"

#include <filesystem>

#include "stb_sprintf.h"

#include "imgui_stdlib.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include "SDL.h"
#include "asap.h"
#include "OFS_GL.h"

// TODO: Use ImGui tables API in keybinding UI
// TODO: extend "range extender" functionality ( only extend bottom/top, range reducer )
// TODO: render simulator relative to video position & zoom
// TODO: make speed coloring configurable

OpenFunscripter* OpenFunscripter::ptr = nullptr;
static constexpr const char* GlslVersion = "#version 330 core";

static ImGuiID MainDockspaceID;
static UndoContextScripts undoContextFor(const std::vector<std::shared_ptr<Funscript>>& targets) noexcept;
static constexpr const char* StatisticsWindowId = "###STATISTICS";
static constexpr const char* ActionEditorWindowId = "###ACTION_EDITOR";
static constexpr const char* ToolbarWindowId = "Toolbar###TOOLBAR";
static constexpr const char* TracksWindowId = "Tracks###TRACKS";

static constexpr int DefaultWidth = 1920;
static constexpr int DefaultHeight = 1080;

static constexpr int AutoBackupIntervalSeconds = 60;

bool OpenFunscripter::imguiSetup() noexcept
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows
    if (OFS_UiDriver::Enabled()) {
        // Everything in the one window, so a screenshot of it shows everything
        // and script coordinates are relative to it.
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    }
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigViewportsNoDecoration = false;
    io.ConfigViewportsNoAutoMerge = false;
    io.ConfigViewportsNoTaskBarIcon = false;
    io.ConfigDockingTransparentPayload = true;

    static auto imguiIniPath = Util::Prefpath("imgui.ini");
    io.IniFilename = imguiIniPath.c_str();

    // NOTE: OFS_Preferences::OFS_Preferences() sets OFS_DynFontAtlas::FontOverride
    OFS_DynFontAtlas::Init();
    OFS_Translator::Init();
    auto& prefState = PreferenceState::State(preferences->StateHandle());
    if (!prefState.languageCsv.empty()) {
        if (OFS_Translator::ptr->LoadTranslation(prefState.languageCsv.c_str())) {
            OFS_DynFontAtlas::AddTranslationText();
        }
    }


    // Setup Platform/Renderer bindings
    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    LOGF_DEBUG("init imgui with glsl: %s", GlslVersion);
    ImGui_ImplOpenGL3_Init(GlslVersion);

    // hook into paste for the dynamic atlas
    if (io.GetClipboardTextFn) {
        static auto OriginalSDL2_GetClipboardFunc = io.GetClipboardTextFn;
        io.GetClipboardTextFn = [](void* d) noexcept -> const char* {
            auto clipboard = OriginalSDL2_GetClipboardFunc(d);
            OFS_DynFontAtlas::AddText(clipboard);
            return clipboard;
        };
    }

    return true;
}

static void SaveState() noexcept
{
    auto stateJson = OFS_StateManager::Get()->SerializeAppAll(true);
    auto stateBin = Util::SerializeCBOR(stateJson);
    auto statePath = Util::Prefpath("state.ofs");
    Util::WriteFile(statePath.c_str(), stateBin.data(), stateBin.size());
}

OpenFunscripter::~OpenFunscripter() noexcept
{
    // needs a certain destruction order
    scripting.reset();
    controllerInput.reset();
    specialFunctions.reset();
    LoadedProject.reset();
    playerWindow.reset();
}

bool OpenFunscripter::Init(int argc, char* argv[])
{
    OFS_FileLogger::Init();
    Util::InMainThread();
    Util::InitRandom();

    FUN_ASSERT(!ptr, "there can only be one instance");
    ptr = this;

    auto prefPath = Util::Prefpath("");
    Util::CreateDirectories(prefPath);

    OFS_StateManager::Init();
    {
        auto stateMgr = OFS_StateManager::Get();
        std::vector<uint8_t> fileData;
        auto statePath = Util::Prefpath("state.ofs");
        if (Util::ReadFile(statePath.c_str(), fileData) > 0) {
            bool succ;
            auto cbor = Util::ParseCBOR(fileData, &succ);
            if (succ) {
                stateMgr->DeserializeAppAll(cbor, true);
            }
        }
    }

    stateHandle = OFS_AppState<OpenFunscripterState>::Register(OpenFunscripterState::StateName);
    const auto& ofsState = OpenFunscripterState::State(stateHandle);

    preferences = std::make_unique<OFS_Preferences>();
    const auto& prefState = PreferenceState::State(preferences->StateHandle());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_ERROR(SDL_GetError());
        return false;
    }
    if (!OFS_MpvLoader::Load()) {
        LOG_ERROR("Failed to load mpv library.");
        return false;
    }

#if __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac according to imgui example
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0 /*| SDL_GL_CONTEXT_DEBUG_FLAG*/);
#endif

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    // antialiasing
    // this caused problems in my linux testing
#ifdef WIN32
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 2);
#endif

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow(
        "OFS-SE " OFS_LATEST_GIT_TAG "@" OFS_LATEST_GIT_HASH,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DefaultWidth, DefaultHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);

    SDL_Rect display;
    int windowDisplay = SDL_GetWindowDisplayIndex(window);
    SDL_GetDisplayBounds(windowDisplay, &display);
    // Maximizing shows the window, which a scripted run does not want: it sets
    // its own size and stays off the screen.
    if ((DefaultWidth >= display.w || DefaultHeight >= display.h)
        && (!OFS_UiDriver::Enabled() || OFS_UiDriver::ShowsWindow())) {
        SDL_MaximizeWindow(window);
    }

    glContext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(prefState.vsync);

    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        LOG_ERROR("Failed to load glad.");
        return false;
    }

    if (!imguiSetup()) {
        LOG_ERROR("Failed to setup ImGui");
        return false;
    }

    preferences->SetTheme(static_cast<OFS_Theme>(prefState.currentTheme));

    EV::Init();
    LoadedProject = std::make_unique<OFS_Project>();

    player = std::make_unique<OFS_Videoplayer>(VideoplayerType::Main);
    if (!player->Init(prefState.forceHwDecoding)) {
        LOG_ERROR("Failed to initialize videoplayer.");
        return false;
    }
    player->SetPaused(true);

    playerWindow = std::make_unique<OFS_VideoplayerWindow>();
    if (!playerWindow->Init(player.get())) {
        LOG_ERROR("Failed to init videoplayer window");
        return false;
    }
    // The empty player panel is what someone sees on a first run, so the way
    // to start without a video is offered there as well as in the File menu.
    playerWindow->OnStartBlankProject = [this]() noexcept { ShowNewScriptDialog = true; };

    playerControls.Init(player.get(), prefState.forceHwDecoding);
    undoSystem = std::make_unique<UndoSystem>();

    keys = std::make_unique<OFS_KeybindingSystem>();

    // Edits on the selection, offered on the timeline where the points are.
    // Each entry runs the same command as its keybinding and menu item, so it
    // shares their undo and shows their shortcut.
    scriptTimeline.OnOpenSettings = [this]() noexcept { preferences->OpenTimelineSection(); };
    scriptTimeline.DrawPointActionsMenu = [this]() noexcept {
        // What the edits below will act on: the selection in every edited lane.
        auto edited = TargetedFunscripts();
        uint32_t selected = 0;
        int lanesWithSelection = 0;
        for (auto& script : edited) {
            selected += script->SelectionSize();
            if (script->HasSelection()) lanesWithSelection += 1;
        }
        const bool hasSelection = selected > 0;
        if (edited.size() == 1) {
            if (hasSelection) ImGui::TextDisabled("%u selected in %s", selected, edited.front()->Title().c_str());
            else ImGui::TextDisabled("Nothing selected in %s", edited.front()->Title().c_str());
        }
        else if (hasSelection) {
            ImGui::TextDisabled("%u selected in %d of %d edited lanes", selected, lanesWithSelection, (int)edited.size());
        }
        else {
            ImGui::TextDisabled("Nothing selected in the %d edited lanes", (int)edited.size());
        }

        if (ImGui::MenuItem(TR(CUT), keys->GetBindingString("cut"), false, hasSelection)) cutSelection();
        if (ImGui::MenuItem(TR(COPY), keys->GetBindingString("copy"), false, hasSelection)) copySelection();
        if (ImGui::MenuItem(TR(PASTE), keys->GetBindingString("paste"), false, !CopiedSelection.empty())) pasteSelection();
        if (ImGui::MenuItem(TR(DELETE), keys->GetBindingString("remove_action"), false, hasSelection)) removeAction();
        ImGui::Separator();

        // These work on one script, the current lane.
        auto script = ActiveFunscript();
        if (ImGui::MenuItem(TR(INVERT), keys->GetBindingString("invert_actions"), false, script->HasSelection())) invertSelection();
        if (ImGui::MenuItem(TR(EQUALIZE), keys->GetBindingString("equalize_actions"), false, script->SelectionSize() >= 3)) equalizeSelection();
        // Unlike the rest, isolate works on the point nearest the playhead
        // rather than on the selection, so the label has to say so.
        if (ImGui::MenuItem("Isolate point at playhead", keys->GetBindingString("isolate_action"))) isolateAction();
        // Simplify needs its epsilon slider, so this opens it on the selection
        // rather than simplifying by a guessed amount.
        if (ImGui::MenuItem("Simplify...", nullptr, false, script->SelectionSize() > 4)) {
            specialFunctions->SetFunction(SpecialFunctionType::RamerDouglasPeucker);
            OpenFunscripterState::State(stateHandle).showSpecialFunctions = true;
        }
        ImGui::Separator();

        // Enabled from three points, which is what they need to tell a top from
        // a bottom. Enabled for any selection, they sat there looking usable
        // with one or two points selected and did nothing when chosen.
        const bool enoughForStroke = script->SelectionSize() >= 3;
        if (ImGui::MenuItem(TR(TOP_POINTS_ONLY), keys->GetBindingString("select_top_points"), false, enoughForStroke)) selectTopPoints();
        if (ImGui::MenuItem(TR(MID_POINTS_ONLY), keys->GetBindingString("select_middle_points"), false, enoughForStroke)) selectMiddlePoints();
        if (ImGui::MenuItem(TR(BOTTOM_POINTS_ONLY), keys->GetBindingString("select_bottom_points"), false, enoughForStroke)) selectBottomPoints();
        ImGui::Separator();
    };
    registerBindings();

    scriptTimeline.Init();

    scripting = std::make_unique<ScriptingMode>();
    scripting->Init();

    EV::Queue().appendListener(FunscriptActionsChangedEvent::EventType,
        FunscriptActionsChangedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::FunscriptChanged)));
    EV::Queue().appendListener(SDL_DROPFILE,
        OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::DragNDrop)));
    EV::Queue().appendListener(SDL_CONTROLLERAXISMOTION,
        OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ControllerAxisPlaybackSpeed)));
    EV::Queue().appendListener(VideoLoadedEvent::EventType,
        VideoLoadedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::VideoLoaded)));
    EV::Queue().appendListener(DurationChangeEvent::EventType,
        DurationChangeEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::VideoDuration)));
    EV::Queue().appendListener(PlayPauseChangeEvent::EventType,
        PlayPauseChangeEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::PlayPauseChange)));
    EV::Queue().appendListener(FunscriptActionShouldMoveEvent::EventType,
        FunscriptActionShouldMoveEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineActionMoved)));
    EV::Queue().appendListener(FunscriptActionClickedEvent::EventType,
        FunscriptActionClickedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineActionClicked)));
    EV::Queue().appendListener(FunscriptActionShouldCreateEvent::EventType,
        FunscriptActionShouldCreateEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineActionCreated)));
    EV::Queue().appendListener(ShouldSetTimeEvent::EventType,
        ShouldSetTimeEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineDoubleClick)));
    EV::Queue().appendListener(FunscriptShouldSelectTimeEvent::EventType,
        FunscriptShouldSelectTimeEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineSelectTime)));
    EV::Queue().appendListener(ShouldChangeActiveScriptEvent::EventType,
        ShouldChangeActiveScriptEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ScriptTimelineActiveScriptChanged)));
    EV::Queue().appendListener(ExportClipForChapter::EventType,
        ExportClipForChapter::HandleEvent(EVENT_SYSTEM_BIND(this, &OpenFunscripter::ExportClip)));

    specialFunctions = std::make_unique<SpecialFunctionsWindow>();
    controllerInput = std::make_unique<ControllerInput>();
    controllerInput->Init();
    simulator.Init();

    FunscriptHeatmap::Init();
    extensions = std::make_unique<OFS_LuaExtensions>();
    extensions->Init();
    metadataEditor = std::make_unique<OFS_FunscriptMetadataEditor>();

    webApi = std::make_unique<OFS_WebsocketApi>();
    webApi->Init();

    chapterMgr = std::make_unique<OFS_ChapterManager>();
    scriptCheck = std::make_unique<OFS_ScriptCheck>();
    deviceLink = std::make_unique<OFS_DeviceLink>();
#ifdef WIN32
    OFS_DownloadFfmpeg::FfmpegMissing = !Util::FileExists(Util::FfmpegPath().u8string());
#endif

    closeProject(true);
    if (argc > 1) {
        const char* path = argv[1];
        openFile(path);
    }
    else if (!ofsState.recentFiles.empty()) {
        auto& project = ofsState.recentFiles.back().projectPath;
        if (!project.empty()) {
            openFile(project);
        }
    }

    // Load potentially missing glyphs of recent files
    for (auto& recentFile : ofsState.recentFiles) {
        OFS_DynFontAtlas::AddText(recentFile.name.c_str());
    }

    // A scripted run draws the same frames whether or not anyone can see them,
    // and a window appearing takes the mouse and the keyboard from whatever
    // the user is doing. Several runs at once did it several times over, so a
    // test run stays off the screen entirely unless OFS_UITEST_SHOW is set.
    if (!OFS_UiDriver::Enabled() || OFS_UiDriver::ShowsWindow()) {
        SDL_ShowWindow(window);
    }
    return true;
}

// The height of the toolbar's dock node as one row: a control and a little
// air above and below it. Together with the 2 pixel splitter ImGui leaves
// between docked nodes, it is the room the toolbar took when it was a bar fixed
// to the window, so everything under it is where it always was.
static constexpr float DockSplitterSize = 2.f;
static float toolbarRowHeight() noexcept
{
    return std::ceil(ImGui::GetFrameHeight() + 8.f) - DockSplitterSize;
}

// The padding that centres one row of controls in that height.
static float toolbarVerticalPadding() noexcept
{
    return std::floor((toolbarRowHeight() - ImGui::GetFrameHeight()) * 0.5f);
}

void OpenFunscripter::setupDefaultLayout(bool force) noexcept
{
    MainDockspaceID = ImGui::GetID("MainAppDockspace");
    OFS_DownloadFfmpeg::ModalId = ImGui::GetID(OFS_DownloadFfmpeg::WindowId);

    auto imgui_ini = ImGui::GetIO().IniFilename;
    bool imgui_ini_found = Util::FileExists(imgui_ini);
    if (force || !imgui_ini_found) {
        if (!imgui_ini_found) {
            LOG_INFO("imgui.ini was not found...");
            LOG_INFO("Setting default layout.");
        }

        ImGui::ClearIniSettings();

        ImGui::DockBuilderRemoveNode(MainDockspaceID); // Clear out existing layout
        ImGui::DockBuilderAddNode(MainDockspaceID, ImGuiDockNodeFlags_DockSpace); // Add empty node
        // The toolbar takes one row across the top. Measured on a node grown
        // by its height, so everything under it is laid out at the same
        // proportions as before it was a window. It keeps its height when the
        // app is resized, since the node beside it holds the central one.
        const float toolbarHeight = toolbarRowHeight();
        const float rootHeight = DefaultHeight + toolbarHeight + DockSplitterSize;
        ImGui::DockBuilderSetNodeSize(MainDockspaceID, ImVec2(DefaultWidth, rootHeight));
        ImGuiID dock_rest_id;
        auto dock_toolbar_id = ImGui::DockBuilderSplitNode(MainDockspaceID, ImGuiDir_Up,
            toolbarHeight / rootHeight, NULL, &dock_rest_id);

        ImGuiID dock_player_center_id;
        ImGuiID opposite_node_id;
        auto dock_time_bottom_id = ImGui::DockBuilderSplitNode(dock_rest_id, ImGuiDir_Down, 0.1f, NULL, &dock_player_center_id);
        auto dock_positions_id = ImGui::DockBuilderSplitNode(dock_player_center_id, ImGuiDir_Down, 0.15f, NULL, &dock_player_center_id);
        // The tracks beside the lanes they list.
        auto dock_tracks_id = ImGui::DockBuilderSplitNode(dock_positions_id, ImGuiDir_Left, 0.14f, NULL, &dock_positions_id);
        auto dock_mode_right_id = ImGui::DockBuilderSplitNode(dock_player_center_id, ImGuiDir_Right, 0.15f, NULL, &dock_player_center_id);
        // The right column, bottom up. Mode is left the most room because it
        // carries each mode's and grid's settings, which the tempo grid's run
        // to several rows; the simulator gets enough for its mode bar and the
        // settings under it; the undo history, empty to begin with, least.
        // Seen in scripted screenshots of a fresh profile, where Mode cut the
        // tempo settings off after one row and the history sat mostly empty.
        auto dock_simulator_right_id = ImGui::DockBuilderSplitNode(dock_mode_right_id, ImGuiDir_Down, 0.24f, NULL, &dock_mode_right_id);
        auto dock_action_right_id = ImGui::DockBuilderSplitNode(dock_mode_right_id, ImGuiDir_Down, 0.30f, NULL, &dock_mode_right_id);
        auto dock_stats_right_id = ImGui::DockBuilderSplitNode(dock_mode_right_id, ImGuiDir_Down, 0.32f, NULL, &dock_mode_right_id);
        auto dock_undo_right_id = ImGui::DockBuilderSplitNode(dock_mode_right_id, ImGuiDir_Down, 0.35f, NULL, &dock_mode_right_id);


        ImGui::DockBuilderGetNode(dock_player_center_id)->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::DockBuilderGetNode(dock_positions_id)->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::DockBuilderGetNode(dock_time_bottom_id)->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::DockBuilderGetNode(dock_toolbar_id)->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
        ImGui::DockBuilderDockWindow(ToolbarWindowId, dock_toolbar_id);

        ImGui::DockBuilderDockWindow(OFS_VideoplayerWindow::WindowId, dock_player_center_id);
        ImGui::DockBuilderDockWindow(OFS_VideoplayerControls::TimeId, dock_time_bottom_id);
        ImGui::DockBuilderDockWindow(ScriptTimeline::WindowId, dock_positions_id);
        ImGui::DockBuilderDockWindow(TracksWindowId, dock_tracks_id);
        ImGui::DockBuilderDockWindow(ScriptingMode::WindowId, dock_mode_right_id);
        ImGui::DockBuilderDockWindow(ScriptSimulator::WindowId, dock_simulator_right_id);
        // A tab beside statistics rather than a split of its own. Opened into
        // its own split it took its height out of the undo history and the
        // statistics, leaving one row of history and cutting the statistics
        // off, and still had too little room for its own keypad.
        (void)dock_action_right_id;
        ImGui::DockBuilderDockWindow(ActionEditorWindowId, dock_stats_right_id);
        ImGui::DockBuilderDockWindow(StatisticsWindowId, dock_stats_right_id);
        ImGui::DockBuilderDockWindow(UndoSystem::WindowId, dock_undo_right_id);
        simulator.CenterSimulator();
        simulator.CenterWhenVideoShows = true;
        ImGui::DockBuilderFinish(MainDockspaceID);
    }
}

void OpenFunscripter::registerBindings()
{
    keys->RegisterGroup("Actions", Tr::ACTIONS_BINDING_GROUP);
    // Adding a point at a position is on the keypad and on the number row, for
    // keyboards without a keypad. A default is only added to a profile that
    // does not already use that key for something.
    {
        // DELETE ACTION
        keys->RegisterAction(
            { "remove_action",
                [this]() { removeAction(); } },
            Tr::ACTION_REMOVE_ACTION, "Actions",
            { { ImGuiMod_None, ImGuiKey_Delete },
                { ImGuiMod_None, ImGuiKey_GamepadFaceRight } });
        // ADD ACTIONS
        keys->RegisterAction(
            { "action_0",
                [this]() { addEditAction(0); } },
            Tr::ACTION_ACTION_0, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad0 },
                { ImGuiMod_None, ImGuiKey_0 },
            });
        keys->RegisterAction(
            { "action_10",
                [this]() { addEditAction(10); } },
            Tr::ACTION_ACTION_10, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad1 },
                { ImGuiMod_None, ImGuiKey_1 },
            });
        keys->RegisterAction(
            { "action_20",
                [this]() { addEditAction(20); } },
            Tr::ACTION_ACTION_20, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad2 },
                { ImGuiMod_None, ImGuiKey_2 },
            });
        keys->RegisterAction(
            { "action_30",
                [this]() { addEditAction(30); } },
            Tr::ACTION_ACTION_30, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad3 },
                { ImGuiMod_None, ImGuiKey_3 },
            });
        keys->RegisterAction(
            { "action_40",
                [this]() { addEditAction(40); } },
            Tr::ACTION_ACTION_40, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad4 },
                { ImGuiMod_None, ImGuiKey_4 },
            });
        keys->RegisterAction(
            { "action_50",
                [this]() { addEditAction(50); } },
            Tr::ACTION_ACTION_50, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad5 },
                { ImGuiMod_None, ImGuiKey_5 },
            });
        keys->RegisterAction(
            { "action_60",
                [this]() { addEditAction(60); } },
            Tr::ACTION_ACTION_60, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad6 },
                { ImGuiMod_None, ImGuiKey_6 },
            });
        keys->RegisterAction(
            { "action_70",
                [this]() { addEditAction(70); } },
            Tr::ACTION_ACTION_70, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad7 },
                { ImGuiMod_None, ImGuiKey_7 },
            });
        keys->RegisterAction(
            { "action_80",
                [this]() { addEditAction(80); } },
            Tr::ACTION_ACTION_80, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad8 },
                { ImGuiMod_None, ImGuiKey_8 },
            });
        keys->RegisterAction(
            { "action_90",
                [this]() { addEditAction(90); } },
            Tr::ACTION_ACTION_90, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_Keypad9 },
                { ImGuiMod_None, ImGuiKey_9 },
            });
        keys->RegisterAction(
            { "action_100",
                [this]() { addEditAction(100); } },
            Tr::ACTION_ACTION_100, "Actions",
            {
                { ImGuiMod_None, ImGuiKey_KeypadDivide },
            });
        // Halfway between those, on the same keys with Ctrl: Ctrl+0 adds a
        // point at 5 through Ctrl+9 at 95. Not localized: English only for now.
        for (int32_t digit = 0; digit <= 9; digit += 1) {
            const int32_t pos = (digit * 10) + 5;
            const std::string id = "action_" + std::to_string(pos);
            keys->RegisterAction(
                { id.c_str(),
                    [this, pos]() { addEditAction(pos); } },
                "Action at " + std::to_string(pos), "Actions",
                {
                    { ImGuiMod_Ctrl, (ImGuiKey)(ImGuiKey_Keypad0 + digit) },
                    { ImGuiMod_Ctrl, (ImGuiKey)(ImGuiKey_0 + digit) },
                });
        }
    }

    keys->RegisterGroup("Core", Tr::CORE_BINDING_GROUP);
    {
        // SAVE
        keys->RegisterAction(
            { "save_project",
                [this]() { saveProject(); } },
            Tr::ACTION_SAVE_PROJECT, "Core",
            {
                { ImGuiMod_Ctrl, ImGuiKey_S },
            });

        keys->RegisterAction(
            { "quick_export",
                [this]() { quickExport(); } },
            Tr::ACTION_QUICK_EXPORT, "Core",
            {
                { ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiKey_S },
            });

        keys->RegisterAction(
            { "quick_export_2_0",
                [this]() { quickExport2(); } },
            Tr::ACTION_QUICK_EXPORT_2_0, "Core",
            {
                { ImGuiMod_Alt, ImGuiKey_S },
            });

        keys->RegisterAction(
            { "sync_timestamps",
                [this]() { player->SyncWithPlayerTime(); } },
            Tr::ACTION_SYNC_TIME_WITH_PLAYER, "Core",
            {
                { ImGuiMod_None, ImGuiKey_S },
            });

        keys->RegisterAction(
            { "cycle_loaded_forward_scripts",
                [this]() { 
                    auto activeIdx = LoadedProject->ActiveIdx();
                    do {
                        activeIdx++;
                        activeIdx %= LoadedFunscripts().size();
                    } while (!LoadedFunscripts()[activeIdx]->Enabled);
                    UpdateNewActiveScript(activeIdx); } },
            Tr::ACTION_CYCLE_FORWARD_LOADED_SCRIPTS, "Core",
            {
                { ImGuiMod_None, ImGuiKey_PageDown },
            });

        keys->RegisterAction(
            { "cycle_loaded_backward_scripts",
                [this]() {
                    auto activeIdx = LoadedProject->ActiveIdx();
                    do {
                        activeIdx--;
                        activeIdx %= LoadedFunscripts().size();
                    } while (!LoadedFunscripts()[activeIdx]->Enabled);
                    UpdateNewActiveScript(activeIdx);
                } },
            Tr::ACTION_CYCLE_BACKWARD_LOADED_SCRIPTS, "Core",
            {
                { ImGuiMod_None, ImGuiKey_PageUp },
            });

        keys->RegisterAction(
            { "reload_translation_csv",
                [this]() {
                    const auto& prefState = PreferenceState::State(preferences->StateHandle());
                    if (!prefState.languageCsv.empty()) {
                        if (OFS_Translator::ptr->LoadTranslation(prefState.languageCsv.c_str())) {
                            OFS_DynFontAtlas::AddTranslationText();
                        }
                    }
                } },
            Tr::ACTION_RELOAD_TRANSLATION, "Core");
    }

    keys->RegisterGroup("Navigation", Tr::NAVIGATION_BINDING_GROUP);
    {
        // JUMP BETWEEN ACTIONS
        keys->RegisterAction(
            { "prev_action",
                [this]() {
                    auto action = ActiveFunscript()->GetPreviousActionBehind(player->CurrentTime() - 0.001f);
                    if (action != nullptr) player->SetPositionExact(action->atS);
                },
                false },
            Tr::ACTION_PREVIOUS_ACTION, "Navigation",
            { { ImGuiKey_None, ImGuiKey_DownArrow, true },
                { ImGuiKey_None, ImGuiKey_GamepadDpadDown, true } });

        keys->RegisterAction(
            { "next_action",
                [this]() {
                    auto action = ActiveFunscript()->GetNextActionAhead(player->CurrentTime() + 0.001f);
                    if (action != nullptr) player->SetPositionExact(action->atS);
                },
                false },
            Tr::ACTION_NEXT_ACTION, "Navigation",
            { { ImGuiKey_None, ImGuiKey_UpArrow, true },
                { ImGuiKey_None, ImGuiKey_GamepadDpadUp, true } });

        keys->RegisterAction(
            { "prev_action_multi",
                [this]() {
                    bool foundAction = false;
                    float closestTime = std::numeric_limits<float>::max();
                    float currentTime = player->CurrentTime();

                    for (int i = 0; i < LoadedFunscripts().size(); i++) {
                        auto& script = LoadedFunscripts()[i];
                        auto action = script->GetPreviousActionBehind(currentTime - 0.001f);
                        if (action != nullptr) {
                            if (std::abs(currentTime - action->atS) < std::abs(currentTime - closestTime)) {
                                foundAction = true;
                                closestTime = action->atS;
                            }
                        }
                    }
                    if (foundAction) {
                        player->SetPositionExact(closestTime);
                    }
                },
                false },
            Tr::ACTION_PREVIOUS_ACTION_MULTI, "Navigation",
            {
                { ImGuiMod_Ctrl, ImGuiKey_DownArrow, true },
            });

        keys->RegisterAction(
            { "next_action_multi",
                [this]() {
                    bool foundAction = false;
                    float closestTime = std::numeric_limits<float>::max();
                    float currentTime = player->CurrentTime();
                    for (int i = 0; i < LoadedFunscripts().size(); i++) {
                        auto& script = LoadedFunscripts()[i];
                        auto action = script->GetNextActionAhead(currentTime + 0.001f);
                        if (action != nullptr) {
                            if (std::abs(currentTime - action->atS) < std::abs(currentTime - closestTime)) {
                                foundAction = true;
                                closestTime = action->atS;
                            }
                        }
                    }
                    if (foundAction) {
                        player->SetPositionExact(closestTime);
                    }
                },
                false },
            Tr::ACTION_NEXT_ACTION_MULTI, "Navigation",
            {
                { ImGuiMod_Ctrl, ImGuiKey_UpArrow, true },
            });

        // FRAME CONTROL
        keys->RegisterAction(
            { "prev_frame",
                [this]() {
                    if (player->IsPaused()) {
                        scripting->PreviousFrame();
                    }
                },
                false },
            Tr::ACTION_PREV_FRAME, "Navigation",
            {
                { ImGuiMod_None, ImGuiKey_LeftArrow, true },
                { ImGuiMod_None, ImGuiKey_GamepadDpadLeft, true },
            });

        keys->RegisterAction(
            { "next_frame",
                [this]() {
                    if (player->IsPaused()) {
                        scripting->NextFrame();
                    }
                },
                false },
            Tr::ACTION_NEXT_FRAME, "Navigation",
            {
                { ImGuiMod_None, ImGuiKey_RightArrow, true },
                { ImGuiMod_None, ImGuiKey_GamepadDpadRight, true },
            });

        keys->RegisterAction(
            { "fast_step",
                [this]() {
                    const auto& prefState = PreferenceState::State(preferences->StateHandle());
                    player->SeekFrames(prefState.fastStepAmount);
                },
                false },
            Tr::ACTION_FAST_STEP, "Navigation",
            { { ImGuiMod_Ctrl, ImGuiKey_RightArrow, true } });

        keys->RegisterAction(
            { "fast_backstep",
                [this]() {
                    const auto& prefState = PreferenceState::State(preferences->StateHandle());
                    player->SeekFrames(-prefState.fastStepAmount);
                },
                false },
            Tr::ACTION_FAST_BACKSTEP, "Navigation",
            { { ImGuiMod_Ctrl, ImGuiKey_LeftArrow, true } });
    }

    keys->RegisterGroup("Utility", Tr::UTILITY_BINDING_GROUP);
    {
        // UNDO / REDO
        keys->RegisterAction(
            { "undo",
                [this]() {
                    Undo();
                },
                false },
            Tr::ACTION_UNDO, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_Z, true } });

        keys->RegisterAction(
            { "redo",
                [this]() {
                    Redo();
                },
                false },
            Tr::ACTION_REDO, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_Y, true } });

        // COPY / PASTE
        keys->RegisterAction(
            { "copy",
                [this]() {
                    copySelection();
                },
                false },
            Tr::ACTION_COPY, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_C } });

        keys->RegisterAction(
            { "paste",
                [this]() {
                    pasteSelection();
                },
                false },
            Tr::ACTION_PASTE, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_V } });

        keys->RegisterAction(
            { "paste_exact",
                [this]() {
                    pasteSelectionExact();
                },
                false },
            Tr::ACTION_PASTE_EXACT, "Utility",
            { { ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiKey_V } });

        keys->RegisterAction(
            { "cut",
                [this]() {
                    cutSelection();
                },
                false },
            Tr::ACTION_CUT, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_X } });

        keys->RegisterAction(
            { "select_all",
                [this]() {
                    selectAll();
                },
                false },
            Tr::ACTION_SELECT_ALL, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_A } });

        keys->RegisterAction(
            { "deselect_all",
                [this]() {
                    clearSelection();
                },
                false },
            Tr::ACTION_DESELECT_ALL, "Utility",
            { { ImGuiMod_Ctrl, ImGuiKey_D } });

        keys->RegisterAction(
            { "select_all_left",
                [this]() {
                    selectTime(0, player->CurrentTime());
                },
                false },
            Tr::ACTION_SELECT_ALL_LEFT, "Utility",
            { { ImGuiMod_Ctrl | ImGuiMod_Alt, ImGuiKey_LeftArrow } });

        keys->RegisterAction(
            { "select_all_right",
                [this]() {
                    selectTime(player->CurrentTime(), player->Duration());
                },
                false },
            Tr::ACTION_SELECT_ALL_RIGHT, "Utility",
            { { ImGuiMod_Ctrl | ImGuiMod_Alt, ImGuiKey_RightArrow } });

        keys->RegisterAction(
            { "select_top_points",
                [this]() {
                    selectTopPoints();
                },
                false },
            Tr::ACTION_SELECT_TOP, "Utility");

        keys->RegisterAction(
            { "select_middle_points",
                [this]() {
                    selectMiddlePoints();
                },
                false },
            Tr::ACTION_SELECT_MID, "Utility");

        keys->RegisterAction(
            { "select_bottom_points",
                [this]() {
                    selectBottomPoints();
                },
                false },
            Tr::ACTION_SELECT_BOTTOM, "Utility");

        // SCREENSHOT VIDEO
        keys->RegisterAction(
            { "save_frame_as_image",
                [this]() {
                    auto screenshotDir = Util::Prefpath("screenshot");
                    player->SaveFrameToImage(screenshotDir);
                },
                false },
            Tr::ACTION_SAVE_FRAME, "Utility",
            { { ImGuiMod_None, ImGuiKey_F2 } });

        // CHANGE SUBTITLES
        keys->RegisterAction(
            { "cycle_subtitles",
                [this]() {
                    player->CycleSubtitles();
                },
                false },
            Tr::ACTION_CYCLE_SUBTITLES, "Utility",
            { { ImGuiMod_None, ImGuiKey_J } });

        // FULLSCREEN
        keys->RegisterAction(
            { "fullscreen_toggle",
                [this]() {
                    Status ^= OFS_Status::OFS_Fullscreen;
                    SetFullscreen(Status & OFS_Status::OFS_Fullscreen);
                },
                false },
            Tr::ACTION_TOGGLE_FULLSCREEN, "Utility",
            { { ImGuiMod_None, ImGuiKey_F10 } });

        // SIMULATOR MODE
        // Unbound by default; every obvious key is already taken by something
        // used far more often.
        keys->RegisterAction(
            { "cycle_simulator_mode",
                [this]() { simulator.CycleMode(); },
                false },
            "Switch the simulator between 2D and 3D", "Utility");

        // The 3D model's easter egg, also on a checkbox in its settings.
        keys->RegisterAction(
            { "toggle_simulator_finish",
                [this]() {
                    simulator.FinishEasterEgg = !simulator.FinishEasterEgg;
                    if (!simulator.FinishEasterEgg) {
                        simulator.FinishDrops.clear();
                        simulator.FinishStimulation = 0.f;
                    }
                },
                false },
            "Toggle the 3D simulator's easter egg", "Utility");

        // Finishes at once, turning the easter egg on if it is off, for
        // trying it out without stroking up to it.
        keys->RegisterAction(
            { "simulator_finish_now",
                [this]() {
                    simulator.FinishEasterEgg = true;
                    simulator.FinishRequested = true;
                },
                false },
            "Make the 3D simulator's easter egg finish now", "Utility");

        keys->RegisterAction(
            { "toggle_simulator_cutaway",
                [this]() {
                    auto& simState = SimulatorState::State(simulator.StateHandle());
                    simState.CutawayCase = !simState.CutawayCase;
                },
                false },
            "Toggle the 3D simulator's cutaway", "Utility");

        keys->RegisterAction(
            { "toggle_simulator_rod",
                [this]() {
                    auto& simState = SimulatorState::State(simulator.StateHandle());
                    simState.ShowRod = !simState.ShowRod;
                },
                false },
            "Show or hide the 3D simulator's rod", "Utility");

        keys->RegisterAction(
            { "cycle_simulator_opacity",
                [this]() {
                    auto& simState = SimulatorState::State(simulator.StateHandle());
                    simState.GlobalOpacity = simState.GlobalOpacity < 0.6f ? 0.75f
                        : (simState.GlobalOpacity < 0.9f ? 1.f : 0.5f);
                },
                false },
            "Cycle the simulator's opacity: half, three quarters, full", "Utility");
    }

    // MOVE LEFT/RIGHT
    // In every edited lane: the selection where there is one, otherwise the
    // point nearest the playhead.
    auto move_actions_horizontal = [](bool forward) {
        auto app = OpenFunscripter::ptr;
        if (app->targetsHaveSelection()) {
            app->moveSelectionTime(forward);
        }
        else {
            app->moveClosestTime(forward);
        }
    };
    // The same, with the video following the leading lane's points.
    auto move_actions_horizontal_with_video = [](bool forward) {
        auto app = OpenFunscripter::ptr;
        if (auto lead = app->selectionLead()) {
            app->moveSelectionTime(forward);
            auto closest = lead->GetClosestActionSelection(app->player->CurrentTime());
            if (closest != nullptr) {
                app->player->SetPositionExact(closest->atS);
            }
            else if (lead->HasSelection()) {
                app->player->SetPositionExact(lead->Selection().front().atS);
            }
        }
        else if (auto movedTo = app->moveClosestTime(forward)) {
            app->player->SetPositionExact(*movedTo);
        }
    };

    keys->RegisterGroup("Moving", Tr::MOVING_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "move_actions_up_ten",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(10);
                    }
                    else {
                        moveClosestPosition(10);
                    }
                },
                false },
            Tr::ACTION_MOVE_UP_10, "Moving");

        keys->RegisterAction(
            { "move_actions_down_ten",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(-10);
                    }
                    else {
                        moveClosestPosition(-10);
                    }
                },
                false },
            Tr::ACTION_MOVE_DOWN_10, "Moving");

        keys->RegisterAction(
            { "move_actions_up_five",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(5);
                    }
                    else {
                        moveClosestPosition(5);
                    }
                },
                false },
            Tr::ACTION_MOVE_UP_5, "Moving");

        keys->RegisterAction(
            { "move_actions_down_five",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(-5);
                    }
                    else {
                        moveClosestPosition(-5);
                    }
                },
                false },
            Tr::ACTION_MOVE_DOWN_5, "Moving");

        keys->RegisterAction(
            { "move_actions_left_snapped",
                [move_actions_horizontal_with_video]() {
                    move_actions_horizontal_with_video(false);
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_LEFT_SNAP, "Moving",
            { { ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiKey_LeftArrow, true } });

        keys->RegisterAction(
            { "move_actions_right_snapped",
                [move_actions_horizontal_with_video]() {
                    move_actions_horizontal_with_video(true);
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_RIGHT_SNAP, "Moving",
            { { ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiKey_RightArrow, true } });

        keys->RegisterAction(
            { "move_actions_left",
                [move_actions_horizontal]() {
                    move_actions_horizontal(false);
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_LEFT, "Moving",
            { { ImGuiMod_Shift, ImGuiKey_LeftArrow, true } });

        keys->RegisterAction(
            { "move_actions_right",
                [move_actions_horizontal]() {
                    move_actions_horizontal(true);
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_RIGHT, "Moving",
            { { ImGuiMod_Shift, ImGuiKey_RightArrow, true } });

        // MOVE SELECTION UP/DOWN
        keys->RegisterAction(
            { "move_actions_up",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(1);
                    }
                    else {
                        moveClosestPosition(1);
                    }
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_UP, "Moving",
            { { ImGuiMod_Shift, ImGuiKey_UpArrow, true } });

        keys->RegisterAction(
            { "move_actions_down",
                [this]() {
                    if (targetsHaveSelection()) {
                        moveSelectionPosition(-1);
                    }
                    else {
                        moveClosestPosition(-1);
                    }
                },
                false },
            Tr::ACTION_MOVE_ACTIONS_DOWN, "Moving",
            { { ImGuiMod_Shift, ImGuiKey_DownArrow, true } });

        keys->RegisterAction(
            { "move_action_to_current_pos",
                [this]() {
                    moveClosestToPlayhead();
                },
                false },
            Tr::ACTION_MOVE_TO_CURRENT_POSITION, "Moving",
            { { ImGuiMod_None, ImGuiKey_End } });
    }
    // FUNCTIONS
    keys->RegisterGroup("Special", Tr::SPECIAL_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "equalize_actions",
                [this]() {
                    equalizeSelection();
                },
                false },
            Tr::ACTION_EQUALIZE_ACTIONS, "Special",
            { { ImGuiMod_None, ImGuiKey_E } });

        keys->RegisterAction(
            { "invert_actions",
                [this]() {
                    invertSelection();
                },
                false },
            Tr::ACTION_INVERT_ACTIONS, "Special",
            { { ImGuiMod_None, ImGuiKey_I } });

        keys->RegisterAction(
            { "isolate_action",
                [this]() {
                    isolateAction();
                },
                false },
            Tr::ACTION_ISOLATE_ACTION, "Special",
            { { ImGuiMod_None, ImGuiKey_R } });

        keys->RegisterAction(
            { "repeat_stroke",
                [this]() {
                    repeatLastStroke();
                },
                false },
            Tr::ACTION_REPEAT_STROKE, "Special",
            { { ImGuiMod_None, ImGuiKey_Home } });
    }

    // VIDEO CONTROL
    keys->RegisterGroup("Videoplayer", Tr::VIDEOPLAYER_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "toggle_play",
                [this]() { player->TogglePlay(); },
                false },
            Tr::ACTION_TOGGLE_PLAY, "Videoplayer",
            { { ImGuiKey_None, ImGuiKey_Space },
                { ImGuiKey_None, ImGuiKey_GamepadStart } });

        // PLAYBACK SPEED
        keys->RegisterAction(
            { "decrement_speed",
                [this]() { player->AddSpeed(-0.10); },
                false },
            Tr::ACTION_REDUCE_PLAYBACK_SPEED, "Videoplayer",
            {
                { ImGuiKey_None, ImGuiKey_KeypadSubtract },
            });

        keys->RegisterAction(
            { "increment_speed",
                [this]() { player->AddSpeed(0.10); },
                false },
            Tr::ACTION_INCREASE_PLAYBACK_SPEED, "Videoplayer",
            {
                { ImGuiKey_None, ImGuiKey_KeypadAdd },
            });

        keys->RegisterAction(
            { "goto_start",
                [this]() { player->SetPositionPercent(0.f, false); },
                false },
            Tr::ACTION_GO_TO_START, "Videoplayer");

        keys->RegisterAction(
            { "goto_end",
                [this]() { player->SetPositionPercent(1.f, false); },
                false },
            Tr::ACTION_GO_TO_END, "Videoplayer");
    }

    keys->RegisterGroup("Extensions", Tr::EXTENSIONS_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "reload_enabled_extensions",
                [this]() { extensions->ReloadEnabledExtensions(); },
                false },
            Tr::ACTION_RELOAD_ENABLED_EXTENSIONS, "Extensions");
    }

    keys->RegisterGroup("Controller", Tr::CONTROLLER_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "toggle_controller_navmode",
                [this]() {
                    auto& io = ImGui::GetIO();
                    io.ConfigFlags ^= ImGuiConfigFlags_NavEnableGamepad;
                },
                false },
            Tr::ACTION_TOGGLE_CONTROLLER_NAV, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadL3 } });

        keys->RegisterAction(
            { "seek_forward_second",
                [this]() {
                    player->SeekRelative(1);
                },
                false },
            Tr::ACTION_SEEK_FORWARD_1, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadR1 } });

        keys->RegisterAction(
            { "seek_backward_second",
                [this]() {
                    player->SeekRelative(-1);
                },
                false },
            Tr::ACTION_SEEK_BACKWARD_1, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadL1 } });

        keys->RegisterAction(
            { "add_action_controller",
                [this]() {
                    addEditAction(100);
                },
                false },
            Tr::ACTION_ADD_ACTION_CONTROLLER, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadFaceDown } });

        keys->RegisterAction(
            { "toggle_recording_mode",
                [this]() {
                    static ScriptingModeEnum prevMode = ScriptingModeEnum::RECORDING;
                    if (scripting->ActiveMode() != ScriptingModeEnum::RECORDING) {
                        prevMode = scripting->ActiveMode();
                        scripting->SetMode(ScriptingModeEnum::RECORDING);
                        ScriptingModeBase* mode = scripting->Mode().get();
                        static_cast<RecordingMode*>(mode)->setRecordingMode(RecordingMode::RecordingType::Controller);
                    }
                    else {
                        scripting->SetMode(prevMode);
                    }
                },
                false },
            Tr::ACTION_TOGGLE_RECORDING_MODE, "Controller");

        keys->RegisterAction(
            { "set_selection_controller",
                [this]() {
                    if (scriptTimeline.selectionStart() < 0) {
                        scriptTimeline.setStartSelection(player->CurrentTime());
                    }
                    else {
                        auto tmp = player->CurrentTime();
                        auto [min, max] = std::minmax<float>(scriptTimeline.selectionStart(), tmp);
                        selectTime(min, max);
                        scriptTimeline.setStartSelection(-1);
                    }
                },
                false },
            Tr::ACTION_CONTROLLER_SELECT, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadR3 } });

        keys->RegisterAction(
            { "set_current_playbackspeed_controller",
                [this]() {
                    Status |= OFS_Status::OFS_GamepadSetPlaybackSpeed;
                },
                false },
            Tr::ACTION_SET_PLAYBACK_SPEED, "Controller",
            { { ImGuiMod_None, ImGuiKey_GamepadFaceLeft } });
    }

    keys->RegisterGroup("Chapters", Tr::CHAPTER_BINDING_GROUP);
    {
        keys->RegisterAction(
            { "create_chapter",
                [this]() {
                    auto& chapterState = chapterMgr->State();
                    if (auto chapter = chapterState.AddChapter(player->CurrentTime(), player->Duration())) {
                        EV::Enqueue<ChapterStateChanged>();
                    }
                },
                false },
            Tr::ACTION_CREATE_CHAPTER, "Chapters",
            {});

        keys->RegisterAction(
            { "create_bookmark",
                [this]() {
                    auto& chapterState = chapterMgr->State();
                    if (auto bookmark = chapterState.AddBookmark(player->CurrentTime())) {
                        EV::Enqueue<ChapterStateChanged>();
                    }
                },
                false },
            Tr::ACTION_CREATE_BOOKMARK, "Chapters",
            {});
    }

    // Group where all dynamic actions are placed.
    // Lua functions for example.
    keys->RegisterGroup("Dynamic", Tr::DYNAMIC_BINDING_GROUP);
}


void OpenFunscripter::newFrame() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.1f, 0.1f, 0.1f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    if (OFS_DynFontAtlas::NeedsRebuild()) {
        const auto& prefState = PreferenceState::State(preferences->StateHandle());
        OFS_DynFontAtlas::RebuildFont(prefState.defaultFontSize);
    }

    // Created on the first frame rather than in Init, since Run draws one
    // frame before the loop and the driver needs a finished app to act on.
    static bool uiDriverChecked = false;
    if (!uiDriverChecked) {
        uiDriverChecked = true;
        uiDriver = OFS_UiDriver::FromEnvironment();
    }
    // After the backend has queued this frame's real input, so scripted input
    // is the last word.
    if (uiDriver) uiDriver->BeforeNewFrame();

    ImGui::NewFrame();
}

void OpenFunscripter::render() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    ImGui::Render();

    OFS_ImGui::CurrentlyRenderedViewport = ImGui::GetMainViewport();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    OFS_ImGui::CurrentlyRenderedViewport = nullptr;

    // Update and Render additional Platform Windows
    // (Platform functions may change the current OpenGL context, so we save/restore it to make it easier to paste this code elsewhere.
    //  For this specific demo app we could also call SDL_GL_MakeCurrent(window, gl_context) directly)
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        {
            // ImGui::RenderPlatformWindowsDefault();
            // Skip the main viewport (index 0), which is always fully handled by the application!
            ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
            for (int i = 1; i < platform_io.Viewports.Size; i++) {
                ImGuiViewport* viewport = platform_io.Viewports[i];
                if (viewport->Flags & ImGuiViewportFlags_Minimized)
                    continue;
                OFS_ImGui::CurrentlyRenderedViewport = viewport;
                if (platform_io.Platform_RenderWindow) platform_io.Platform_RenderWindow(viewport, nullptr);
                if (platform_io.Renderer_RenderWindow) platform_io.Renderer_RenderWindow(viewport, nullptr);
            }
            OFS_ImGui::CurrentlyRenderedViewport = nullptr;
            for (int i = 1; i < platform_io.Viewports.Size; i++) {
                ImGuiViewport* viewport = platform_io.Viewports[i];
                if (viewport->Flags & ImGuiViewportFlags_Minimized)
                    continue;
                if (platform_io.Platform_SwapBuffers) platform_io.Platform_SwapBuffers(viewport, nullptr);
                if (platform_io.Renderer_SwapBuffers) platform_io.Renderer_SwapBuffers(viewport, nullptr);
            }
        }

        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }
    glFlush();
    glFinish();

    // The finished frame is still in the back buffer until the swap.
    if (uiDriver) uiDriver->AfterRender();
}

void OpenFunscripter::processEvents() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto wrappedEvent = EV::MakeTyped<OFS_SDL_Event>();
    auto& event = wrappedEvent->sdl;
    bool IsExiting = false;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        switch (event.type) {
            case SDL_QUIT: {
                if (!IsExiting) {
                    exitApp();
                    IsExiting = true;
                }
                break;
            }
            case SDL_WINDOWEVENT: {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
                    if (!IsExiting) {
                        exitApp();
                        IsExiting = true;
                    }
                }
                break;
            }
            case SDL_TEXTINPUT: {
                OFS_DynFontAtlas::AddText(event.text.text);
                break;
            }
        }

        switch (event.type) {
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(event.caxis.value) < 2000) break;
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEMOTION:
            case SDL_MOUSEWHEEL:
            case SDL_TEXTINPUT:
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERBUTTONDOWN:
                IdleTimer = SDL_GetTicks();
                setIdle(false);
                break;
        }

        // This is a slight hack in order to avoid creating a bunch of SDL_Event wrapper classes
        OFS_SDL_Event::EventType = event.type;
        EV::Queue().directDispatch(OFS_SDL_Event::EventType, wrappedEvent);
    }
    EV::Process();
}

void OpenFunscripter::ExportClip(const ExportClipForChapter* ev) noexcept
{
    const auto& ofsState = OpenFunscripterState::State(stateHandle);
    Util::OpenDirectoryDialog(TR(CHOOSE_OUTPUT_DIR), ofsState.lastPath,
        [chapter = ev->chapter](auto& result) {
            if (!result.files.empty()) {
                OFS_ChapterManager::ExportClip(chapter, result.files[0]);
            }
        });
}

void OpenFunscripter::FunscriptChanged(const FunscriptActionsChangedEvent* ev) noexcept
{
    // the event passes the address of the Funscript
    // by searching for the funscript with the same address
    // the index can be retrieved
    auto ptr = ev->Script;
    for (int i = 0, size = LoadedFunscripts().size(); i < size; i += 1) {
        if (LoadedFunscripts()[i].get() == ptr) {
            extensions->ScriptChanged(i);
            break;
        }
    }

    Status = Status | OFS_Status::OFS_GradientNeedsUpdate;
}

void OpenFunscripter::ScriptTimelineActionClicked(const FunscriptActionClickedEvent* ev) noexcept
{
    if (SDL_GetModState() & KMOD_CTRL) {
        if (auto script = ev->script.lock()) {
            script->SelectAction(ev->action);
        }
    }
    else {
        player->SetPositionExact(ev->action.atS);
    }
}

void OpenFunscripter::ScriptTimelineActionCreated(const FunscriptActionShouldCreateEvent* ev) noexcept
{
    if (auto script = ev->script.lock()) {
        // A click in an edited lane places the point in every edited lane,
        // as a key would, through the scripting mode: Alternating alternates
        // and Auto peak adds its peak. A click in a lane that is not being
        // edited places exactly what was clicked, there alone.
        if (IsEdited(script)) {
            auto targets = TargetedFunscripts();
            undoSystem->Snapshot(StateType::ADD_ACTION, undoContextFor(targets));
            addEditActionTo(targets, ev->newAction);
        }
        else {
            undoSystem->Snapshot(StateType::ADD_ACTION, script);
            script->AddEditAction(ev->newAction, scripting->LogicalFrameTime());
        }
    }
}

void OpenFunscripter::ScriptTimelineActionMoved(const FunscriptActionShouldMoveEvent* ev) noexcept
{
    auto script = ev->script.lock();
    if (!script) return;

    if (ev->moveStarted) {
        // Dragging a point in an edited lane carries the selection of every
        // edited lane along with it.
        std::vector<std::shared_ptr<Funscript>> lanes;
        if (IsEdited(script)) lanes = TargetedFunscripts();
        else lanes.push_back(script);
        UndoContextScripts snapshot;
        dragGroups.clear();
        for (auto& lane : lanes) {
            if (lane != script && !lane->HasSelection()) continue;
            dragGroups.push_back({ lane, lane->Selection() });
            snapshot.push_back(lane);
        }
        undoSystem->Snapshot(StateType::ACTIONS_MOVED, std::move(snapshot));
        dragGrabbed = ev->action;
        return;
    }

    // One millisecond, the resolution a funscript is saved at. Closer than
    // that, two points would share a timestamp once written out. The clamp
    // stops the group short of a neighbour instead of refusing the move, which
    // is what used to delete a single point dropped onto an occupied slot.
    constexpr float MinGap = 0.001f;
    const float wantTime = ev->action.atS - dragGrabbed.atS;
    const int32_t wantPos = ev->action.pos - dragGrabbed.pos;

    // Each lane says how far it can go, and every lane goes the shortest of
    // those, so the lanes move as one piece and keep their timing.
    bool any = false;
    FunscriptGroupMove::Offset offset{ wantTime, wantPos };
    std::vector<FunscriptArray> staying(dragGroups.size());
    for (size_t i = 0; i < dragGroups.size(); i += 1) {
        auto lane = dragGroups[i].script.lock();
        if (!lane || dragGroups[i].group.empty()) continue;
        // The points staying put. The selection is the group wherever the
        // last move left it, so taking it out leaves exactly the neighbours.
        staying[i].reserve(lane->Actions().size());
        for (auto action : lane->Actions()) {
            if (!lane->IsSelected(action)) staying[i].emplace_back_unsorted(action);
        }
        auto allowed = FunscriptGroupMove::Clamp(staying[i], dragGroups[i].group, wantTime, wantPos, MinGap);
        if (std::abs(allowed.time) < std::abs(offset.time)) offset.time = allowed.time;
        if (std::abs(allowed.pos) < std::abs(offset.pos)) offset.pos = allowed.pos;
        any = true;
    }
    if (!any) return;

    for (auto& drag : dragGroups) {
        auto lane = drag.script.lock();
        if (!lane || drag.group.empty()) continue;
        const auto moved = FunscriptGroupMove::Apply(drag.group, offset);
        lane->RemoveSelectedActions();
        lane->AddMultipleActions(moved);
        lane->SetSelection(moved);
    }
}

void OpenFunscripter::DragNDrop(const OFS_SDL_Event* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);

    std::string dragNDropFile = ev->sdl.drop.file;
    closeWithoutSavingDialog([this, dragNDropFile]() {
        openFile(dragNDropFile);
    });
    // NOTE: currently there is just one DragNDrop handler
    // If another one would be added this SDL_free would be problematic
    SDL_free(ev->sdl.drop.file);
}

void OpenFunscripter::VideoDuration(const DurationChangeEvent* ev) noexcept
{
    auto& projectState = LoadedProject->State();
    projectState.metadata.duration = player->Duration();
    player->SetPositionExact(projectState.lastPlayerPosition);
    Status |= OFS_Status::OFS_GradientNeedsUpdate;
}

void OpenFunscripter::VideoLoaded(const VideoLoadedEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // Ensure project metadata duration reflects the loaded video's duration
    if (LoadedProject && player) {
        auto& projectState = LoadedProject->State();
        projectState.metadata.duration = player->Duration();
    }
}

void OpenFunscripter::PlayPauseChange(const PlayPauseChangeEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!ev->paused) {
        IdleTimer = SDL_GetTicks();
        setIdle(false);
    }
}

void OpenFunscripter::update() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    const float delta = ImGui::GetIO().DeltaTime;
    keys->ProcessKeybindings();
    extensions->Update(delta);
    player->Update(delta);
    playerControls.videoPreview->Update(delta);
    ControllerInput::UpdateControllers();
    scripting->Update();
    scriptTimeline.Update();
    autoWaveformForAudio();

    if (LoadedProject->IsValid()) {
        LoadedProject->Update(delta, IdleMode);
    }

    if (Status & OFS_Status::OFS_AutoBackup) {
        autoBackup();
    }

    webApi->Update();
    deviceLink->Update(delta);
}

void OpenFunscripter::autoWaveformForAudio() noexcept
{
    // With no picture the waveform is the only view of what is playing, so an
    // audio file gets one straight away instead of from a menu.
    if (!player->IsAudioOnly()) {
        // Forgotten on close, so the same song opened again gets one again.
        autoWaveformMedia.clear();
        return;
    }
    const std::string media = player->VideoPath();
    if (media.empty() || media == autoWaveformMedia) return;
    // The timeline learns of the new file by event, which can land a frame
    // after the player knows. Until it does, a request would read the old file.
    if (scriptTimeline.MediaPath() != media) return;

    autoWaveformMedia = media;
    if (OFS_DownloadFfmpeg::FfmpegMissing) return;
    if (scriptTimeline.WaveformShown() || !scriptTimeline.CanGenerateWaveform()) return;
    scriptTimeline.RequestWaveform();
}

void OpenFunscripter::autoBackup() noexcept
{
    if (!LoadedProject->IsValid()) {
        return;
    }
    std::chrono::duration<float> timeSinceBackup = std::chrono::steady_clock::now() - lastBackup;
    if (timeSinceBackup.count() < AutoBackupIntervalSeconds) {
        return;
    }
    OFS_PROFILE(__FUNCTION__);
    lastBackup = std::chrono::steady_clock::now();

    auto backupDir = Util::PathFromString(Util::Prefpath("backup"));
    // A project with no media has no video name to file its backups under, so
    // they go under the project's own.
    auto name = LoadedProject->IsStandalone()
        ? Util::PathFromString(LoadedProject->Path()).stem().u8string()
        : Util::Filename(player->VideoPath());
    name = Util::trim(name); // this needs to be trimmed because trailing spaces

    static auto BackupStartPoint = asap::now();
    name = Util::Format("%s_%02d%02d%02d_%02d%02d%02d",
        name.c_str(), BackupStartPoint.year(),
        BackupStartPoint.month() + 1,
        BackupStartPoint.mday(),
        BackupStartPoint.hour(), BackupStartPoint.minute(), BackupStartPoint.second());

#ifdef WIN32
    backupDir /= Util::Utf8ToUtf16(name);
#else
    backupDir /= name;
#endif
    if (!Util::CreateDirectories(backupDir)) {
        return;
    }

    std::error_code ec;
    auto iterator = std::filesystem::directory_iterator(backupDir, ec);
    for (auto it = std::filesystem::begin(iterator); it != std::filesystem::end(iterator); ++it) {
        if (it->path().has_extension()) {
            if (it->path().extension() == ".backup") {
                LOGF_INFO("Removing \"%s\"", it->path().u8string().c_str());
                std::filesystem::remove(it->path(), ec);
                if (ec) {
                    LOGF_ERROR("%s", ec.message().c_str());
                }
            }
        }
    }

    auto time = asap::now();
    auto fileName = Util::PathFromString(Util::Format("%s_%02d-%02d-%02d" OFS_PROJECT_EXT ".backup", name.c_str(), time.hour(), time.minute(), time.second()));
    auto savePath = backupDir / fileName;
    LOGF_INFO("Backup at \"%s\"", savePath.u8string().c_str());
    LoadedProject->Save(savePath.u8string(), false);
}

void OpenFunscripter::exitApp(bool force) noexcept
{
    if (force) {
        Status |= OFS_Status::OFS_ShouldExit;
        return;
    }

    bool unsavedChanges = LoadedProject->HasUnsavedEdits();

    if (unsavedChanges) {
        Util::YesNoCancelDialog(TR(UNSAVED_CHANGES), TR(UNSAVED_CHANGES_MSG),
            [&](Util::YesNoCancel result) {
                if (result == Util::YesNoCancel::Yes) {
                    saveProject();
                    Status |= OFS_Status::OFS_ShouldExit;
                }
                else if (result == Util::YesNoCancel::No) {
                    Status |= OFS_Status::OFS_ShouldExit;
                }
                else {
                    // cancel does nothing
                    Status &= ~(OFS_Status::OFS_ShouldExit);
                }
            });
    }
    else {
        Status |= OFS_Status::OFS_ShouldExit;
    }
}

void OpenFunscripter::setIdle(bool idle) noexcept
{
    if (idle == IdleMode) return;
    if (idle && !player->IsPaused()) return; // can't idle while player is playing
    IdleMode = idle;
}

void OpenFunscripter::Step() noexcept
{
    OFS_BEGINPROFILING();
    {
        OFS_PROFILE(__FUNCTION__);
        processEvents();
        newFrame();
        update();
        {
            OFS_PROFILE("ImGui");
            // IMGUI HERE

            // Escape closes whatever menu, right click menu or dropdown is open.
            // ImGui only does that itself with keyboard navigation turned on,
            // which OFS leaves off, so Escape did nothing to an open menu, and
            // a click on a second menu after pressing it closed the first
            // instead of opening the second. Modals keep Escape to themselves:
            // the keys window uses it to cancel capturing a binding.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)
                && GImGui->OpenPopupStack.Size > 0
                && ImGui::GetTopMostPopupModal() == nullptr) {
                ImGui::ClosePopupsExceptModals();
            }

            CreateDockspace();
            blockingTask.ShowBlockingTask();

            auto& ofsState = OpenFunscripterState::State(stateHandle);
#ifdef WIN32
            if (OFS_DownloadFfmpeg::FfmpegMissing) {
                ImGui::OpenPopup(OFS_DownloadFfmpeg::ModalId);
                OFS_DownloadFfmpeg::DownloadFfmpegModal();
            }
#endif

            auto& overlayState = BaseOverlay::State();
            ShowAboutWindow(&ShowAbout);

            specialFunctions->ShowFunctionsWindow(&ofsState.showSpecialFunctions);
            undoSystem->ShowUndoRedoHistory(&ofsState.showHistory);
            // A click on a history entry, carried out once the window is done
            // drawing the stacks it would change.
            for (; undoSystem->PendingUndoSteps > 0; undoSystem->PendingUndoSteps -= 1) Undo();
            for (; undoSystem->PendingRedoSteps > 0; undoSystem->PendingRedoSteps -= 1) Redo();
            simulator.ShowSimulator(&ofsState.showSimulator, ActiveFunscript(), player->CurrentTime(), overlayState.SplineMode);

            if (ShowMetadataEditor) {
                auto& projectState = LoadedProject->State();
                projectState.metadata.duration = player->Duration();
                if (metadataEditor->ShowMetadataEditor(&ShowMetadataEditor, projectState.metadata)) {
                    EV::Enqueue<MetadataChanged>();
                }
            }

            webApi->ShowWindow(&ofsState.showWsApi);
            scripting->DrawScriptingMode(NULL);
            LoadedProject->ShowProjectWindow(&ShowProjectEditor);
            ShowNewScriptWindow(&ShowNewScriptDialog);

            extensions->ShowExtensions();
            OFS_FileLogger::DrawLogWindow(&ofsState.showDebugLog);
            keys->RenderKeybindingWindow();
            chapterMgr->ShowWindow(&ofsState.showChapterManager);
            scriptCheck->ShowWindow(&ofsState.showScriptCheck);
            deviceLink->DrawWindow(&ofsState.showDevices);

            if (preferences->ShowPreferenceWindow()) {}

            if (Status & OFS_GradientNeedsUpdate) {
                Status &= ~(OFS_GradientNeedsUpdate);
                playerControls.UpdateHeatmap(player->Duration(), ActiveFunscript()->Actions());
            }

            playerControls.DrawTimeline();

            scriptTimeline.ShowScriptPositions(player.get(),
                scripting->Overlay().get(),
                LoadedFunscripts(),
                LoadedProject->ActiveIdx());

            ShowStatisticsWindow(&ofsState.showStatistics);
            ShowTracksWindow(&ofsState.showTracks);
            drawRemoveTrackModal();

            if (ofsState.showActionEditor) {
                ImGui::Begin(TR_ID(ActionEditorWindowId, Tr::ACTION_EDITOR), &ofsState.showActionEditor);
                OFS_PROFILE(ActionEditorWindowId);

                // 100 across the top, 90 to 10 as three rows of three, 0 across
                // the bottom: the same keypad as before, sized by hand instead of
                // through the old Columns API.
                const auto& style = ImGui::GetStyle();
                const float fullWidth = ImGui::GetContentRegionAvail().x;
                const float thirdWidth = (fullWidth - (style.ItemSpacing.x * 2.f)) / 3.f;
                char positionLabel[8];

                if (ImGui::Button("100", ImVec2(fullWidth, 0.f))) {
                    addEditAction(100);
                }
                for (int row = 0; row < 3; row += 1) {
                    for (int column = 0; column < 3; column += 1) {
                        const int position = (9 - (row * 3) - column) * 10;
                        if (column > 0) ImGui::SameLine();
                        stbsp_snprintf(positionLabel, sizeof(positionLabel), "%d", position);
                        if (ImGui::Button(positionLabel, ImVec2(thirdWidth, 0.f))) {
                            addEditAction(position);
                        }
                    }
                }
                if (ImGui::Button("0", ImVec2(fullWidth, 0.f))) {
                    addEditAction(0);
                }
                OFS::Tooltip("Places a point at the playhead with this position, "
                             "through the current scripting mode.");

                ImGui::Spacing();
                if (player->IsPaused()) {
                    auto scriptAction = ActiveFunscript()->GetActionAtTime(player->CurrentTime(), scripting->LogicalFrameTime());
                    if (!scriptAction) {
                        // create action
                        static int newActionPosition = 0;
                        ImGui::SetNextItemWidth(-1.f);
                        ImGui::SliderInt("##Position", &newActionPosition, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
                        if (ImGui::Button(TR(ADD_ACTION), ImVec2(-1.f, 0.f))) {
                            addEditAction(newActionPosition);
                        }
                    }
                    else {
                        ImGui::PushTextWrapPos(0.f);
                        ImGui::TextDisabled("A point already sits at the playhead, at %d.", scriptAction->pos);
                        ImGui::PopTextWrapPos();
                    }
                }
                else {
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextDisabled("Pause to place a point at any position with a slider.");
                    ImGui::PopTextWrapPos();
                }
                ImGui::End();
            }

#ifndef NDEBUG
            if (DebugDemo) {
                ImGui::ShowDemoWindow(&DebugDemo);
            }
#endif
            if (DebugMetrics) {
                ImGui::ShowMetricsWindow(&DebugMetrics);
            }

            playerWindow->DrawVideoPlayer(NULL, &ofsState.showVideo);
        }

        render();
    }

    OFS_FileLogger::Flush();
    OFS_ENDPROFILING();
    SDL_GL_SwapWindow(window);
    player->NotifySwap();
}

int OpenFunscripter::Run() noexcept
{
    newFrame();
    setupDefaultLayout(false);
    render();

    const uint64_t PerfFreq = SDL_GetPerformanceFrequency();
    while (!(Status & OFS_Status::OFS_ShouldExit)) {

        uint64_t FrameStart = SDL_GetPerformanceCounter();
        OFS_CrashHandler::Heartbeat();
        Step();
        uint64_t FrameEnd = SDL_GetPerformanceCounter();

        const auto& prefState = PreferenceState::State(preferences->StateHandle());
        float frameLimit = IdleMode ? 10.f : (float)prefState.framerateLimit;
        const float minFrameTime = (float)PerfFreq / frameLimit;

        int32_t sleepMs = ((minFrameTime - (float)(FrameEnd - FrameStart)) / minFrameTime) * (1000.f / frameLimit);
        if (!IdleMode) sleepMs -= 1;
        if (sleepMs > 0) SDL_Delay(sleepMs);

        if (!prefState.vsync) {
            FrameEnd = SDL_GetPerformanceCounter();
            while ((FrameEnd - FrameStart) < minFrameTime) {
                OFS_PAUSE_INTRIN();
                FrameEnd = SDL_GetPerformanceCounter();
            }
        }

        if (SDL_GetTicks() - IdleTimer > 3000) {
            setIdle(true);
        }
    }
    return 0;
}

void OpenFunscripter::Shutdown() noexcept
{
    SaveState();

    OFS_DynFontAtlas::Shutdown();
    OFS_Translator::Shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    // These players need to be freed before unloading mpv
    // NOTE: Do not free the GL context before these players
    player.reset();
    playerControls.videoPreview.reset();
    OFS_MpvLoader::Unload();
    OFS_FileLogger::Shutdown();
    webApi->Shutdown();
    controllerInput->Shutdown();

    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void OpenFunscripter::Undo() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (undoSystem->Undo()) scripting->Undo();
}

void OpenFunscripter::Redo() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (undoSystem->Redo()) scripting->Redo();
}

void OpenFunscripter::openFile(const std::string& file) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!Util::FileExists(file)) {
        Util::MessageBoxAlert(TR(FILE_NOT_FOUND), std::string(TR(COULDNT_FIND_FILE)) + "\n" + file);
        return;
    }

    // If a project with the same name exists, it's opened instead.
    auto testProjectPath = Util::PathFromString(file);
    if (testProjectPath.extension().u8string() != OFS_Project::Extension) {
        testProjectPath.replace_extension(OFS_Project::Extension);
        if (Util::FileExists(testProjectPath.u8string())) {
            openFile(testProjectPath.u8string());
            return;
        }
    }

    closeWithoutSavingDialog(
        [this, file]() noexcept {
            auto filePath = Util::PathFromString(file);
            auto fileExtension = filePath.extension().u8string();
            LoadedProject = std::make_unique<OFS_Project>();
            OFS_StateManager::Get()->ClearProjectAll();

            if (fileExtension == OFS_Project::Extension) {
                // It's a project
                LoadedProject->Load(file);
            }
            else if (fileExtension == Funscript::Extension) {
                // It's a funscript it should be imported into a new project
                LoadedProject->ImportFromFunscript(file);
            }
            else {
                // Assume it's some kind of media file
                LoadedProject->ImportFromMedia(file);
            }

            if (LoadedProject->IsValid()) {
                initProject();
            }
            else {
                Util::MessageBoxAlert("Failed to open file.", LoadedProject->NotValidError());
            }
        });
}

void OpenFunscripter::createStandaloneProject(float durationSeconds) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& ofsState = OpenFunscripterState::State(stateHandle);
    Util::SaveFileDialog(
        "New script without video", ofsState.lastPath,
        [this, durationSeconds](auto& result) {
            if (result.files.empty()) return;
            startStandaloneProject(result.files[0], durationSeconds);
        },
        { "Funscript", "*.funscript" });
}

void OpenFunscripter::startStandaloneProject(const std::string& file, float durationSeconds) noexcept
{
    closeWithoutSavingDialog([this, file, durationSeconds]() noexcept {
        LoadedProject = std::make_unique<OFS_Project>();
        OFS_StateManager::Get()->ClearProjectAll();

        if (LoadedProject->CreateStandalone(file, durationSeconds)) {
            initProject();
            // Written out at once. A project with no media cannot be found
            // again from the file that was scripted, the way one opened from a
            // video can, so it has to exist on disk from the start to be
            // reopenable at all.
            saveProject();
        }
        else {
            Util::MessageBoxAlert("Failed to create script.", LoadedProject->NotValidError());
        }
    });
}

void OpenFunscripter::initProject() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // Whatever was open before is gone, and so is anything its edits could undo.
    undoSystem->Clear();
    if (LoadedProject->IsValid()) {
        auto& projectState = LoadedProject->State();

        // The title is the one piece of metadata the media already knows, so
        // opening a dialog to ask for it was asking a question that had an
        // answer. Taken from the file name, which is what someone would have
        // typed in anyway, and only when there is nothing there to overwrite:
        // a script that arrived with a title keeps it.
        if (projectState.metadata.title.empty()) {
            // With no media the project file's own name is the only name there
            // is, and it is the one that was just typed into the save dialog.
            auto namedAfter = Util::PathFromString(LoadedProject->IsStandalone()
                    ? LoadedProject->Path()
                    : LoadedProject->MediaPath());
            projectState.metadata.title = namedAfter.stem().u8string();
        }

        if (projectState.nudgeMetadata) {
            const auto& prefState = PreferenceState::State(preferences->StateHandle());
            ShowMetadataEditor = prefState.showMetaOnNew;
            projectState.nudgeMetadata = false;
        }

        if (LoadedProject->IsStandalone()) {
            // Nothing to load and nothing to go looking for: the timeline is
            // the whole of it, at the length the project was saved with.
            player->OpenBlank(projectState.standaloneDuration);
        }
        else if (Util::FileExists(LoadedProject->MediaPath())) {
            player->OpenVideo(LoadedProject->MediaPath());
        }
        else {
            pickDifferentMedia();
        }
    }
    updateTitle();

    auto lastPath = Util::PathFromString(LoadedProject->Path());
    lastPath.remove_filename();

    auto& ofsState = OpenFunscripterState::State(stateHandle);
    ofsState.lastPath = lastPath.u8string();

    lastBackup = std::chrono::steady_clock::now();
    EV::Enqueue<ProjectLoadedEvent>();
}

void OpenFunscripter::UpdateNewActiveScript(uint32_t activeIndex) noexcept
{
    LoadedProject->SetActiveIdx(activeIndex);
    updateTitle();
    Status = Status | OFS_Status::OFS_GradientNeedsUpdate;
}

void OpenFunscripter::updateTitle() noexcept
{
    const char* title = "OFS-SE";
    if (LoadedProject->IsValid()) {
        title = Util::Format("OFS-SE %s@%s - \"%s\"",
            OFS_LATEST_GIT_TAG,
            OFS_LATEST_GIT_HASH,
            LoadedProject->Path().c_str());
    }
    else {
        title = Util::Format("OFS-SE %s@%s",
            OFS_LATEST_GIT_TAG,
            OFS_LATEST_GIT_HASH);
    }
    SDL_SetWindowTitle(window, title);
}

void OpenFunscripter::saveProject() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& projectState = LoadedProject->State();
    projectState.lastPlayerPosition = player->CurrentTime();
    LoadedProject->Save(true);

    auto& ofsState = OpenFunscripterState::State(stateHandle);
    auto recentFile = RecentFile{ Util::PathFromString(LoadedProject->Path()).filename().u8string(), LoadedProject->Path() };
    ofsState.addRecentFile(recentFile);
}

void OpenFunscripter::quickExport() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    LoadedProject->ExportFunscripts();
}

// Writes a copy for a device: everything the script check would complain about
// is taken out of the copy, and the project keeps the script as it was
// written. A folder rather than a file, since a project can hold an axis
// script per channel and they have to stay together.
void OpenFunscripter::exportForDevice() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& ofsState = OpenFunscripterState::State(stateHandle);
    Util::OpenDirectoryDialog("Export for device", ofsState.lastPath,
        [this](auto& result) noexcept {
            if (result.files.empty()) return;
            const auto limits = scriptCheck->CurrentLimits();
            std::vector<FunscriptArray> limited;
            limited.reserve(LoadedFunscripts().size());
            for (auto& script : LoadedFunscripts()) {
                limited.emplace_back(
                    OFS_Check::ThinOut(OFS_Check::LimitSpeed(script->Actions(), limits), limits));
            }
            LoadedProject->ExportFunscriptsLimited(result.files[0], limited);
        });
}

void OpenFunscripter::quickExport2() noexcept
{
	OFS_PROFILE(__FUNCTION__);
	LoadedProject->ExportFunscript2Quick();
}

bool OpenFunscripter::closeProject(bool closeWithUnsavedChanges) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!closeWithUnsavedChanges && LoadedProject->HasUnsavedEdits()) {
        FUN_ASSERT(false, "this branch should ideally never be taken");
        return false;
    }
    else {
        UpdateNewActiveScript(0);
        LoadedProject = std::make_unique<OFS_Project>();
        undoSystem->Clear();
        player->CloseVideo();
        playerControls.videoPreview->CloseVideo();
        updateTitle();
    }
    return true;
}

void OpenFunscripter::pickDifferentMedia() noexcept
{
    if (LoadedProject->IsValid()) {
        auto& projectState = LoadedProject->State();
        Util::OpenFileDialog(
            TR(PICK_DIFFERENT_MEDIA), LoadedProject->MediaPath(),
            [this](auto& result) {
                auto& projectState = LoadedProject->State();
                if (!result.files.empty() && Util::FileExists(result.files[0])) {
                    projectState.relativeMediaPath = LoadedProject->MakePathRelative(result.files[0]);
                    player->OpenVideo(LoadedProject->MediaPath());
                }
            },
            false);
    }
}

void OpenFunscripter::saveHeatmap(const char* path, int width, int height, bool withChapters)
{
    OFS_PROFILE(__FUNCTION__);
    if (withChapters) {
        auto bitmap = playerControls.RenderHeatmapToBitmapWithChapters(width, height, height);
        Util::SavePNG(path, bitmap.data(), width, height + height, 4);
    }
    else {
        auto bitmap = playerControls.Heatmap->RenderToBitmap(width, height);
        Util::SavePNG(path, bitmap.data(), width, height, 4);
    }
}

std::vector<std::shared_ptr<Funscript>> OpenFunscripter::TargetedFunscripts() noexcept
{
    // The track you're on is always edited, and leads. Every other showing
    // track whose box is checked is edited along with it, whichever track
    // is current.
    std::vector<std::shared_ptr<Funscript>> targets;
    auto active = ActiveFunscript();
    targets.push_back(active);
    for (auto& script : LoadedFunscripts()) {
        if (script != active && script->Enabled && script->Targeted) {
            targets.push_back(script);
        }
    }
    return targets;
}

bool OpenFunscripter::IsEdited(const std::shared_ptr<Funscript>& script) noexcept
{
    for (auto& target : TargetedFunscripts()) {
        if (target == script) return true;
    }
    return false;
}

// The scripts of an edit, as the undo system wants them, so one undo takes
// back what one key did to every edited lane.
static UndoContextScripts undoContextFor(const std::vector<std::shared_ptr<Funscript>>& targets) noexcept
{
    UndoContextScripts scripts;
    scripts.reserve(targets.size());
    for (auto& script : targets) scripts.push_back(script);
    return scripts;
}

bool OpenFunscripter::targetsHaveSelection() noexcept
{
    return selectionLead() != nullptr;
}

std::shared_ptr<Funscript> OpenFunscripter::selectionLead() noexcept
{
    for (auto& script : TargetedFunscripts()) {
        if (script->HasSelection()) return script;
    }
    return nullptr;
}

void OpenFunscripter::removeAction() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto targets = TargetedFunscripts();
    if (targetsHaveSelection()) {
        // A selection made by dragging across the timeline lands in every
        // edited lane, so deleting it clears them all.
        undoSystem->Snapshot(StateType::REMOVE_SELECTION, undoContextFor(targets));
        for (auto& script : targets) {
            if (script->HasSelection()) script->RemoveSelectedActions();
        }
        return;
    }
    // Nothing selected: the point nearest the playhead, in each edited lane.
    std::vector<std::pair<std::shared_ptr<Funscript>, FunscriptAction>> removing;
    for (auto& script : targets) {
        auto closest = script->GetClosestAction(player->CurrentTime());
        if (closest != nullptr) removing.emplace_back(script, *closest);
    }
    if (removing.empty()) return;
    UndoContextScripts scripts;
    for (auto& [script, action] : removing) scripts.push_back(script);
    undoSystem->Snapshot(StateType::REMOVE_ACTION, std::move(scripts));
    for (auto& [script, action] : removing) script->RemoveAction(action);
}

void OpenFunscripter::addEditActionTo(const std::vector<std::shared_ptr<Funscript>>& targets, FunscriptAction action) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // Every lane runs the same mode with the lane swapped in underneath, so
    // Alternating alternates and Auto peak adds its peak in each one. The
    // mode moves on to its next step once, after the last lane, so every
    // lane gets the same step.
    for (size_t i = 0; i < targets.size(); i += 1) {
        ScriptingModeBase::TargetOverride = targets[i].get();
        ScriptingModeBase::HoldState = i + 1 < targets.size();
        scripting->AddEditAction(action);
    }
    ScriptingModeBase::TargetOverride = nullptr;
    ScriptingModeBase::HoldState = false;
}

void OpenFunscripter::addEditAction(int pos) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto targets = TargetedFunscripts();
    undoSystem->Snapshot(StateType::ADD_EDIT_ACTIONS, undoContextFor(targets));
    addEditActionTo(targets, FunscriptAction(player->CurrentTime(), pos));
}

void OpenFunscripter::cutSelection() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (targetsHaveSelection()) {
        copySelection();
        auto targets = TargetedFunscripts();
        undoSystem->Snapshot(StateType::CUT_SELECTION, undoContextFor(targets));
        for (auto& script : targets) {
            if (script->HasSelection()) script->RemoveSelectedActions();
        }
    }
}

void OpenFunscripter::copySelection() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto lead = selectionLead();
    if (lead == nullptr) return;
    CopiedSelection.clear();
    CopiedTracks.clear();
    for (auto action : lead->Selection()) {
        CopiedSelection.emplace(action);
    }
    // Each edited lane's selection is remembered with the lane it came from,
    // so a paste puts every part back where it belongs.
    for (auto& script : TargetedFunscripts()) {
        if (!script->HasSelection()) continue;
        CopiedTrack track;
        track.script = script;
        for (auto action : script->Selection()) track.actions.emplace(action);
        CopiedTracks.push_back(std::move(track));
    }
}

void OpenFunscripter::moveSelectionTime(bool forward) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto lead = selectionLead();
    if (lead == nullptr) return;
    auto targets = TargetedFunscripts();

    // One step from the leading lane's selection, then cut down to what every
    // lane has room for, so the lanes move together and stay in step.
    const float from = lead->Selection().front().atS;
    const float frameTime = scripting->LogicalFrameTime();
    float time = forward
        ? scripting->SteppingIntervalForward(from)
        : scripting->SteppingIntervalBackward(from);
    for (auto& script : targets) {
        if (!script->HasSelection()) continue;
        float limited = script->LimitSelectionTimeOffset(time, frameTime);
        time = forward ? Util::Min(time, limited) : Util::Max(time, limited);
    }

    undoSystem->Snapshot(StateType::ACTIONS_MOVED, undoContextFor(targets));
    for (auto& script : targets) {
        if (script->HasSelection()) script->MoveSelectionTime(time, frameTime);
    }
}

void OpenFunscripter::moveSelectionPosition(int32_t delta) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!targetsHaveSelection()) return;
    auto targets = TargetedFunscripts();
    undoSystem->Snapshot(StateType::ACTIONS_MOVED, undoContextFor(targets));
    for (auto& script : targets) {
        if (script->HasSelection()) script->MoveSelectionPosition(delta);
    }
}

// A point moved in each lane that has one to move, as one undo.
struct ClosestEdit
{
    std::shared_ptr<Funscript> script;
    FunscriptAction from;
    FunscriptAction to;
};

static void applyClosestEdits(OpenFunscripter* app, StateType type, const std::vector<ClosestEdit>& edits) noexcept
{
    if (edits.empty()) return;
    UndoContextScripts scripts;
    for (auto& edit : edits) scripts.push_back(edit.script);
    app->undoSystem->Snapshot(type, std::move(scripts));
    for (auto& edit : edits) edit.script->EditAction(edit.from, edit.to);
}

std::optional<float> OpenFunscripter::moveClosestTime(bool forward) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    const float now = player->CurrentTime();
    const float frameTime = scripting->LogicalFrameTime();
    std::vector<ClosestEdit> edits;
    for (auto& script : TargetedFunscripts()) {
        auto closest = script->GetClosestAction(now);
        if (closest == nullptr) continue;
        auto time = forward
            ? scripting->SteppingIntervalForward(closest->atS)
            : scripting->SteppingIntervalBackward(closest->atS);
        FunscriptAction moved(closest->atS + time, closest->pos);
        // Not onto or past a neighbour.
        auto inMoveRange = script->GetActionAtTime(moved.atS, frameTime);
        if (inMoveRange == nullptr
            || (forward && inMoveRange->atS < moved.atS)
            || (!forward && inMoveRange->atS > moved.atS)) {
            edits.push_back({ script, *closest, moved });
        }
    }
    applyClosestEdits(this, StateType::ACTIONS_MOVED, edits);
    if (edits.empty()) return std::nullopt;
    return edits.front().to.atS;
}

void OpenFunscripter::moveClosestPosition(int32_t delta) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<ClosestEdit> edits;
    for (auto& script : TargetedFunscripts()) {
        auto closest = script->GetClosestAction(player->CurrentTime());
        if (closest == nullptr) continue;
        FunscriptAction moved(closest->atS, Util::Clamp<int32_t>(closest->pos + delta, 0, 100));
        if (moved.pos != closest->pos) edits.push_back({ script, *closest, moved });
    }
    applyClosestEdits(this, StateType::ACTIONS_MOVED, edits);
}

void OpenFunscripter::moveClosestToPlayhead() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    const float now = player->CurrentTime();
    std::vector<ClosestEdit> edits;
    for (auto& script : TargetedFunscripts()) {
        auto closest = script->GetClosestAction(now);
        if (closest != nullptr) edits.push_back({ script, *closest, FunscriptAction(now, closest->pos) });
    }
    applyClosestEdits(this, StateType::MOVE_ACTION_TO_CURRENT_POS, edits);
}

void OpenFunscripter::selectAll() noexcept
{
    for (auto& script : TargetedFunscripts()) script->SelectAll();
}

void OpenFunscripter::clearSelection() noexcept
{
    for (auto& script : TargetedFunscripts()) script->ClearSelection();
}

void OpenFunscripter::selectTime(float start, float end) noexcept
{
    for (auto& script : TargetedFunscripts()) script->SelectTime(start, end);
}

bool OpenFunscripter::trackLoaded(const std::string& path) noexcept
{
    // By file name, as the Add menu always has: the same name in another
    // folder would save over the one already loaded.
    const auto filename = Util::PathFromString(path).filename().u8string();
    for (auto& script : LoadedFunscripts()) {
        if (Util::PathFromString(script->RelativePath()).filename().u8string() == filename) return true;
    }
    return false;
}

std::string OpenFunscripter::axisTrackPath(const char* axis) noexcept
{
    if (LoadedFunscripts().empty()) return std::string();
    auto root = Util::PathFromString(LoadedProject->MakePathAbsolute(LoadedFunscripts()[0]->RelativePath()));
    root.replace_extension(Util::Format(".%s.funscript", axis));
    return root.u8string();
}

void OpenFunscripter::addAxisTrack(const char* axis) noexcept
{
    auto path = axisTrackPath(axis);
    if (!path.empty() && !trackLoaded(path)) LoadedProject->AddFunscript(path);
}

void OpenFunscripter::AddMachineTracks(int32_t machine) noexcept
{
    if (machine < 0 || machine >= TCodeMachineCount || !LoadedProject->IsValid()) return;
    const auto& picked = TCodeMachines[machine];
    for (int32_t i = 0; i < picked.axisCount; i += 1) {
        // The main track is the stroke.
        if (std::strcmp(picked.axes[i], "stroke") == 0) continue;
        addAxisTrack(picked.axes[i]);
    }
}

void OpenFunscripter::addNewTrackDialog() noexcept
{
    auto& ofsState = OpenFunscripterState::State(stateHandle);
    Util::SaveFileDialog(TR(ADD_NEW_FUNSCRIPT), ofsState.lastPath,
        [](auto& result) noexcept {
            auto app = OpenFunscripter::ptr;
            if (!result.files.empty() && !app->trackLoaded(result.files[0])) {
                app->LoadedProject->AddFunscript(result.files[0]);
            }
        },
        { "Funscript", "*.funscript" });
}

void OpenFunscripter::loadTracksDialog() noexcept
{
    auto& ofsState = OpenFunscripterState::State(stateHandle);
    Util::OpenFileDialog(TR(ADD_EXISTING_FUNSCRIPTS), ofsState.lastPath,
        [](auto& result) noexcept {
            auto app = OpenFunscripter::ptr;
            for (auto& scriptPath : result.files) {
                if (!app->trackLoaded(scriptPath)) app->LoadedProject->AddFunscript(scriptPath);
            }
        },
        true, { "*.funscript" }, "Funscript");
}

void OpenFunscripter::removeTrack(int32_t idx) noexcept
{
    auto& scripts = LoadedFunscripts();
    if (idx < 0 || idx >= (int32_t)scripts.size() || scripts.size() <= 1) return;
    // Asked in drawRemoveTrackModal. Held by the track rather than its place
    // in the list, since the list can change while the question is open.
    trackToRemove = scripts[idx];
}

void OpenFunscripter::removeTrackNow(const std::shared_ptr<Funscript>& script) noexcept
{
    auto& scripts = LoadedFunscripts();
    auto it = std::find(scripts.begin(), scripts.end(), script);
    if (!script || it == scripts.end() || scripts.size() <= 1) return;

    // The current track stays current. When it is the one removed, the track
    // that takes its place in the list does, or the one before it at the end,
    // and it is shown if it was hidden.
    auto current = ActiveFunscript();
    const int32_t removed = (int32_t)(it - scripts.begin());
    LoadedProject->RemoveFunscript(removed);
    int32_t next = 0;
    if (current != script) {
        next = (int32_t)(std::find(scripts.begin(), scripts.end(), current) - scripts.begin());
    }
    else {
        next = Util::Min(removed, (int32_t)scripts.size() - 1);
        scripts[next]->Enabled = true;
    }
    LoadedProject->SetActiveIdx(next);
    updateTitle();
    Status = Status | OFS_Status::OFS_GradientNeedsUpdate;
}

void OpenFunscripter::drawRemoveTrackModal() noexcept
{
    constexpr const char* PopupId = "Remove track?###REMOVE_TRACK";
    auto script = trackToRemove.lock();
    if (script && !ImGui::IsPopupOpen(PopupId)) ImGui::OpenPopup(PopupId);
    if (!ImGui::BeginPopupModal(PopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    bool close = false;
    if (script) {
        ImGui::Text("Remove %s from the project?", script->Title().c_str());
        ImGui::TextDisabled("The file stays on disk, but changes that weren't saved\n"
                            "to it or exported are lost. This can't be undone.");
        ImGui::Spacing();
        // Enter removes and Escape cancels, as in a system dialog.
        if (ImGui::Button("Remove") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            removeTrackNow(script);
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    else {
        close = true;
    }
    if (close) {
        trackToRemove.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void OpenFunscripter::setTrackShown(int32_t idx, bool shown) noexcept
{
    auto& scripts = LoadedFunscripts();
    if (idx < 0 || idx >= (int32_t)scripts.size()) return;
    if (shown) {
        scripts[idx]->Enabled = true;
        return;
    }
    // One track always stays showing.
    int32_t showing = 0;
    for (auto& script : scripts) showing += script->Enabled ? 1 : 0;
    if (!scripts[idx]->Enabled || showing <= 1) return;
    scripts[idx]->Enabled = false;
    // Hiding the current track moves to another that is showing, as the eye
    // in the lane header does.
    if (idx == (int32_t)LoadedProject->ActiveIdx()) {
        for (int32_t other = 0; other < (int32_t)scripts.size(); other += 1) {
            if (scripts[other]->Enabled) {
                EV::Enqueue<ShouldChangeActiveScriptEvent>(other);
                break;
            }
        }
    }
}

// A tooltip on a control that may be disabled, which the usual one skips.
static void trackTooltip(const char* tip) noexcept
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        OFS::ShowWrappedTooltip(tip);
    }
}

void OpenFunscripter::ShowTracksWindow(bool* open) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);

    // Not in the layout it was saved with: put it left of the timeline, where
    // the tracks it lists are, rather than floating over the video.
    if (ImGui::FindWindowByName(TracksWindowId) == nullptr
        && ImGui::FindWindowSettings(ImHashStr(TracksWindowId)) == nullptr) {
        auto timeline = ImGui::FindWindowByName(ScriptTimeline::WindowId);
        if (timeline != nullptr && timeline->DockId != 0) {
            ImGuiID tracksId = 0, timelineId = 0;
            ImGui::DockBuilderSplitNode(timeline->DockId, ImGuiDir_Left, 0.14f, &tracksId, &timelineId);
            ImGui::DockBuilderDockWindow(TracksWindowId, tracksId);
            ImGui::DockBuilderFinish(tracksId);
        }
    }

    ImGui::Begin(TracksWindowId, open);
    if (!LoadedProject->IsValid() || LoadedFunscripts().empty()) {
        ImGui::TextDisabled("Open a video or a script to see its tracks.");
        ImGui::End();
        return;
    }

    auto& scripts = LoadedFunscripts();
    const int32_t current = (int32_t)LoadedProject->ActiveIdx();

    if (ImGui::Button("Add")) ImGui::OpenPopup("##addTrack");
    trackTooltip("Add a track: an axis like twist or roll beside the main script, or a new file.");
    ImGui::SameLine();
    if (ImGui::Button("Load...")) loadTracksDialog();
    trackTooltip("Load .funscript files as tracks.");
    if (ImGui::BeginPopup("##addTrack")) {
        // A whole machine's worth of tracks at once, and the Devices panel
        // set to send to it.
        ImGui::TextDisabled("Machine");
        for (int32_t m = 0; m < TCodeMachineCount; m += 1) {
            if (ImGui::MenuItem(TCodeMachines[m].name)) {
                AddMachineTracks(m);
                deviceLink->UseMachine(m);
            }
            const std::string tip = std::string(TCodeMachines[m].tip)
                + " Adds the tracks it needs that aren't here yet, and sets the Devices panel to send to it.";
            trackTooltip(tip.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("Axis");
        for (auto axis : Funscript::AxisNames) {
            if (ImGui::MenuItem(axis, nullptr, false, !trackLoaded(axisTrackPath(axis)))) addAxisTrack(axis);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New file...")) addNewTrackDialog();
        ImGui::EndPopup();
    }

    int32_t showing = 0;
    for (auto& script : scripts) showing += script->Enabled ? 1 : 0;

    int32_t switchTo = -1;
    // Dragging a row's grip reorders. Where each row sits, to tell which row
    // the mouse is over while dragging.
    int32_t dragging = -1;
    std::vector<float> rowMiddles;
    rowMiddles.reserve(scripts.size());
    int32_t removeAt = -1;
    // Compact rows, since the panel shares the height of the timeline and
    // should show as many tracks as the timeline shows lanes.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1.f));
    const float box = ImGui::GetFrameHeight();
    const float grip = box * 0.6f;
    if (ImGui::BeginTable("##tracks", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("##grip", ImGuiTableColumnFlags_WidthFixed, grip);
        ImGui::TableSetupColumn("##shown", ImGuiTableColumnFlags_WidthFixed, box);
        ImGui::TableSetupColumn("##edited", ImGuiTableColumnFlags_WidthFixed, box);
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, box);

        for (int32_t i = 0; i < (int32_t)scripts.size(); i += 1) {
            auto& script = scripts[i];
            const bool isCurrent = i == current;
            // By track rather than by row, so a row being dragged keeps hold
            // of the mouse as it moves to another row.
            ImGui::PushID(script.get());
            ImGui::TableNextRow();

            // Grip: drag it to change the order. Two columns of dots, the
            // usual sign of something that can be dragged. The main track
            // stays first, so it has none.
            ImGui::TableNextColumn();
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##grip", ImVec2(grip, box));
                rowMiddles.push_back((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
                if (i > 0) {
                    const bool hovered = ImGui::IsItemHovered();
                    const bool active = ImGui::IsItemActive();
                    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f)) dragging = i;
                    const uint32_t color = active ? OFS_Sashimi::Role().AccentBright
                        : (hovered ? OFS_Sashimi::Grey95 : OFS_Sashimi::Grey50);
                    auto drawList = ImGui::GetWindowDrawList();
                    const float dot = Util::Max(1.5f, box * 0.07f);
                    const ImVec2 center = pos + ImVec2(grip * 0.5f, box * 0.5f);
                    for (int32_t row = -1; row <= 1; row += 1) {
                        for (int32_t col = 0; col < 2; col += 1) {
                            drawList->AddCircleFilled(center + ImVec2((col == 0 ? -1.f : 1.f) * dot * 1.6f, row * dot * 3.2f), dot, color, 6);
                        }
                    }
                    trackTooltip("Drag to change the order of the tracks, here and in the timeline.");
                }
                else {
                    trackTooltip("The main track. It stays first: exports and devices treat it as the stroke.");
                }
            }

            // Eye: shown in the timeline or not. One track always stays shown.
            ImGui::TableNextColumn();
            {
                const bool canToggle = !script->Enabled || showing > 1;
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##eye", ImVec2(box, box)) && canToggle) {
                    setTrackShown(i, !script->Enabled);
                }
                const bool hovered = ImGui::IsItemHovered();
                if (hovered && canToggle) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                auto drawList = ImGui::GetWindowDrawList();
                const uint32_t color = !canToggle ? IM_COL32(0x4A, 0x4A, 0x4A, 0xFF)
                    : script->Enabled ? (hovered ? OFS_Sashimi::Grey95 : OFS_Sashimi::Grey60)
                    : (hovered ? OFS_Sashimi::Grey80 : IM_COL32(0x5A, 0x5A, 0x5A, 0xFF));
                const ImVec2 center = pos + ImVec2(box * 0.5f, box * 0.5f);
                ScriptTimeline::DrawEye(drawList, center, box * 0.75f, color);
                if (!script->Enabled) {
                    // Struck through while hidden.
                    drawList->AddLine(center + ImVec2(-box * 0.3f, box * 0.3f), center + ImVec2(box * 0.3f, -box * 0.3f), color, 1.5f);
                }
                trackTooltip(!canToggle ? "The only track showing, so it stays."
                    : script->Enabled ? "Shown in the timeline. Click to hide it."
                    : "Hidden. Click to show it in the timeline.");
            }

            // Edit checkbox, as in the lane header: the track you're on shows a
            // dim mark when it isn't checked, since it is edited only while
            // you're on it. Hidden tracks aren't edited.
            ImGui::TableNextColumn();
            {
                const bool dimMark = isCurrent && !script->Targeted;
                bool shownChecked = script->Targeted || isCurrent;
                ImGui::BeginDisabled(!script->Enabled);
                if (dimMark) ImGui::PushStyleColor(ImGuiCol_CheckMark, OFS_Sashimi::Role().AccentDeep);
                if (ImGui::Checkbox("##edit", &shownChecked)) script->Targeted = !script->Targeted;
                if (dimMark) ImGui::PopStyleColor();
                ImGui::EndDisabled();
                trackTooltip(!script->Enabled ? "Hidden tracks aren't edited. Show it to edit it."
                    : dimMark ? "Edited because it's the track you're on. Check it to keep it edited when you switch to another track."
                    : (isCurrent ? "Edited, and stays edited when you switch to another track. Click to edit it only while you're on it."
                    : script->Targeted ? "Edited along with the track you're on: new points, moves, delete and paste happen here too. Click to stop editing it."
                    : "Not edited. Check it to edit it along with the track you're on."));
            }

            // Name: the current track is highlighted, and a click switches to
            // a track, showing it first if it was hidden.
            ImGui::TableNextColumn();
            {
                const auto label = ScriptTimeline::LaneLabel(scripts, i);
                if (!script->Enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(label.c_str(), isCurrent, ImGuiSelectableFlags_None, ImVec2(0.f, box)) && !isCurrent) {
                    switchTo = i;
                }
                if (!script->Enabled) ImGui::PopStyleColor();
                const std::string tip = script->Title() + "\n" + std::to_string(script->Actions().size()) + " points\n"
                    + (isCurrent ? "The track you're on. It's always edited, and Special functions and other one-track tools use it."
                        : "Click to switch to this track.");
                trackTooltip(tip.c_str());
            }

            // Remove from the project. The last track stays.
            ImGui::TableNextColumn();
            {
                const bool canRemove = scripts.size() > 1;
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##remove", ImVec2(box, box)) && canRemove) removeAt = i;
                const bool hovered = ImGui::IsItemHovered();
                if (hovered && canRemove) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                const uint32_t color = !canRemove ? IM_COL32(0x4A, 0x4A, 0x4A, 0xFF)
                    : (hovered ? OFS_Sashimi::Grey95 : OFS_Sashimi::Grey60);
                const ImVec2 center = pos + ImVec2(box * 0.5f, box * 0.5f);
                const float r = box * 0.18f;
                auto drawList = ImGui::GetWindowDrawList();
                drawList->AddLine(center + ImVec2(-r, -r), center + ImVec2(r, r), color, 1.5f);
                drawList->AddLine(center + ImVec2(-r, r), center + ImVec2(r, -r), color, 1.5f);
                trackTooltip(canRemove ? "Remove this track from the project. The file stays on disk."
                    : "The only track, so it stays.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);

    // Moved to the row the mouse is over, one row at a time as it crosses
    // each row's middle. Never above the main track.
    if (dragging > 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const float mouseY = ImGui::GetMousePos().y;
        int32_t target = dragging;
        if (target > 1 && mouseY < rowMiddles[target - 1]) target -= 1;
        else if (target + 1 < (int32_t)rowMiddles.size() && mouseY > rowMiddles[target + 1]) target += 1;
        if (target != dragging && LoadedProject->MoveFunscript(dragging, target)) {
            updateTitle();
        }
    }

    if (switchTo >= 0) {
        setTrackShown(switchTo, true);
        EV::Enqueue<ShouldChangeActiveScriptEvent>(switchTo);
    }
    if (removeAt >= 0) removeTrack(removeAt);
    ImGui::End();
}

// The scripts a paste goes into, and which part goes where. Parts copied from
// a lane that has since been removed are dropped. A clipboard from before the
// lanes were tracked, or with only one lane in it, pastes into the first
// edited lane: the current one, when it is being edited.
struct PasteTarget
{
    std::shared_ptr<Funscript> script;
    const FunscriptArray* actions;
};

static std::vector<PasteTarget> pasteTargetsFor(OpenFunscripter* app) noexcept
{
    std::vector<PasteTarget> targets;
    for (auto& track : app->CopiedTracks) {
        auto script = track.script.lock();
        if (!script || track.actions.empty()) continue;
        targets.push_back({ script, &track.actions });
    }
    if (targets.size() <= 1) {
        targets.clear();
        if (!app->FunscriptClipboard().empty()) {
            targets.push_back({ app->TargetedFunscripts().front(), &app->FunscriptClipboard() });
        }
    }
    return targets;
}

static UndoContextScripts undoContextFor(const std::vector<PasteTarget>& targets) noexcept
{
    UndoContextScripts scripts;
    scripts.reserve(targets.size());
    for (auto& target : targets) scripts.push_back(target.script);
    return scripts;
}

void OpenFunscripter::pasteSelection() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto targets = pasteTargetsFor(this);
    if (targets.empty()) return;
    undoSystem->Snapshot(StateType::PASTE_COPIED_ACTIONS, undoContextFor(targets));

    // Pasted relative to the playhead. The offset comes from the earliest
    // point across every part, so parts copied from different lanes keep
    // their timing against each other.
    // NOTE: assumes each part is ordered by time
    float firstTime = std::numeric_limits<float>::max();
    float lastTime = 0.f;
    for (auto& target : targets) {
        firstTime = Util::Min(firstTime, target.actions->front().atS);
        lastTime = Util::Max(lastTime, target.actions->back().atS);
    }
    const float currentTime = player->CurrentTime();
    const float offsetTime = currentTime - firstTime;

    for (auto& target : targets) {
        target.script->RemoveActionsInInterval(
            target.actions->front().atS + offsetTime - 0.0005f,
            target.actions->back().atS + offsetTime + 0.0005f);
        for (auto&& action : *target.actions) {
            target.script->AddAction(FunscriptAction(action.atS + offsetTime, action.pos));
        }
    }
    player->SetPositionExact(lastTime + offsetTime);
}

void OpenFunscripter::pasteSelectionExact() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto targets = pasteTargetsFor(this);
    if (targets.empty()) return;
    undoSystem->Snapshot(StateType::PASTE_COPIED_ACTIONS, undoContextFor(targets));

    // paste without altering timestamps
    for (auto& target : targets) {
        if (target.actions->size() >= 2) {
            target.script->RemoveActionsInInterval(target.actions->front().atS, target.actions->back().atS);
        }
        for (auto&& action : *target.actions) {
            target.script->AddAction(action);
        }
    }
}

void OpenFunscripter::equalizeSelection() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!ActiveFunscript()->HasSelection()) {
        undoSystem->Snapshot(StateType::EQUALIZE_ACTIONS, ActiveFunscript());
        // this is a small hack
        auto closest = ActiveFunscript()->GetClosestAction(player->CurrentTime());
        if (closest != nullptr) {
            auto behind = ActiveFunscript()->GetPreviousActionBehind(closest->atS);
            if (behind != nullptr) {
                auto front = ActiveFunscript()->GetNextActionAhead(closest->atS);
                if (front != nullptr) {
                    ActiveFunscript()->SelectAction(*behind);
                    ActiveFunscript()->SelectAction(*closest);
                    ActiveFunscript()->SelectAction(*front);
                    ActiveFunscript()->EqualizeSelection();
                    ActiveFunscript()->ClearSelection();
                }
            }
        }
    }
    else if (ActiveFunscript()->Selection().size() >= 3) {
        undoSystem->Snapshot(StateType::EQUALIZE_ACTIONS, ActiveFunscript());
        ActiveFunscript()->EqualizeSelection();
    }
}

void OpenFunscripter::invertSelection() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (!ActiveFunscript()->HasSelection()) {
        // same hack as above
        auto closest = ActiveFunscript()->GetClosestAction(player->CurrentTime());
        if (closest != nullptr) {
            undoSystem->Snapshot(StateType::INVERT_ACTIONS, ActiveFunscript());
            ActiveFunscript()->SelectAction(*closest);
            ActiveFunscript()->InvertSelection();
            ActiveFunscript()->ClearSelection();
        }
    }
    else {
        // Any selection. This asked for at least three points, a condition
        // that belongs to equalize, which needs a first and last point with
        // something between them to space out. Mirroring positions has no such
        // need, and with one or two points selected the key and the right
        // click menu's Invert did nothing at all, with nothing to say why.
        undoSystem->Snapshot(StateType::INVERT_ACTIONS, ActiveFunscript());
        ActiveFunscript()->InvertSelection();
    }
}

void OpenFunscripter::isolateAction() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto closest = ActiveFunscript()->GetClosestAction(player->CurrentTime());
    if (closest != nullptr) {
        undoSystem->Snapshot(StateType::ISOLATE_ACTION, ActiveFunscript());
        auto prev = ActiveFunscript()->GetPreviousActionBehind(closest->atS - 0.001f);
        auto next = ActiveFunscript()->GetNextActionAhead(closest->atS + 0.001f);
        if (prev != nullptr && next != nullptr) {
            auto tmp = *next; // removing prev will invalidate the pointer
            ActiveFunscript()->RemoveAction(*prev);
            ActiveFunscript()->RemoveAction(tmp);
        }
        else if (prev != nullptr) {
            ActiveFunscript()->RemoveAction(*prev);
        }
        else if (next != nullptr) {
            ActiveFunscript()->RemoveAction(*next);
        }
    }
}

void OpenFunscripter::repeatLastStroke() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto stroke = ActiveFunscript()->GetLastStroke(player->CurrentTime());
    if (stroke.size() > 1) {
        auto offsetTime = player->CurrentTime() - stroke.back().atS;
        undoSystem->Snapshot(StateType::REPEAT_STROKE, ActiveFunscript());
        auto action = ActiveFunscript()->GetActionAtTime(player->CurrentTime(), scripting->LogicalFrameTime());
        // if we are on top of an action we ignore the first action of the last stroke
        if (action != nullptr) {
            for (int i = stroke.size() - 2; i >= 0; i--) {
                auto action = stroke[i];
                action.atS += offsetTime;
                ActiveFunscript()->AddAction(action);
            }
        }
        else {
            for (int i = stroke.size() - 1; i >= 0; i--) {
                auto action = stroke[i];
                action.atS += offsetTime;
                ActiveFunscript()->AddAction(action);
            }
        }
        player->SetPositionExact(stroke.front().atS + offsetTime);
    }
}

void OpenFunscripter::saveActiveScriptAs()
{
    Util::SaveFileDialog(TR(SAVE),
        LoadedProject->MakePathAbsolute(ActiveFunscript()->RelativePath()),
        [this](auto& result) {
            if (result.files.size() > 0) {
                LoadedProject->ExportFunscript(result.files[0], LoadedProject->ActiveIdx());
                auto dir = Util::PathFromString(result.files[0]);
                dir.remove_filename();
                auto& ofsState = OpenFunscripterState::State(stateHandle);
                ofsState.lastPath = dir.u8string();
            }
        },
        { "Funscript", "*.funscript" });
}

void OpenFunscripter::ShowMainMenuBar() noexcept
{
#define BINDING_STRING(binding) keys->GetBindingString(binding)
    OFS_PROFILE(__FUNCTION__);
    ImColor alertCol = ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg);
    std::chrono::duration<float> saveDuration;
    bool unsavedEdits = LoadedProject->HasUnsavedEdits();
    if (player->VideoLoaded() && unsavedEdits) {
        saveDuration = std::chrono::system_clock::now() - ActiveFunscript()->EditTime();
        const float timeUnit = saveDuration.count() / 60.f;
        if (timeUnit >= 5.f) {
            alertCol = ImLerp(alertCol.Value, ImColor(IM_COL32(184, 33, 22, 255)).Value, std::max(std::sin(saveDuration.count()), 0.f));
        }
    }

    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, alertCol.Value);
    if (ImGui::BeginMainMenuBar()) {
        auto region = ImGui::GetContentRegionAvail();
        auto& ofsState = OpenFunscripterState::State(stateHandle);
        if (ImGui::BeginMenu(TR_ID("FILE", Tr::FILE))) {
            if (ImGui::MenuItem(TR(GENERIC_OPEN))) {
                Util::OpenFileDialog(
                    TR(GENERIC_OPEN), ofsState.lastPath,
                    [this](auto& result) {
                        if (result.files.size() > 0) {
                            auto& file = result.files[0];
                            openFile(file);
                        }
                    },
                    false);
            }
            if (ImGui::MenuItem("New script without video...")) {
                ShowNewScriptDialog = true;
            }
            OFS::Tooltip("Script to a length you pick, with no media to open.");
            if (LoadedProject->IsValid() && ImGui::MenuItem(TR(CLOSE_PROJECT), NULL, false, LoadedProject->IsValid())) {
                closeWithoutSavingDialog([]() {});
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(TR_ID("RECENT_FILES", Tr::RECENT_FILES))) {
                if (ofsState.recentFiles.empty()) {
                    ImGui::TextDisabled("%s", TR(NO_RECENT_FILES));
                }
                auto& recentFiles = ofsState.recentFiles;
                for (auto it = recentFiles.rbegin(); it != recentFiles.rend(); ++it) {
                    auto& recent = *it;
                    if (ImGui::MenuItem(recent.name.c_str())) {
                        if (!recent.projectPath.empty()) {
                            closeWithoutSavingDialog([this, clickedFile = recent.projectPath]() {
                                openFile(clickedFile);
                            });
                            break;
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(TR(CLEAR_RECENT_FILES))) {
                    ofsState.recentFiles.clear();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();

            if (ImGui::MenuItem(TR(SAVE_PROJECT), BINDING_STRING("save_project"), false, LoadedProject->IsValid())) {
                saveProject();
            }
            if (ImGui::BeginMenu(TR_ID("EXPORT_MENU", Tr::EXPORT_MENU), LoadedProject->IsValid())) {
                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", TR(QUICK_EXPORT)), BINDING_STRING("quick_export"))) {
                    quickExport();
                }
                OFS::Tooltip(TR(QUICK_EXPORT_TOOLTIP));
                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", TR(QUICK_EXPORT_2_0)), BINDING_STRING("quick_export_2_0"))) {
                    quickExport2();
                }
                OFS::Tooltip(TR(QUICK_EXPORT_2_0_TOOLTIP));
                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", TR(QUICK_EXPORT_1_1)))) {
                    LoadedProject->ExportFunscript11Quick();
                }
                OFS::Tooltip(TR(QUICK_EXPORT_1_1_TOOLTIP));
                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", TR(EXPORT_ACTIVE_SCRIPT)))) {
                    saveActiveScriptAs();
                }
                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", TR(EXPORT_ALL)))) {
                    if (LoadedFunscripts().size() == 1) {
                        auto savePath = Util::PathFromString(ofsState.lastPath) / (ActiveFunscript()->Title() + ".funscript");
                        Util::SaveFileDialog(TR(EXPORT_MENU), savePath.u8string(),
                            [this](auto& result) {
                                if (result.files.size() > 0) {
                                    LoadedProject->ExportFunscript(result.files[0], LoadedProject->ActiveIdx());
                                    std::filesystem::path dir = Util::PathFromString(result.files[0]);
                                    dir.remove_filename();
                                    auto& ofsState = OpenFunscripterState::State(stateHandle);
                                    ofsState.lastPath = dir.u8string();
                                }
                            },
                            { "Funscript", "*.funscript" });
                    }
                    else if (LoadedFunscripts().size() > 1) {
                        Util::OpenDirectoryDialog(TR(EXPORT_MENU), ofsState.lastPath,
                            [this](auto& result) {
                                if (result.files.size() > 0) {
                                    LoadedProject->ExportFunscripts(result.files[0]);
                                }
                            });
                    }
                }

                if (ImGui::MenuItem(FMT(ICON_SHARE " %s", "Export for device..."))) {
                    exportForDevice();
                }
                OFS::Tooltip("Writes a copy with nothing in it faster or closer together than "
                             "the script check's limits. The project keeps the script as written.");

                // Images of the video and of the script. These were in Edit,
                // among undo and the clipboard, though they change nothing and
                // produce a file, which is what this menu is for.
                ImGui::Separator();
                if (ImGui::MenuItem(TR(SAVE_FRAME_AS_IMAGE), BINDING_STRING("save_frame_as_image"))) {
                    auto screenshotDir = Util::Prefpath("screenshot");
                    player->SaveFrameToImage(screenshotDir);
                }
                if (ImGui::MenuItem(TR(OPEN_SCREENSHOT_DIR))) {
                    auto screenshotDir = Util::Prefpath("screenshot");
                    Util::CreateDirectories(screenshotDir);
                    Util::OpenFileExplorer(screenshotDir.c_str());
                }

                ImGui::Separator();
                // The size used to be two unlabelled number boxes with their
                // own plus and minus buttons, in a menu, with no hint of what
                // they sized.
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Heatmap size");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.f);
                if (ImGui::InputInt("##heatmapWidth", &ofsState.heatmapSettings.defaultWidth, 0, 0)) {
                    ofsState.heatmapSettings.defaultWidth = Util::Clamp(ofsState.heatmapSettings.defaultWidth, 32, 8192);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted("x");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.f);
                if (ImGui::InputInt("##heatmapHeight", &ofsState.heatmapSettings.defaultHeight, 0, 0)) {
                    ofsState.heatmapSettings.defaultHeight = Util::Clamp(ofsState.heatmapSettings.defaultHeight, 8, 8192);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("px");
                if (ImGui::MenuItem(TR(SAVE_HEATMAP))) {
                    std::string filename = ActiveFunscript()->Title() + "_Heatmap.png";
                    auto defaultPath = Util::PathFromString(ofsState.heatmapSettings.defaultPath);
                    Util::ConcatPathSafe(defaultPath, filename);
                    Util::SaveFileDialog(
                        TR(SAVE_HEATMAP), defaultPath.u8string(),
                        [this](auto& result) {
                            if (result.files.size() > 0) {
                                auto savePath = Util::PathFromString(result.files.front());
                                if (savePath.has_filename()) {
                                    auto& ofsState = OpenFunscripterState::State(stateHandle);
                                    saveHeatmap(result.files.front().c_str(), ofsState.heatmapSettings.defaultWidth, ofsState.heatmapSettings.defaultHeight, false);
                                    savePath.remove_filename();
                                    ofsState.heatmapSettings.defaultPath = savePath.u8string();
                                }
                            }
                        },
                        { "*.png" }, "PNG");
                }
                if (ImGui::MenuItem(TR(SAVE_HEATMAP_WITH_CHAPTERS))) {
                    std::string filename = ActiveFunscript()->Title() + "_Heatmap.png";
                    auto defaultPath = Util::PathFromString(ofsState.heatmapSettings.defaultPath);
                    Util::ConcatPathSafe(defaultPath, filename);
                    Util::SaveFileDialog(
                        TR(SAVE_HEATMAP), defaultPath.u8string(),
                        [this](auto& result) {
                            if (result.files.size() > 0) {
                                auto savePath = Util::PathFromString(result.files.front());
                                if (savePath.has_filename()) {
                                    auto& ofsState = OpenFunscripterState::State(stateHandle);
                                    saveHeatmap(result.files.front().c_str(), ofsState.heatmapSettings.defaultWidth, ofsState.heatmapSettings.defaultHeight, true);
                                    savePath.remove_filename();
                                    ofsState.heatmapSettings.defaultPath = savePath.u8string();
                                }
                            }
                        },
                        { "*.png" }, "PNG");
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            bool autoBackupTmp = Status & OFS_Status::OFS_AutoBackup;
            if (ImGui::MenuItem(autoBackupTmp && LoadedProject->IsValid() ? FMT(TR(AUTO_BACKUP_TIMER_FMT), AutoBackupIntervalSeconds - std::chrono::duration_cast<std::chrono::seconds>((std::chrono::steady_clock::now() - lastBackup)).count())
                                                                          : TR(AUTO_BACKUP),
                    NULL, &autoBackupTmp)) {
                Status = autoBackupTmp
                    ? Status | OFS_Status::OFS_AutoBackup
                    : Status ^ OFS_Status::OFS_AutoBackup;
            }
            if (ImGui::MenuItem(TR(OPEN_BACKUP_DIR))) {
                Util::OpenFileExplorer(Util::Prefpath("backup").c_str());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR_ID("PROJECT", Tr::PROJECT), LoadedProject->IsValid())) {
            if (ImGui::MenuItem(TR(CONFIGURE), NULL, &ShowProjectEditor)) {}
            ImGui::Separator();
            if (ImGui::MenuItem(TR(PICK_DIFFERENT_MEDIA))) {
                pickDifferentMedia();
            }
            if (ImGui::BeginMenu(TR(ADD_MENU), LoadedProject->IsValid())) {
                if (ImGui::BeginMenu(TR(ADD_SHORTCUTS))) {
                    for (auto axis : Funscript::AxisNames) {
                        if (ImGui::MenuItem(axis, nullptr, false, !trackLoaded(axisTrackPath(axis)))) {
                            addAxisTrack(axis);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(TR(ADD_NEW))) {
                    addNewTrackDialog();
                }
                if (ImGui::MenuItem(TR(ADD_EXISTING))) {
                    loadTracksDialog();
                }
                ImGui::EndMenu();
            }
            // The last track stays: there is always a script to edit.
            if (ImGui::BeginMenu(TR(REMOVE), LoadedFunscripts().size() > 1)) {
                for (int i = 0; i < (int)LoadedFunscripts().size(); i += 1) {
                    if (ImGui::MenuItem(LoadedFunscripts()[i]->Title().c_str())) {
                        removeTrack(i);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        // Undo and the clipboard. Frame and heatmap images moved to File >
        // Export, since they change nothing and make a file.
        if (ImGui::BeginMenu(TR_ID("EDIT", Tr::EDIT))) {
            if (ImGui::MenuItem(TR(UNDO), BINDING_STRING("undo"), false, !undoSystem->UndoEmpty())) {
                this->Undo();
            }
            if (ImGui::MenuItem(TR(REDO), BINDING_STRING("redo"), false, !undoSystem->RedoEmpty())) {
                this->Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TR(CUT), BINDING_STRING("cut"), false, targetsHaveSelection())) {
                cutSelection();
            }
            if (ImGui::MenuItem(TR(COPY), BINDING_STRING("copy"), false, targetsHaveSelection())) {
                copySelection();
            }
            if (ImGui::MenuItem(TR(PASTE), BINDING_STRING("paste"), false, CopiedSelection.size() > 0)) {
                pasteSelection();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR(SELECT))) {
            if (ImGui::MenuItem(TR(SELECT_ALL), BINDING_STRING("select_all"), false)) {
                selectAll();
            }
            if (ImGui::MenuItem(TR(DESELECT_ALL), BINDING_STRING("deselect_all"), false)) {
                clearSelection();
            }

            if (ImGui::BeginMenu(TR(SPECIAL))) {
                if (ImGui::MenuItem(TR(SELECT_ALL_LEFT), BINDING_STRING("select_all_left"), false)) {
                    selectTime(0, player->CurrentTime());
                }
                if (ImGui::MenuItem(TR(SELECT_ALL_RIGHT), BINDING_STRING("select_all_right"), false)) {
                    selectTime(player->CurrentTime(), player->Duration());
                }
                ImGui::Separator();
                static int32_t selectionPoint = -1;
                if (ImGui::MenuItem(TR(SET_SELECTION_START))) {
                    if (selectionPoint == -1) {
                        selectionPoint = player->CurrentTime();
                    }
                    else {
                        selectTime(player->CurrentTime(), selectionPoint);
                        selectionPoint = -1;
                    }
                }
                if (ImGui::MenuItem(TR(SET_SELECTION_END))) {
                    if (selectionPoint == -1) {
                        selectionPoint = player->CurrentTime();
                    }
                    else {
                        selectTime(selectionPoint, player->CurrentTime());
                        selectionPoint = -1;
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Greyed out below three selected points, the least these need to
            // tell a top from a bottom, as in the timeline's right click menu.
            // Always enabled, they did nothing when chosen with fewer.
            const bool enoughForStroke = ActiveFunscript()->SelectionSize() >= 3;
            if (ImGui::MenuItem(TR(TOP_POINTS_ONLY), BINDING_STRING("select_top_points"), false, enoughForStroke)) {
                selectTopPoints();
            }
            if (ImGui::MenuItem(TR(MID_POINTS_ONLY), BINDING_STRING("select_middle_points"), false, enoughForStroke)) {
                selectMiddlePoints();
            }
            if (ImGui::MenuItem(TR(BOTTOM_POINTS_ONLY), BINDING_STRING("select_bottom_points"), false, enoughForStroke)) {
                selectBottomPoints();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TR(EQUALIZE), BINDING_STRING("equalize_actions"), false)) {
                equalizeSelection();
            }
            if (ImGui::MenuItem(TR(INVERT), BINDING_STRING("invert_actions"), false)) {
                invertSelection();
            }
            if (ImGui::MenuItem(TR(ISOLATE), BINDING_STRING("isolate_action"))) {
                isolateAction();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR_ID("VIEW_MENU", Tr::VIEW_MENU))) {
#ifndef NDEBUG
            // this breaks the layout after restarting for some reason
            if (ImGui::MenuItem("Reset layout")) {
                setupDefaultLayout(true);
            }
            ImGui::Separator();
#endif
            if (ImGui::MenuItem("Toolbar", NULL, &ofsState.showToolbar)) {}
            OFS::Tooltip("The row of mode, grid, snap and simulator choices under the menu bar. "
                "Hidden, the Mode panel and the Simulator window show the same choices.");
            if (ImGui::MenuItem(TR(STATISTICS), NULL, &ofsState.showStatistics)) {}
            if (ImGui::MenuItem(TR(UNDO_REDO_HISTORY), NULL, &ofsState.showHistory)) {}
            if (ImGui::MenuItem(TR(SIMULATOR), NULL, &ofsState.showSimulator)) {}
            if (ImGui::MenuItem(TR(METADATA), NULL, &ShowMetadataEditor)) {}
            if (ImGui::MenuItem(TR(ACTION_EDITOR), NULL, &ofsState.showActionEditor)) {}
            if (ImGui::MenuItem(TR(SPECIAL_FUNCTIONS), NULL, &ofsState.showSpecialFunctions)) {}
            if (ImGui::MenuItem(TR(WEBSOCKET_API), NULL, &ofsState.showWsApi)) {}
            if (ImGui::MenuItem(TR(CHAPTERS), NULL, &ofsState.showChapterManager)) {}
            if (ImGui::MenuItem("Script check", NULL, &ofsState.showScriptCheck)) {}
            OFS::Tooltip("Lists the strokes a device cannot follow, and slows them down.");
            if (ImGui::MenuItem("Devices", NULL, &ofsState.showDevices)) {}
            OFS::Tooltip("Plays the script on a real device through Intiface Central.");
            if (ImGui::MenuItem("Tracks", NULL, &ofsState.showTracks)) {}
            OFS::Tooltip("Every track in the project: show or hide it, choose which are edited, add, load and remove them.");


            ImGui::Separator();

            if (ImGui::MenuItem(TR(DRAW_VIDEO), NULL, &ofsState.showVideo)) {}
            if (ImGui::MenuItem(TR(RESET_VIDEO_POS), NULL)) {
                playerWindow->ResetTranslationAndZoom();
            }

            auto videoModeToString = [](VideoMode mode) noexcept -> const char* {
                switch (mode) {
                    case VideoMode::Full: return TR(VIDEO_MODE_FULL);
                    case VideoMode::LeftPane: return TR(VIDEO_MODE_LEFT_PANE);
                    case VideoMode::RightPane: return TR(VIDEO_MODE_RIGHT_PANE);
                    case VideoMode::TopPane: return TR(VIDEO_MODE_TOP_PANE);
                    case VideoMode::BottomPane: return TR(VIDEO_MODE_BOTTOM_PANE);
                    case VideoMode::VrMode: return TR(VIDEO_MODE_VR);
                }
                return "";
            };

            auto& videoWindow = VideoPlayerWindowState::State(playerWindow->StateHandle());
            if (ImGui::BeginCombo(TR(VIDEO_MODE), videoModeToString(videoWindow.activeMode))) {
                auto& mode = videoWindow.activeMode;
                if (ImGui::Selectable(TR(VIDEO_MODE_FULL), mode == VideoMode::Full)) {
                    mode = VideoMode::Full;
                }
                if (ImGui::Selectable(TR(VIDEO_MODE_LEFT_PANE), mode == VideoMode::LeftPane)) {
                    mode = VideoMode::LeftPane;
                }
                if (ImGui::Selectable(TR(VIDEO_MODE_RIGHT_PANE), mode == VideoMode::RightPane)) {
                    mode = VideoMode::RightPane;
                }
                if (ImGui::Selectable(TR(VIDEO_MODE_TOP_PANE), mode == VideoMode::TopPane)) {
                    mode = VideoMode::TopPane;
                }
                if (ImGui::Selectable(TR(VIDEO_MODE_BOTTOM_PANE), mode == VideoMode::BottomPane)) {
                    mode = VideoMode::BottomPane;
                }
                if (ImGui::Selectable(TR(VIDEO_MODE_VR), mode == VideoMode::VrMode)) {
                    mode = VideoMode::VrMode;
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            if (ImGui::BeginMenu(TR(DEBUG))) {
                if (ImGui::MenuItem(TR(METRICS), NULL, &DebugMetrics)) {}
                if (ImGui::MenuItem(TR(LOG_OUTPUT), NULL, &ofsState.showDebugLog)) {}
#ifndef NDEBUG
                if (ImGui::MenuItem("ImGui Demo", NULL, &DebugDemo)) {}
#endif
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR(OPTIONS))) {
            if (ImGui::MenuItem(TR(KEYS))) {
                keys->ShowModal();
            }
            bool fullscreenTmp = Status & OFS_Status::OFS_Fullscreen;
            if (ImGui::MenuItem(TR(FULLSCREEN), BINDING_STRING("fullscreen_toggle"), &fullscreenTmp)) {
                SetFullscreen(fullscreenTmp);
                Status = fullscreenTmp
                    ? Status | OFS_Status::OFS_Fullscreen
                    : Status ^ OFS_Status::OFS_Fullscreen;
            }
            if (ImGui::MenuItem(TR(PREFERENCES), nullptr, &preferences->ShowWindow)) {}
            if (ImGui::BeginMenu(TR(CONTROLLER), ControllerInput::AnythingConnected())) {
                ImGui::TextColored(ImColor(IM_COL32(0, 255, 0, 255)), "%s", TR(CONTROLLER_CONNECTED));
                ImGui::TextUnformatted(ControllerInput::Controllers[0].GetName());
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR_ID("EXTENSIONS", Tr::EXTENSIONS_MENU))) {
            if (ImGui::IsWindowAppearing()) {
                extensions->UpdateExtensionList();
            }
            if (ImGui::MenuItem(TR(DEV_MODE), NULL, &OFS_LuaExtensions::DevMode)) {}
            OFS::Tooltip(TR(DEV_MODE_TOOLTIP));
            if (ImGui::MenuItem(TR(SHOW_LOGS), NULL, &OFS_LuaExtensions::ShowLogs)) {}
            if (ImGui::MenuItem(TR(EXTENSION_DIR))) {
                Util::OpenFileExplorer(Util::Prefpath(OFS_LuaExtensions::ExtensionDir));
            }
            ImGui::Separator();
            for (auto& ext : extensions->Extensions) {
                if (ImGui::BeginMenu(ext.NameId.c_str())) {
                    bool isActive = ext.Active;
                    if (ImGui::MenuItem(TR(ENABLED), NULL, &isActive)) {
                        ext.Toggle();
                        if (ext.HasError()) {
                            Util::MessageBoxAlert(TR(UNKNOWN_ERROR), ext.Error);
                        }
                    }
                    if (ImGui::MenuItem(Util::Format(TR(SHOW_WINDOW), ext.NameId.c_str()), NULL, &ext.WindowOpen, ext.Active)) {}
                    if (ImGui::MenuItem(Util::Format(TR(OPEN_DIRECTORY), ext.NameId.c_str()), NULL)) {
                        Util::OpenFileExplorer(ext.Directory);
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("?##About")) {
            ImGui::CloseCurrentPopup();
            ImGui::EndMenu();
        }
        if (ImGui::IsItemClicked()) ShowAbout = true;

        ImGui::Separator();
        ImGui::Spacing();
        if (ControllerInput::AnythingConnected()) {
            bool navmodeActive = ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad;
            ImGui::Text(ICON_GAMEPAD " " ICON_LONG_ARROW_RIGHT " %s", (navmodeActive) ? TR(NAVIGATION) : TR(SCRIPTING));
        }
        ImGui::Spacing();
        if (IdleMode) {
            ImGui::TextUnformatted(ICON_LEAF);
        }
        if (player->VideoLoaded() && unsavedEdits) {
            const float timeUnit = saveDuration.count() / 60.f;
            ImGui::SameLine(region.x - ImGui::GetFontSize() * 13.5f);
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_Text], TR(UNSAVED_CHANGES_FMT), (int)(timeUnit));
        }
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleColor(1);
#undef BINDING_STRING
}

void OpenFunscripter::SetFullscreen(bool fullscreen)
{
    static SDL_Rect restoreRect = { 0, 0, 1280, 720 };
    if (fullscreen) {
        SDL_GetWindowPosition(window, &restoreRect.x, &restoreRect.y);
        SDL_GetWindowSize(window, &restoreRect.w, &restoreRect.h);

        SDL_SetWindowResizable(window, SDL_FALSE);
        SDL_SetWindowBordered(window, SDL_FALSE);
        SDL_SetWindowPosition(window, 0, 0);
        int display = SDL_GetWindowDisplayIndex(window);
        SDL_Rect bounds;
        SDL_GetDisplayBounds(display, &bounds);

#ifdef WIN32
        // +1 pixel to the height because windows is dumb
        // when the window has the exact size as the screen windows will do some
        // bs that causes the screen to flash black when focusing a different window,file picker, etc.
        SDL_SetWindowSize(window, bounds.w, bounds.h + 1);
#else
        SDL_SetWindowSize(window, bounds.w, bounds.h);
#endif
    }
    else {
        SDL_SetWindowResizable(window, SDL_TRUE);
        SDL_SetWindowBordered(window, SDL_TRUE);
        SDL_SetWindowPosition(window, restoreRect.x, restoreRect.y);
        SDL_SetWindowSize(window, restoreRect.w, restoreRect.h);
    }
}

void OpenFunscripter::CreateDockspace() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    constexpr bool opt_fullscreen_persistant = true;
    constexpr bool opt_fullscreen = opt_fullscreen_persistant;
    constexpr ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None | ImGuiDockNodeFlags_PassthruCentralNode;

    // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
    // because it would be confusing to have two docking targets within each others.
    ImGuiWindowFlags window_flags = /*ImGuiWindowFlags_MenuBar |*/ ImGuiWindowFlags_NoDocking;
    if constexpr (opt_fullscreen) {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }

    // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
    // and handle the pass-thru hole, so we ask Begin() to not render a background.
    if constexpr ((bool)(dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)) window_flags |= ImGuiWindowFlags_NoBackground;

    // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
    // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
    // all active windows docked into it will lose their parent and become undocked.
    // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
    // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpace", 0, window_flags);
    ImGui::PopStyleVar();

    if constexpr (opt_fullscreen) ImGui::PopStyleVar(2);

    // DockSpace
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGui::DockSpace(MainDockspaceID, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    ShowMainMenuBar();

    ImGui::End();

    ShowToolbar();
}

// The choices made most often while scripting: how a click places a point,
// which grid is under the timeline, whether points snap to it, and what the
// simulator shows. Each is the same control its panel uses, so the two can
// never disagree, and the panels keep the settings that go with each choice.
//
// An ordinary window, so it docks, floats and closes like every other panel.
// It starts docked across the top with its tab hidden, where it reads as a
// bar. It is laid out as one row when wide enough, and as a column of groups
// when docked somewhere narrow such as the right side.
void OpenFunscripter::ShowToolbar() noexcept
{
    auto& ofsState = OpenFunscripterState::State(stateHandle);
    if (!ofsState.showToolbar) return;
    OFS_PROFILE(__FUNCTION__);

    const auto& style = ImGui::GetStyle();

    // A profile saved before the toolbar was a window has no place for it.
    // It goes where it used to be, across the top of the dockspace, the same
    // place dragging it to the top edge would put it.
    const bool firstUse = ImGui::FindWindowSettings(ImHashStr(ToolbarWindowId)) == nullptr
        && ImGui::FindWindowByName(ToolbarWindowId) == nullptr;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    if (toolbarWide) {
        flags |= ImGuiWindowFlags_NoScrollbar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, toolbarVerticalPadding()));
    }
    const bool visible = ImGui::Begin(ToolbarWindowId, &ofsState.showToolbar, flags);
    if (toolbarWide) ImGui::PopStyleVar();

    if (firstUse) {
        auto* root = ImGui::DockBuilderGetNode(MainDockspaceID);
        if (root != nullptr && root->HostWindow != nullptr && root->Size.y > 0.f) {
            ImGui::DockContextQueueDock(ImGui::GetCurrentContext(), root->HostWindow, root,
                ImGui::GetCurrentWindow(), ImGuiDir_Up, toolbarRowHeight() / root->Size.y, true);
            toolbarHideTabBar = true;
        }
    }
    else if (toolbarHideTabBar && ImGui::IsWindowDocked()) {
        // Docking clears the flag, so it is set once the dock has happened.
        if (auto* node = ImGui::GetWindowDockNode()) {
            node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
        }
        toolbarHideTabBar = false;
    }

    // Docked alone in a bar across the top or bottom, the bar is sized to one
    // row, plus the tab when it is showing. Without this, showing the tab to
    // move the toolbar pushed the row out of sight.
    if (auto* node = ImGui::GetWindowDockNode(); node != nullptr && node->Windows.Size == 1
        && node->ParentNode != nullptr && node->ParentNode->SplitAxis == ImGuiAxis_Y && toolbarWide) {
        const float want = toolbarRowHeight() + (node->IsHiddenTabBar() ? 0.f : ImGui::GetFrameHeight());
        if (node->SizeRef.y != want) node->SizeRef.y = want;
    }

    if (visible) {
        const float fontSize = ImGui::GetFontSize();
        const bool wide = ImGui::GetContentRegionAvail().x >= fontSize * 48.f;
        toolbarWide = wide;

        // A short dimmed name in front of each group, so the row reads
        // without hovering. On their own the bars were mode names, grid
        // names and bare numbers that said nothing about what they set.
        // Down a column it sits on its own line above its group instead.
        auto group = [wide](const char* label, const char* tip) noexcept {
            if (wide) {
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
            }
            else {
                ImGui::Spacing();
            }
            ImGui::TextDisabled("%s", label);
            OFS::Tooltip(tip);
            if (wide) ImGui::SameLine();
        };

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV
            | ImGuiTableFlags_SizingStretchProp
            | ImGuiTableFlags_NoPadOuterX;
        const bool table = wide && ImGui::BeginTable("##ToolbarTable", 5, tableFlags);
        if (table) {
            // Weighted by how much each control has to say, so the four mode
            // names get the room they need before the simulator's two.
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 4.f);
            ImGui::TableSetupColumn("Grid", ImGuiTableColumnFlags_WidthStretch, 2.8f);
            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.f);
            ImGui::TableSetupColumn("Snap", ImGuiTableColumnFlags_WidthStretch, 3.4f);
            ImGui::TableSetupColumn("Simulator", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableNextRow();
        }

        if (table || !wide) {
            group("Mode", "How a click on the timeline places a point.");
            scripting->DrawModeSelector("##ToolbarMode");

            group("Grid", "What the lines on the timeline mark, and what points snap to.");
            scripting->DrawOverlaySelector("##ToolbarGrid");

            group("Note", "Spacing of the tempo grid's lines, as a note length.");
            if (!wide) ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            TempoOverlay::DrawNoteDivisionSelector("##ToolbarNote",
                scripting->ActiveOverlay() == ScriptingOverlayModes::TEMPO);

            {
                group("Snap", "Where points placed or dragged with the mouse land.");
                if (!wide) ImGui::SameLine();
                auto& overlayState = BaseOverlay::State();
                const bool hasGrid = scripting->Overlay() != nullptr && scripting->Overlay()->HasSnapGrid();
                const bool gridSnapOn = overlayState.SnapToGrid && hasGrid;

                // One label whether on or off, lit when on: a label that changed
                // with the state changed the button's width, and at toolbar
                // widths got cut off.
                ImGui::BeginDisabled(!hasGrid);
                if (OFS::ToggleButton("Grid###ToolbarSnap", gridSnapOn)) {
                    overlayState.SnapToGrid = !overlayState.SnapToGrid;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s", hasGrid
                        ? "Snap times to the grid lines. Hold Alt to place one point off the grid."
                        : "There is no grid to snap to. Pick Frame or Tempo.");
                }

                ImGui::SameLine();
                ImGui::TextDisabled("Round position");
                OFS::Tooltip("Rounds the position, up and down, of points placed or dragged with the mouse.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.f);
                DrawPositionRoundingSelector("##ToolbarPosStep");
            }

            group("Simulator", "What the simulator shows: a 2D bar, or a 3D model driven by every loaded axis.");
            simulator.DrawModeSelector("##ToolbarSimulator");
        }
        if (table) ImGui::EndTable();

        // Docked across the top its tab is hidden, so the way to move or put
        // it away is offered where it is, not only in the View menu.
        // Asked for here, since inside the popup the current window is the popup.
        ImGuiDockNode* toolbarNode = ImGui::GetWindowDockNode();
        if (ImGui::BeginPopupContextWindow("##ToolbarMenu")) {
            if (auto* node = toolbarNode) {
                const bool tabHidden = node->IsHiddenTabBar();
                if (ImGui::MenuItem(tabHidden ? "Show tab, to move it" : "Hide tab")) {
                    node->WantHiddenTabBarToggle = true;
                }
                if (tabHidden) {
                    OFS::Tooltip("Shows the toolbar's tab. Drag the tab to dock the toolbar somewhere else, "
                        "such as the right side, or out on its own.");
                }
            }
            if (ImGui::MenuItem("Hide toolbar")) ofsState.showToolbar = false;
            ImGui::TextDisabled("View > Toolbar brings it back. With it hidden,\nthe Mode panel shows the mode and grid choices.");
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

// Asks the one thing a script without a video cannot be given by a file: how
// long it runs. Everything else about the project follows from the save dialog
// this hands off to.
void OpenFunscripter::ShowNewScriptWindow(bool* open) noexcept
{
    static constexpr const char* WindowId = "New script without video";
    if (*open) {
        ImGui::OpenPopup(WindowId);
    }
    if (!ImGui::BeginPopupModal(WindowId, open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    OFS_PROFILE(__FUNCTION__);

    auto& ofsState = OpenFunscripterState::State(stateHandle);

    ImGui::TextDisabled("%s", "A timeline of a fixed length with nothing playing behind it.");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Length");
    ImGui::SameLine();
    OFS::InputDuration("##NewScriptDuration", &ofsState.newScriptDurationSeconds,
        (int32_t)OFS_Project::MinStandaloneDuration, (int32_t)OFS_Project::MaxStandaloneDuration);
    OFS::Tooltip("Can be changed later under Project > Configure.");

    ImGui::Spacing();
    if (ImGui::Button("Choose location...", ImVec2(-1.f, 0.f))) {
        // Closed first: the file dialog is the next thing the user deals with,
        // and the modal behind it would only be in the way.
        *open = false;
        ImGui::CloseCurrentPopup();
        createStandaloneProject((float)ofsState.newScriptDurationSeconds);
    }
    if (ImGui::Button("Cancel", ImVec2(-1.f, 0.f))) {
        *open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void OpenFunscripter::ShowAboutWindow(bool* open) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);
    ImGui::Begin(TR(ABOUT), open, ImGuiWindowFlags_None | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("OFS-SE " OFS_LATEST_GIT_TAG);
    ImGui::TextUnformatted("OpenFunscripter - Sashimi Edition");
    ImGui::Text("%s: %s", TR(GIT_COMMIT), OFS_LATEST_GIT_HASH);

    ImGui::End();
}

bool OpenFunscripter::ToolbarVisible() noexcept
{
    return OpenFunscripterState::State(stateHandle).showToolbar;
}

void OpenFunscripter::ShowChapters() noexcept
{
    OpenFunscripterState::State(stateHandle).showChapterManager = true;
    chapterMgr->FocusNextFrame();
}

// One label and value per row, label dimmed, so a column of numbers can be
// read down without the names in the way.
static bool beginStatisticsTable(const char* id) noexcept
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    // Labels take the width they need and values the rest. Split by ratio, a
    // narrow panel cut the longer labels short.
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void statisticsRow(const char* label, const char* fmt, ...) noexcept
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

static void statisticsHeading(const char* heading) noexcept
{
    ImGui::Spacing();
    OFS::SeparatorText(heading);
}

void OpenFunscripter::ShowStatisticsWindow(bool* open) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);
    ImGui::Begin(TR_ID(StatisticsWindowId, Tr::STATISTICS), open, ImGuiWindowFlags_None);

    auto script = ActiveFunscript();
    const auto& actions = script->Actions();
    const float currentTime = player->CurrentTime();

    // The stroke under the playhead.
    const FunscriptAction* front = script->GetActionAtTime(currentTime, 0.001f);
    const FunscriptAction* behind = nullptr;
    if (front != nullptr) {
        behind = script->GetPreviousActionBehind(front->atS);
    }
    else {
        behind = script->GetPreviousActionBehind(currentTime);
        front = script->GetNextActionAhead(currentTime);
    }

    statisticsHeading("At the playhead");
    if (actions.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("This script has no points yet. Once it does, the stroke under "
                            "the playhead is measured here.");
        ImGui::PopTextWrapPos();
    }
    else if (behind == nullptr) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("The playhead is before the first point. Move past it to measure "
                            "the stroke it is in.");
        ImGui::PopTextWrapPos();
    }
    else if (beginStatisticsTable("##playheadStats")) {
        // Short enough for the value column of a narrow panel; the longer
        // "since the last point" ran out of the panel.
        statisticsRow("Since last point", "%.0f ms", ((double)currentTime - behind->atS) * 1000.0);
        if (front != nullptr) {
            const float duration = front->atS - behind->atS;
            const int32_t length = front->pos - behind->pos;
            statisticsRow(TR(SPEED), "%.0f units/s", std::abs(length) / duration);
            statisticsRow(TR(DURATION), "%.0f ms", (double)duration * 1000.0);
            statisticsRow("Stroke", "%d " ICON_LONG_ARROW_RIGHT " %d  (%d %s)",
                behind->pos, front->pos, std::abs(length),
                length >= 0 ? ICON_LONG_ARROW_UP : ICON_LONG_ARROW_DOWN);
        }
        else {
            statisticsRow("Stroke", "past the last point");
        }
        ImGui::EndTable();
    }

    // The selection, measured between its own points. A selection that skips
    // points in between is still measured point to point, which is what
    // editing it will act on.
    const auto& selection = script->Selection();
    if (selection.size() >= 2) {
        const float span = selection.back().atS - selection.front().atS;
        float travelled = 0.f;
        float topSpeed = 0.f;
        for (size_t i = 1; i < selection.size(); i += 1) {
            const float dt = selection[i].atS - selection[i - 1].atS;
            const float dp = (float)std::abs(selection[i].pos - selection[i - 1].pos);
            travelled += dp;
            if (dt > 0.f) topSpeed = std::max(topSpeed, dp / dt);
        }

        statisticsHeading("Selection");
        if (beginStatisticsTable("##selectionStats")) {
            statisticsRow("Points", "%u", (uint32_t)selection.size());
            statisticsRow("Span", "%.2f s", span);
            statisticsRow("Average speed", "%.0f units/s", span > 0.f ? travelled / span : 0.f);
            statisticsRow("Top speed", "%.0f units/s", topSpeed);
            ImGui::EndTable();
        }
    }
    else if (selection.size() == 1) {
        statisticsHeading("Selection");
        ImGui::TextDisabled("One point, at %.3f s and %d.", selection.front().atS, selection.front().pos);
    }

    statisticsHeading("Script");
    if (beginStatisticsTable("##scriptStats")) {
        statisticsRow("Points", "%u", (uint32_t)actions.size());
        if (actions.size() >= 2) {
            statisticsRow("Span", "%.1f s", actions.back().atS - actions.front().atS);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void OpenFunscripter::ControllerAxisPlaybackSpeed(const OFS_SDL_Event* ev) noexcept
{
    static Uint8 lastAxis = 0;
    OFS_PROFILE(__FUNCTION__);
    auto& caxis = ev->sdl.caxis;
    if ((Status & OFS_Status::OFS_GamepadSetPlaybackSpeed) && caxis.axis == lastAxis && caxis.value <= 0) {
        Status &= ~(OFS_Status::OFS_GamepadSetPlaybackSpeed);
        return;
    }

    if (caxis.value < 0) {
        return;
    }
    if (Status & OFS_Status::OFS_GamepadSetPlaybackSpeed) {
        return;
    }
    auto app = OpenFunscripter::ptr;
    if (caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
        float speed = 1.f - (caxis.value / (float)std::numeric_limits<int16_t>::max());
        app->player->SetSpeed(speed);
        lastAxis = caxis.axis;
    }
    else if (caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        float speed = 1.f + (caxis.value / (float)std::numeric_limits<int16_t>::max());
        app->player->SetSpeed(speed);
        lastAxis = caxis.axis;
    }
}

void OpenFunscripter::ScriptTimelineDoubleClick(const ShouldSetTimeEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    player->SetPositionExact(ev->newTime);
}

void OpenFunscripter::ScriptTimelineSelectTime(const FunscriptShouldSelectTimeEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (auto script = ev->script.lock()) {
        script->SelectTime(ev->startTime, ev->endTime, ev->clearSelection);
        // Dragging across an edited lane selects the same span in every
        // edited lane, which is what makes copying a passage from several
        // channels a single drag rather than one per lane.
        if (IsEdited(script)) {
            for (auto& target : TargetedFunscripts()) {
                if (target != script) target->SelectTime(ev->startTime, ev->endTime, ev->clearSelection);
            }
        }
    }
}

void OpenFunscripter::ScriptTimelineActiveScriptChanged(const ShouldChangeActiveScriptEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    UpdateNewActiveScript(ev->activeIdx);
}

void OpenFunscripter::selectTopPoints() noexcept
{
    undoSystem->Snapshot(StateType::TOP_POINTS_ONLY, ActiveFunscript());
    ActiveFunscript()->SelectTopActions();
}

void OpenFunscripter::selectMiddlePoints() noexcept
{
    undoSystem->Snapshot(StateType::MID_POINTS_ONLY, ActiveFunscript());
    ActiveFunscript()->SelectMidActions();
}

void OpenFunscripter::selectBottomPoints() noexcept
{
    undoSystem->Snapshot(StateType::BOTTOM_POINTS_ONLY, ActiveFunscript());
    ActiveFunscript()->SelectBottomActions();
}

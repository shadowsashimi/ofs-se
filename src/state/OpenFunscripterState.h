#pragma once

#include <string>
#include <vector>

#include "OFS_Reflection.h"
#include "OFS_StateHandle.h"

struct RecentFile 
{
	std::string name;
	std::string projectPath;
};

REFL_TYPE(RecentFile)
	REFL_FIELD(name)
	REFL_FIELD(projectPath)
REFL_END

// Which panels are open, as a saved layout keeps them: the window layout
// alone says where each panel goes, not whether it is showing. Defaults as
// a fresh profile has them.
struct LayoutPanels
{
    bool showVideo = true;
    bool showActionEditor = false;
    bool showStatistics = true;
    bool showHistory = true;
    bool showSimulator = true;
    bool showSpecialFunctions = true;
    bool showWsApi = false;
    bool showChapterManager = true;
    bool showScriptCheck = true;
    bool showDevices = false;
    bool showToolbar = true;
    bool showTracks = true;
};

REFL_TYPE(LayoutPanels)
	REFL_FIELD(showVideo)
	REFL_FIELD(showActionEditor)
	REFL_FIELD(showStatistics)
	REFL_FIELD(showHistory)
	REFL_FIELD(showSimulator)
	REFL_FIELD(showSpecialFunctions)
	REFL_FIELD(showWsApi)
	REFL_FIELD(showChapterManager)
	REFL_FIELD(showScriptCheck)
	REFL_FIELD(showDevices)
	REFL_FIELD(showToolbar)
	REFL_FIELD(showTracks)
REFL_END

// A layout saved by name from View > Layouts: where every panel goes, as
// ImGui writes it to its ini, and which panels are open.
struct SavedLayout
{
    std::string name;
    std::string ini;
    LayoutPanels panels;
};

REFL_TYPE(SavedLayout)
	REFL_FIELD(name)
	REFL_FIELD(ini)
	REFL_FIELD(panels)
REFL_END

struct OpenFunscripterState 
{
	static constexpr auto StateName = "OpenFunscripter";

	std::vector<RecentFile> recentFiles;
    std::string lastPath;

	struct HeatmapSettings {
		int32_t defaultWidth = 2000;
		int32_t defaultHeight = 50;
		std::string defaultPath = "./";
	} heatmapSettings;

    // The length offered next time a script is started without a video. Kept
    // because someone scripting for a fixed length toy pattern uses the same
    // one over and over.
    int32_t newScriptDurationSeconds = 10 * 60;

    bool showDebugLog = false;
    bool showVideo = true;

    bool showActionEditor = false;
    bool showStatistics = true;
    bool alwaysShowBookmarkLabels = false;
    bool showHistory = true;
    bool showSimulator = true;
    bool showSpecialFunctions = true;
    bool showWsApi = false;
    bool showChapterManager = true;
    bool showScriptCheck = true;
    bool showDevices = false;
    bool showToolbar = true;
    bool showTracks = true;

    std::vector<SavedLayout> savedLayouts;

    inline static OpenFunscripterState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<OpenFunscripterState>(stateHandle).Get();
    }

    void addRecentFile(const RecentFile& recentFile) noexcept;

    static void RegisterAll() noexcept;
};

REFL_TYPE(OpenFunscripterState::HeatmapSettings)
	REFL_FIELD(defaultWidth)
	REFL_FIELD(defaultHeight)
	REFL_FIELD(defaultPath)
REFL_END

REFL_TYPE(OpenFunscripterState)
    REFL_FIELD(recentFiles)
    REFL_FIELD(lastPath)
    REFL_FIELD(newScriptDurationSeconds)
    REFL_FIELD(showDebugLog)
    REFL_FIELD(showVideo)
    REFL_FIELD(alwaysShowBookmarkLabels)
    REFL_FIELD(showHistory)
    REFL_FIELD(showSimulator)
    REFL_FIELD(showSpecialFunctions)
    REFL_FIELD(showWsApi)
    REFL_FIELD(showChapterManager)
    REFL_FIELD(showScriptCheck)
    REFL_FIELD(showDevices)
    REFL_FIELD(showToolbar)
    REFL_FIELD(showTracks)
    REFL_FIELD(showActionEditor)
    REFL_FIELD(showStatistics)
    REFL_FIELD(savedLayouts)
REFL_END
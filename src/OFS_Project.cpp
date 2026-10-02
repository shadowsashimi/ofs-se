#include "OFS_Project.h"
#include "OpenFunscripter.h"
#include "OFS_Localization.h"
#include "OFS_ImGui.h"
#include "OFS_DynamicFontAtlas.h"
#include "OFS_BlockingTask.h"
#include "OFS_EventSystem.h"

#include "subprocess.h"

#include <algorithm>
#include <cctype>

static std::array<const char*, 6> VideoExtensions{
    ".mp4",
    ".mkv",
    ".webm",
    ".wmv",
    ".avi",
    ".m4v",
};

// Anything mpv and ffmpeg can both read will do, since one plays it and the
// other makes its waveform. These are the audio containers people are likely
// to have a song in.
static std::array<const char*, 12> AudioExtensions{
    ".mp3",
    ".m4a",
    ".m4b",
    ".aac",
    ".ogg",
    ".oga",
    ".opus",
    ".flac",
    ".wav",
    ".wma",
    ".aif",
    ".aiff",
};

inline bool static HasMediaExtension(const std::string& pathStr) noexcept
{
    auto path = Util::PathFromString(pathStr);
    auto ext = path.extension().u8string();
    // Files straight off a phone or a camera are often named in capitals.
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](char c) noexcept { return (char)std::tolower((unsigned char)c); });

    auto matches = [&ext](const char* validExt) noexcept {
        return ext == validExt;
    };
    return std::any_of(VideoExtensions.begin(), VideoExtensions.end(), matches)
        || std::any_of(AudioExtensions.begin(), AudioExtensions.end(), matches);
}

inline bool FindMedia(const std::string& pathStr, std::string* outMedia) noexcept
{
    auto path = Util::PathFromString(pathStr);
    auto pathDir = path.parent_path();

    auto filename = path.stem().u8string();

    std::error_code ec;
    std::filesystem::directory_iterator dirIt(pathDir, ec);
    for (auto& entry : dirIt) {
        auto entryName = entry.path().stem().u8string();
        if (entryName == filename) {
            auto entryPathStr = entry.path().u8string();

            if (HasMediaExtension(entryPathStr)) {
                *outMedia = entryPathStr;
                return true;
            }
        }
    }

    return false;
}

OFS_Project::OFS_Project() noexcept
{
    stateHandle = OFS_ProjectState<ProjectState>::Register(ProjectState::StateName);
    Funscripts.emplace_back(std::move(std::make_shared<Funscript>()));
}

OFS_Project::~OFS_Project() noexcept
{
}

void OFS_Project::loadNecessaryGlyphs() noexcept
{
    // This should be called after loading or importing.
    auto& projectState = State();
    auto& metadata = projectState.metadata;
    OFS_DynFontAtlas::AddText(metadata.type);
    OFS_DynFontAtlas::AddText(metadata.title);
    OFS_DynFontAtlas::AddText(metadata.creator);
    OFS_DynFontAtlas::AddText(metadata.script_url);
    OFS_DynFontAtlas::AddText(metadata.video_url);
    for (auto& tag : metadata.tags) OFS_DynFontAtlas::AddText(tag);
    for (auto& performer : metadata.performers) OFS_DynFontAtlas::AddText(performer);
    OFS_DynFontAtlas::AddText(metadata.description);
    OFS_DynFontAtlas::AddText(metadata.license);
    OFS_DynFontAtlas::AddText(metadata.notes);
    for (auto& script : Funscripts) OFS_DynFontAtlas::AddText(script->Title().c_str());
    OFS_DynFontAtlas::AddText(lastPath);
}

bool OFS_Project::Load(const std::string& path) noexcept
{
    FUN_ASSERT(!valid, "Can't import if project is already loaded.");
#if 1
    std::vector<uint8_t> projectBin;
    if (Util::ReadFile(path.c_str(), projectBin) > 0) {
        bool succ;
        auto projectState = Util::ParseCBOR(projectBin, &succ);
        if (succ) {
            valid = OFS_StateManager::Get()->DeserializeProjectAll(projectState, true);
        }
    }
#else
    std::string projectJson = Util::ReadFileString(path.c_str());
    if (!projectJson.empty()) {
        bool succ;
        auto json = Util::ParseJson(projectJson, &succ);
        if (succ) {
            valid = OFS_StateManager::Get()->DeserializeProjectAll(json, false);
        }
        else {
            valid = false;
            addError("Failed to parse project.\nIt likely is an old project file not supported in " OFS_LATEST_GIT_TAG);
        }
    }
#endif

    if (valid) {
        auto& projectState = State();
        OFS_Binary::Deserialize(projectState.binaryFunscriptData, *this);
        lastPath = path;
        loadNecessaryGlyphs();
    }

    return valid;
}

bool OFS_Project::ImportFromFunscript(const std::string& file) noexcept
{
    FUN_ASSERT(!valid, "Can't import if project is already loaded.");

    auto& projectState = State();
    auto basePath = Util::PathFromString(file);
    lastPath = basePath.replace_extension(OFS_Project::Extension).u8string();

    if (Util::FileExists(file)) {
        Funscripts.clear();
        if (!AddFunscript(file)) {
            addError("Failed to load funscript.");
            return valid;
        }
        loadMultiAxis(file);

        std::string absMediaPath;
        if (FindMedia(file, &absMediaPath)) {
            projectState.relativeMediaPath = MakePathRelative(absMediaPath);
            valid = true;
            loadNecessaryGlyphs();
        }
        else {
            addError("Failed to find media for funscript.");
            return valid;
        }
    }

    return valid;
}

bool OFS_Project::ImportFromMedia(const std::string& file) noexcept
{
    FUN_ASSERT(!valid, "Can't import if project is already loaded.");

    if (!HasMediaExtension(file)) {
        // Unsupported media.
        addError("Unsupported media file extension.");
        return false;
    }

    auto& projectState = State();
    auto basePath = Util::PathFromString(file);
    lastPath = basePath.replace_extension(OFS_Project::Extension).u8string();

    basePath = Util::PathFromString(file);
    if (Util::FileExists(file)) {
        projectState.relativeMediaPath = MakePathRelative(file);
        auto funscriptPath = basePath;
        auto funscriptPathStr = funscriptPath.replace_extension(".funscript").u8string();

        Funscripts.clear();
        AddFunscript(funscriptPathStr);
        loadMultiAxis(funscriptPathStr);
        valid = true;
        loadNecessaryGlyphs();
    }

    return valid;
}

bool OFS_Project::CreateStandalone(const std::string& file, float durationSeconds) noexcept
{
    FUN_ASSERT(!valid, "Can't import if project is already loaded.");

    auto basePath = Util::PathFromString(file);
    lastPath = basePath.replace_extension(OFS_Project::Extension).u8string();

    auto& projectState = State();
    // An empty media path is what marks a project standalone everywhere else,
    // so it is cleared rather than assumed to be empty already.
    projectState.relativeMediaPath.clear();
    projectState.standaloneDuration = Util::Clamp(durationSeconds, MinStandaloneDuration, MaxStandaloneDuration);
    projectState.metadata.duration = projectState.standaloneDuration;

    auto scriptPath = Util::PathFromString(file).replace_extension(".funscript").u8string();
    Funscripts.clear();
    // Picks up a funscript already sitting under that name and otherwise starts
    // an empty one there, which is what opening a video does.
    AddFunscript(scriptPath);
    loadMultiAxis(scriptPath);
    valid = true;
    loadNecessaryGlyphs();

    return valid;
}

bool OFS_Project::AddFunscript(const std::string& path) noexcept
{
    bool loadedScript = false;

    bool succ = false;
    auto jsonText = Util::ReadFileString(path.c_str());
    auto json = Util::ParseJson(jsonText, &succ);

	bool isFirstFunscript = Funscripts.size() == 0;

	if (succ && json.is_object()) {
		// Support Funscript 2.0 (channels) and 1.1 (axes)
		bool hasChannels = json.contains("channels") && json["channels"].is_object();
		bool hasAxes = json.contains("axes") && json["axes"].is_array();
		if (hasChannels || hasAxes) {
			// Load root/top-level actions if available (treat as main channel)
			{
				auto script = std::make_shared<Funscript>();
				auto metadata = Funscript::Metadata();
				if (script->Deserialize(json, &metadata, isFirstFunscript)) {
					script = Funscripts.emplace_back(std::move(script));
					script->UpdateRelativePath(MakePathRelative(path));
					if (isFirstFunscript) {
						auto& projectState = State();
						projectState.metadata = metadata;
						isFirstFunscript = false;
					}
					loadedScript = true;
				}
			}
			// Load each named channel (2.0)
			if (hasChannels) {
				for (auto it = json["channels"].begin(); it != json["channels"].end(); ++it) {
					const std::string channelName = it.key();
					const nlohmann::json& channelObj = it.value();
					if (!channelObj.is_object()) continue;
					if (!channelObj.contains("actions") || !channelObj["actions"].is_array()) continue;
					auto scriptCh = std::make_shared<Funscript>();
					if (scriptCh->Deserialize(channelObj, nullptr, false)) {
						scriptCh = Funscripts.emplace_back(std::move(scriptCh));
						// Synthesize a per-channel relative path for UI/export compatibility
						auto base = Util::PathFromString(path);
						auto baseNoExt = base;
						baseNoExt.replace_extension("");
						auto channelPath = (baseNoExt.u8string() + "." + channelName + ".funscript");
						scriptCh->UpdateRelativePath(MakePathRelative(channelPath));
						loadedScript = true;
					}
				}
			}
			// Load 1.1 axes
			if (hasAxes) {
				auto mapAxisIdToName = [](const std::string& id) -> std::string {
					if (id == "L0") return "stroke";
					if (id == "L1") return "surge";
					if (id == "L2") return "sway";
					if (id == "R0") return "twist";
					if (id == "R1") return "roll";
					if (id == "R2") return "pitch";
					if (id == "A1") return "suck";
					return id; // fallback
				};
				for (auto& axisObj : json["axes"]) {
					if (!axisObj.is_object()) continue;
					if (!axisObj.contains("actions") || !axisObj["actions"].is_array()) continue;
					std::string axisId = axisObj.contains("id") && axisObj["id"].is_string() ? axisObj["id"].get<std::string>() : std::string{};
					std::string channelName = !axisId.empty() ? mapAxisIdToName(axisId) : std::string{"axis"};
					auto scriptAxis = std::make_shared<Funscript>();
					if (scriptAxis->Deserialize(axisObj, nullptr, false)) {
						scriptAxis = Funscripts.emplace_back(std::move(scriptAxis));
						// Synthesize a per-axis relative path
						auto base = Util::PathFromString(path);
						auto baseNoExt = base;
						baseNoExt.replace_extension("");
						auto axisPath = (baseNoExt.u8string() + "." + channelName + ".funscript");
						scriptAxis->UpdateRelativePath(MakePathRelative(axisPath));
						loadedScript = true;
					}
				}
			}
			return loadedScript;
		}
	}

	// Default 1.0 single-file path
	auto script = std::make_shared<Funscript>();
	auto metadata = Funscript::Metadata();
	if (succ && script->Deserialize(json, &metadata, isFirstFunscript)) {
		// Add existing script to project
		script = Funscripts.emplace_back(std::move(script));
		script->UpdateRelativePath(MakePathRelative(path));
		if (isFirstFunscript) {
			// Initialize project metadata using the first funscript
			auto& projectState = State();
			projectState.metadata = metadata;
		}
		loadedScript = true;
	}
	else {
		// Add empty script to project
		script = std::make_shared<Funscript>();
		script->UpdateRelativePath(MakePathRelative(path));
		script = Funscripts.emplace_back(std::move(script));
	}
	return loadedScript;
}

void OFS_Project::RemoveFunscript(int32_t idx) noexcept
{
    if (idx >= 0 && idx < Funscripts.size()) {
        EV::Enqueue<FunscriptRemovedEvent>(Funscripts[idx]->Title());
        Funscripts.erase(Funscripts.begin() + idx);
    }
}

void OFS_Project::Save(const std::string& path, bool clearUnsavedChanges) noexcept
{
    {
        auto& projectState = State();
        projectState.binaryFunscriptData.clear();
        auto size = OFS_Binary::Serialize(projectState.binaryFunscriptData, *this);
        projectState.binaryFunscriptData.resize(size);
    }

#if 1
    auto projectState = OFS_StateManager::Get()->SerializeProjectAll(true);
    auto projectBin = Util::SerializeCBOR(projectState);
    Util::WriteFile(path.c_str(), projectBin.data(), projectBin.size());
#else
    auto projectState = OFS_StateManager::Get()->SerializeProjectAll(false);
    auto projectJson = Util::SerializeJson(projectState, false);
    Util::WriteFile(path.c_str(), projectJson.data(), projectJson.size());
#endif
    if (clearUnsavedChanges) {
        for (auto& script : Funscripts) {
            script->ClearUnsavedEdits();
        }
    }
}

void OFS_Project::Update(float delta, bool idleMode) noexcept
{
    if (!idleMode) {
        auto& projectState = State();
        projectState.activeTimer += delta;
    }
    for (auto& script : Funscripts) script->Update();
}

bool OFS_Project::HasUnsavedEdits() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    for (auto& script : Funscripts) {
        if (script->HasUnsavedEdits()) {
            return true;
        }
    }
    return false;
}

void OFS_Project::ShowProjectWindow(bool* open) noexcept
{
    if (*open) {
        ImGui::OpenPopup(TR_ID("PROJECT", Tr::PROJECT));
    }

    if (ImGui::BeginPopupModal(TR_ID("PROJECT", Tr::PROJECT), open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        OFS_PROFILE(__FUNCTION__);
        auto& projectState = State();
        auto& Metadata = projectState.metadata;
        ImGui::PushID(Metadata.title.c_str());

        if (IsStandalone()) {
            ImGui::Text("%s: %s", TR(MEDIA), "none, timeline only");

            // With no file to take a length from, this is where it is set. The
            // player is told at once so the timeline redraws under the change
            // rather than at the next load.
            int32_t duration = (int32_t)projectState.standaloneDuration;
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "Length");
            ImGui::SameLine();
            if (OFS::InputDuration("##StandaloneDuration", &duration,
                    (int32_t)MinStandaloneDuration, (int32_t)MaxStandaloneDuration)) {
                projectState.standaloneDuration = (float)duration;
                projectState.metadata.duration = projectState.standaloneDuration;
                OpenFunscripter::ptr->player->SetBlankDuration(projectState.standaloneDuration);
            }
        }
        else {
            ImGui::Text("%s: %s", TR(MEDIA), projectState.relativeMediaPath.c_str());
        }

        Util::FormatTime(Util::FormatBuffer, sizeof(Util::FormatBuffer), projectState.activeTimer, true);
        ImGui::Text("%s: %s", TR(TIME_SPENT), Util::FormatBuffer);
        ImGui::Separator();

        ImGui::Spacing();
        ImGui::TextDisabled(TR(SCRIPTS));
        for (auto& script : Funscripts) {
            if (ImGui::Button(script->Title().c_str(), ImVec2(-1.f, 0.f))) {
                Util::SaveFileDialog(TR(CHANGE_DEFAULT_LOCATION),
                    MakePathAbsolute(script->RelativePath()),
                    [&](auto result) {
                        if (!result.files.empty()) {
                            auto newPath = Util::PathFromString(result.files[0]);
                            if (newPath.extension().u8string() == ".funscript") {
                                script->UpdateRelativePath(MakePathRelative(newPath.u8string()));
                            }
                        }
                    });
            }
            OFS::Tooltip(TR(CHANGE_LOCATION));
        }
        ImGui::PopID();
        ImGui::EndPopup();
    }
}

// The key order a funscript is written in: version, then metadata with its own
// fields in a fixed order, then actions, then anything else the file arrived
// with. Shared by every writer here, which each used to carry their own copy.
static nlohmann::ordered_json orderedFunscriptJson(const nlohmann::json& json) noexcept
{
    nlohmann::ordered_json ordered;
    ordered["version"] = json.contains("version") ? json["version"] : "1.0";

    nlohmann::ordered_json orderedMeta;
    const auto& meta = json.contains("metadata") && json["metadata"].is_object()
        ? json["metadata"] : nlohmann::json::object();
    static constexpr const char* metaOrder[] = {
        "type", "title", "creator", "script_url", "video_url", "tags", "performers",
        "description", "license", "notes", "duration", "durationTime",
        "topic_url", "topic_tags", "topic_creator", "topic_date", "bookmarks", "chapters"
    };
    for (const char* key : metaOrder) {
        if (meta.contains(key)) orderedMeta[key] = meta[key];
    }
    // Anything else the file carried, kept rather than dropped.
    if (meta.is_object()) {
        for (auto it = meta.begin(); it != meta.end(); ++it) {
            if (!orderedMeta.contains(it.key())) orderedMeta[it.key()] = it.value();
        }
    }
    ordered["metadata"] = std::move(orderedMeta);
    ordered["actions"] = json.contains("actions") ? json["actions"] : nlohmann::json::array();
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (!ordered.contains(it.key())) ordered[it.key()] = it.value();
        }
    }
    return ordered;
}

void OFS_Project::ExportFunscripts() noexcept
{
    auto& state = State();
    for (auto& script : Funscripts) {
        FUN_ASSERT(!script->RelativePath().empty(), "path is empty");
        if (!script->RelativePath().empty()) {
            auto json = script->Serialize(state.metadata, true);
            script->ClearUnsavedEdits();
            auto jsonText = orderedFunscriptJson(json).dump(-1, ' ');
            Util::WriteFile(MakePathAbsolute(script->RelativePath()).c_str(), jsonText.data(), jsonText.size());
        }
    }
}

void OFS_Project::ExportFunscripts(const std::string& outputDir) noexcept
{
    auto& state = State();
    for (auto& script : Funscripts) {
        FUN_ASSERT(!script->RelativePath().empty(), "path is empty");
        if (!script->RelativePath().empty()) {
            auto filename = Util::PathFromString(script->RelativePath()).filename();
            auto outputPath = (Util::PathFromString(outputDir) / filename).u8string();
            auto json = script->Serialize(state.metadata, true);
            script->ClearUnsavedEdits();
            auto jsonText = orderedFunscriptJson(json).dump(-1, ' ');
            Util::WriteFile(outputPath.c_str(), jsonText.data(), jsonText.size());
        }
    }
}

void OFS_Project::ExportFunscriptsLimited(const std::string& outputDir,
    const std::vector<FunscriptArray>& limitedActions) noexcept
{
    FUN_ASSERT(limitedActions.size() == Funscripts.size(), "one set of actions per script");
    auto& state = State();
    for (size_t i = 0; i < Funscripts.size() && i < limitedActions.size(); i += 1) {
        auto& script = Funscripts[i];
        if (script->RelativePath().empty()) continue;

        // Serialized from a copy, so the project keeps the script it has: this
        // writes a version for a device, it does not change what is being
        // scripted, and must not mark the project as edited either.
        Funscript::FunscriptData copy;
        copy.Actions = limitedActions[i];
        nlohmann::json json;
        Funscript::Serialize(json, copy, state.metadata, true);

        auto filename = Util::PathFromString(script->RelativePath()).filename();
        auto outputPath = (Util::PathFromString(outputDir) / filename).u8string();
        auto jsonText = orderedFunscriptJson(json).dump(-1, ' ');
        Util::WriteFile(outputPath.c_str(), jsonText.data(), jsonText.size());
    }
}

void OFS_Project::ExportFunscript(const std::string& outputPath, int32_t idx) noexcept
{
    FUN_ASSERT(idx >= 0 && idx < Funscripts.size(), "out of bounds");
    auto& state = State();
    auto json = Funscripts[idx]->Serialize(state.metadata, true);
    Funscripts[idx]->ClearUnsavedEdits();
    // Using this function changes the default path
    Funscripts[idx]->UpdateRelativePath(MakePathRelative(outputPath));
    auto jsonText = orderedFunscriptJson(json).dump(-1, ' ');
    Util::WriteFile(outputPath.c_str(), jsonText.data(), jsonText.size());
}

void OFS_Project::ExportFunscript2Quick() noexcept
{
	// Export a single combined 2.0 funscript next to each script's default path.
	// If multiple scripts are loaded, export one 2.0 file based on the first script path
	// and include other scripts as channels.
	auto& state = State();
	if (Funscripts.empty()) return;

	// Determine primary output path (use first script's relative path)
	auto baseRel = Util::PathFromString(Funscripts[0]->RelativePath());
	if (baseRel.empty()) return;
	// Ensure extension is .funscript
	baseRel.replace_extension(".funscript");
	auto outPath = MakePathAbsolute(baseRel.u8string());

	// Start from the primary axis as fully serialized 1.0 (keeps chapters/bookmarks)
	nlohmann::json root = Funscripts[0]->Serialize(state.metadata, true);
	root["version"] = "2.0";
	// Channels for subsequent scripts using filename suffix as channel name if present
	{
		nlohmann::json channels = nlohmann::json::object();
		for (size_t i = 1; i < Funscripts.size(); ++i) {
			auto& fs = Funscripts[i];
			nlohmann::json chObj;
			// Only include actions
			Funscript::Serialize(chObj, fs->Data(), state.metadata, false);
			nlohmann::json actions = std::move(chObj["actions"]);
			// Channel name derived from relative filename like name.roll.funscript -> "roll"
			auto rel = Util::PathFromString(fs->RelativePath());
			auto stem = rel.stem().u8string();
			// If the base contains dots, use the last segment as channel (e.g., name.roll)
			auto dot = stem.rfind('.');
			std::string channelName = dot != std::string::npos ? stem.substr(dot + 1) : fs->Title();
			channels[channelName] = nlohmann::json{ {"actions", std::move(actions)} };
		}
		if (!channels.empty()) root["channels"] = std::move(channels);
	}

    // Write file with ordered keys: version, metadata (ordered), actions, channels
    nlohmann::ordered_json ordered;
    ordered["version"] = root.contains("version") ? root["version"] : "2.0";
    {
        nlohmann::ordered_json orderedMeta;
        const auto& meta = root.contains("metadata") && root["metadata"].is_object() ? root["metadata"] : nlohmann::json::object();
        if (meta.contains("type")) orderedMeta["type"] = meta["type"];
        if (meta.contains("title")) orderedMeta["title"] = meta["title"];
        if (meta.contains("creator")) orderedMeta["creator"] = meta["creator"];
        if (meta.contains("script_url")) orderedMeta["script_url"] = meta["script_url"];
        if (meta.contains("video_url")) orderedMeta["video_url"] = meta["video_url"];
        if (meta.contains("tags")) orderedMeta["tags"] = meta["tags"];
        if (meta.contains("performers")) orderedMeta["performers"] = meta["performers"];
        if (meta.contains("description")) orderedMeta["description"] = meta["description"];
        if (meta.contains("license")) orderedMeta["license"] = meta["license"];
        if (meta.contains("notes")) orderedMeta["notes"] = meta["notes"];
        if (meta.contains("duration")) orderedMeta["duration"] = meta["duration"];
        if (meta.contains("durationTime")) orderedMeta["durationTime"] = meta["durationTime"];
        if (meta.contains("topic_url")) orderedMeta["topic_url"] = meta["topic_url"];
        if (meta.contains("topic_tags")) orderedMeta["topic_tags"] = meta["topic_tags"];
        if (meta.contains("topic_creator")) orderedMeta["topic_creator"] = meta["topic_creator"];
        if (meta.contains("topic_date")) orderedMeta["topic_date"] = meta["topic_date"];
        if (meta.contains("bookmarks")) orderedMeta["bookmarks"] = meta["bookmarks"];
        if (meta.contains("chapters")) orderedMeta["chapters"] = meta["chapters"];
        ordered["metadata"] = std::move(orderedMeta);
    }
    ordered["actions"] = root.contains("actions") ? root["actions"] : nlohmann::json::array();
    if (root.contains("channels")) ordered["channels"] = root["channels"];
    // Preserve any unknown top-level keys
    if (root.is_object()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            const auto& key = it.key();
            if (!ordered.contains(key)) {
                ordered[key] = it.value();
            }
        }
    }
    auto jsonText = ordered.dump(-1, ' ');
	Util::WriteFile(outPath.c_str(), jsonText.data(), jsonText.size());
}

void OFS_Project::ExportFunscript11Quick() noexcept
{
	// Export a single combined 1.1 funscript (axes array) next to first script's path
	if (Funscripts.empty()) return;
	auto& state = State();

	// Determine output path based on first script
	auto baseRel = Util::PathFromString(Funscripts[0]->RelativePath());
	if (baseRel.empty()) return;
	baseRel.replace_extension(".funscript");
	auto outPath = MakePathAbsolute(baseRel.u8string());

	// Start from the primary axis as fully serialized 1.0 (keeps chapters/bookmarks)
	nlohmann::json root = Funscripts[0]->Serialize(state.metadata, true);
	root["version"] = "1.1";
	// axes array from subsequent scripts using id mapping inverse
	{
		auto channelNameToId = [](const std::string& name) -> std::string {
			if (name == "stroke") return "L0";
			if (name == "surge") return "L1";
			if (name == "sway") return "L2";
			if (name == "twist") return "R0";
			if (name == "roll") return "R1";
			if (name == "pitch") return "R2";
			if (name == "suck") return "A1";
			return name;
		};
		nlohmann::json axes = nlohmann::json::array();
		for (size_t i = 1; i < Funscripts.size(); ++i) {
			auto& fs = Funscripts[i];
			nlohmann::json chObj;
			Funscript::Serialize(chObj, fs->Data(), state.metadata, false);
			nlohmann::json actions = std::move(chObj["actions"]);
			// Derive channel name from filename suffix
			auto rel = Util::PathFromString(fs->RelativePath());
			auto stem = rel.stem().u8string();
			auto dot = stem.rfind('.');
			std::string channelName = dot != std::string::npos ? stem.substr(dot + 1) : fs->Title();
			std::string axisId = channelNameToId(channelName);
			axes.emplace_back(nlohmann::json{ {"id", axisId}, {"actions", std::move(actions)} });
		}
		if (!axes.empty()) root["axes"] = std::move(axes);
	}

    // Write file with ordered keys: version, metadata (ordered), actions, axes
    nlohmann::ordered_json ordered;
    ordered["version"] = root.contains("version") ? root["version"] : "1.1";
    {
        nlohmann::ordered_json orderedMeta;
        const auto& meta = root.contains("metadata") && root["metadata"].is_object() ? root["metadata"] : nlohmann::json::object();
        if (meta.contains("type")) orderedMeta["type"] = meta["type"];
        if (meta.contains("title")) orderedMeta["title"] = meta["title"];
        if (meta.contains("creator")) orderedMeta["creator"] = meta["creator"];
        if (meta.contains("script_url")) orderedMeta["script_url"] = meta["script_url"];
        if (meta.contains("video_url")) orderedMeta["video_url"] = meta["video_url"];
        if (meta.contains("tags")) orderedMeta["tags"] = meta["tags"];
        if (meta.contains("performers")) orderedMeta["performers"] = meta["performers"];
        if (meta.contains("description")) orderedMeta["description"] = meta["description"];
        if (meta.contains("license")) orderedMeta["license"] = meta["license"];
        if (meta.contains("notes")) orderedMeta["notes"] = meta["notes"];
        if (meta.contains("duration")) orderedMeta["duration"] = meta["duration"];
        if (meta.contains("durationTime")) orderedMeta["durationTime"] = meta["durationTime"];
        if (meta.contains("topic_url")) orderedMeta["topic_url"] = meta["topic_url"];
        if (meta.contains("topic_tags")) orderedMeta["topic_tags"] = meta["topic_tags"];
        if (meta.contains("topic_creator")) orderedMeta["topic_creator"] = meta["topic_creator"];
        if (meta.contains("topic_date")) orderedMeta["topic_date"] = meta["topic_date"];
        if (meta.contains("bookmarks")) orderedMeta["bookmarks"] = meta["bookmarks"];
        if (meta.contains("chapters")) orderedMeta["chapters"] = meta["chapters"];
        ordered["metadata"] = std::move(orderedMeta);
    }
    ordered["actions"] = root.contains("actions") ? root["actions"] : nlohmann::json::array();
    if (root.contains("axes")) ordered["axes"] = root["axes"];
    if (root.is_object()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            const auto& key = it.key();
            if (!ordered.contains(key)) {
                ordered[key] = it.value();
            }
        }
    }
    auto jsonText = ordered.dump(-1, ' ');
	Util::WriteFile(outPath.c_str(), jsonText.data(), jsonText.size());
}

void OFS_Project::loadMultiAxis(const std::string& rootScript) noexcept
{
    std::vector<std::filesystem::path> relatedFiles;
    {
        auto filename = Util::Filename(rootScript) + '.';
        auto searchDirectory = Util::PathFromString(rootScript);
        searchDirectory.remove_filename();

        std::error_code ec;
        std::filesystem::directory_iterator dirIt(searchDirectory, ec);
        for (auto&& entry : dirIt) {
            auto extension = entry.path()
                                 .extension()
                                 .u8string();
            auto currentFilename = entry.path()
                                       .filename()
                                       .replace_extension("")
                                       .u8string();

            if (extension == Funscript::Extension
                && Util::StringStartsWith(currentFilename, filename)
                && currentFilename != filename) {
                relatedFiles.emplace_back(entry.path());
            }
        }
    }
    // reorder for 3d simulator
    std::array<std::string, 3> desiredOrder{
        // it's in reverse order
        ".twist.funscript",
        ".pitch.funscript",
        ".roll.funscript"
    };
    if (relatedFiles.size() > 1) {
        for (auto& ending : desiredOrder) {
            for (int i = 0; i < relatedFiles.size(); i++) {
                auto& path = relatedFiles[i];
                if (Util::StringEndsWith(path.u8string(), ending)) {
                    auto move = std::move(path);
                    relatedFiles.erase(relatedFiles.begin() + i);
                    relatedFiles.emplace_back(std::move(move));
                    break;
                }
            }
        }
    }
    // load the related files
    for (int i = relatedFiles.size() - 1; i >= 0; i -= 1) {
        auto& file = relatedFiles[i];
        auto filePathString = file.u8string();
        AddFunscript(filePathString);
    }
}

std::string OFS_Project::MakePathAbsolute(const std::string& relPathStr) const noexcept
{
    auto relPath = Util::PathFromString(relPathStr);
    FUN_ASSERT(relPath.is_relative(), "Path isn't relative");
    if (relPath.is_absolute()) {
        LOGF_ERROR("Path was already absolute. \"%s\"", relPathStr.c_str());
        return relPathStr;
    }
    else {
        auto projectDir = Util::PathFromString(lastPath);
        projectDir.remove_filename();
        std::error_code ec;
        auto absPath = std::filesystem::absolute(projectDir / relPath, ec);
        if (!ec) {
            auto absPathStr = absPath.u8string();
            LOGF_INFO("Convert relative path \"%s\" to absolute \"%s\"", relPath.u8string().c_str(), absPathStr.c_str());
            return absPathStr;
        }
        FUN_ASSERT(false, "This must not happen.");
        LOG_ERROR("Failed to convert path to absolute path.");
        return "";
    }
}

std::string OFS_Project::MakePathRelative(const std::string& absPathStr) const noexcept
{
    auto absPath = Util::PathFromString(absPathStr);
    auto projectDir = Util::PathFromString(lastPath).parent_path();
    auto relPath = absPath.lexically_relative(projectDir);
    auto relPathStr = relPath.u8string();
    LOGF_INFO("Convert absolute path \"%s\" to relative \"%s\"", absPathStr.c_str(), relPathStr.c_str());
    return relPathStr;
}

std::string OFS_Project::MediaPath() const noexcept
{
    auto& projectState = State();
    return MakePathAbsolute(projectState.relativeMediaPath);
}

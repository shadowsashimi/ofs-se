#include "OFS_Preferences.h"
#include "OFS_Util.h"
#include "OpenFunscripter.h"
#include "OFS_Localization.h"
#include "OFS_ImGui.h"
#include "OFS_SashimiTheme.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include "OFS_Reflection.h"
#include "OFS_StateHandle.h"
#include "state/states/BaseOverlayState.h"

OFS_Preferences::OFS_Preferences() noexcept
{
	prefStateHandle = OFS_AppState<PreferenceState>::Register(PreferenceState::StateName);
	auto& state = PreferenceState::State(prefStateHandle);
    OFS_DynFontAtlas::FontOverride = state.fontOverride;
}

static void copyTranslationHelper() noexcept
{
	auto srcDir = Util::Basepath() / "data" / OFS_Translator::TranslationDir;
	auto targetDir = Util::Prefpath(OFS_Translator::TranslationDir);
	std::error_code ec;
	std::filesystem::directory_iterator langDirIt(srcDir, ec);
	for(auto& pIt : langDirIt) {
		if(pIt.path().extension() == ".csv") {
			auto targetFile = targetDir / pIt.path().filename();
			if(Util::FileExists(targetFile.u8string())) {
				// merge the two
				auto input1 = pIt.path().u8string();
				auto input2 = targetFile.u8string();
				if(OFS_Translator::MergeIntoOne(input1.c_str(), input2.c_str(), input2.c_str())) {
					std::filesystem::remove(pIt.path(), ec);
				}
			}
			else {
				std::filesystem::copy_file(pIt.path(), targetFile, ec);
				if(!ec) {
					std::filesystem::remove(pIt.path(), ec);
				}
			}
		}
	}
}

// A section's title and a sentence on what it covers, then a rule, so every
// section opens the same way.
static void sectionHeader(const char* title, const char* description) noexcept
{
	ImGui::TextUnformatted(title);
	if (description != nullptr) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("%s", description);
		ImGui::PopTextWrapPos();
	}
	ImGui::Separator();
	ImGui::Spacing();
}

// A group of related settings within a section.
static void groupHeader(const char* title) noexcept
{
	ImGui::Spacing();
	OFS::SeparatorText(title);
}

enum PreferenceSection : int32_t
{
	SectionApplication,
	SectionVideo,
	SectionScripting,
	SectionTimeline,
	SectionSimulator,
	SectionCount
};

void OFS_Preferences::OpenTimelineSection() noexcept
{
	activeSection = SectionTimeline;
	ShowWindow = true;
}

// Laid out as a sidebar of sections rather than tabs. Tabs held three
// sections and could not have held five without crowding, and the settings
// for how the timeline is drawn were not in here at all: they lived in the
// timeline's right click menu, out of sight of anyone looking for them here.
bool OFS_Preferences::ShowPreferenceWindow() noexcept
{
	bool save = false;
	if (ShowWindow)
		ImGui::OpenPopup(TR_ID("PREFERENCES", Tr::PREFERENCES));

	const auto* viewport = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(em * 44.f, em * 28.f), ImGuiCond_Appearing);
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal(TR_ID("PREFERENCES", Tr::PREFERENCES), &ShowWindow, ImGuiWindowFlags_None))
	{
		OFS_PROFILE(__FUNCTION__);
		auto& state = PreferenceState::State(prefStateHandle);
		auto app = OpenFunscripter::ptr;

		const char* sectionNames[SectionCount] = {
			TR(APPLICATION),
			TR(VIDEOPLAYER),
			TR(SCRIPTING),
			"Timeline",
			TR(SIMULATOR),
		};

		ImGui::BeginChild("##prefSections", ImVec2(em * 9.f, 0.f), true);
		for (int32_t i = 0; i < SectionCount; i += 1) {
			if (ImGui::Selectable(sectionNames[i], activeSection == i)) {
				activeSection = i;
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##prefContent", ImVec2(0.f, 0.f), false);
		ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);

		switch (activeSection)
		{
			case SectionApplication:
			{
				sectionHeader(TR(APPLICATION), "How OFS looks and how hard it works to draw itself.");

				groupHeader("Theme");
				// Not localized on purpose, "Sashimi" is the build name.
				if (ImGui::RadioButton("Sashimi", (int*)&state.currentTheme,
					static_cast<int32_t>(OFS_Theme::Sashimi))) {
					SetTheme((OFS_Theme)state.currentTheme);
					save = true;
				}
				ImGui::SameLine();
				if (ImGui::RadioButton(TR(DARK_MODE), (int*)&state.currentTheme,
					static_cast<int32_t>(OFS_Theme::Dark))) {
					SetTheme((OFS_Theme)state.currentTheme);
					save = true;
				}
				ImGui::SameLine();
				if (ImGui::RadioButton(TR(LIGHT_MODE), (int*)&state.currentTheme,
					static_cast<int32_t>(OFS_Theme::Light))) {
					SetTheme((OFS_Theme)state.currentTheme);
					save = true;
				}
				if (ImGui::Checkbox("Rounded corners", &state.roundedCorners)) {
					SetTheme((OFS_Theme)state.currentTheme);
					save = true;
				}
				OFS::Tooltip("Round the corners of windows, buttons, tabs and timeline lanes, in any theme. Off gives the square look of the original OFS.");

				groupHeader("Performance");
				ImGui::PushTextWrapPos(0.f);
				ImGui::TextDisabled("%s", TR(PREFERENCES_TXT));
				ImGui::PopTextWrapPos();
				if (OFS::StepperInt("Frame limit (fps)", "##FrameLimit", &state.framerateLimit, 10, 60, 300)) {
					state.framerateLimit = Util::Clamp(state.framerateLimit, 60, 300);
					save = true;
				}
				OFS::Tooltip(TR(FRAME_LIMIT_TOOLTIP));
				if (ImGui::Checkbox(TR(VSYNC), (bool*)&state.vsync)) {
					state.vsync = Util::Clamp(state.vsync, 0, 1); // just in case...
					SDL_GL_SetSwapInterval(state.vsync);
					save = true;
				}
				OFS::Tooltip(TR(VSYNC_TOOLTIP));

				groupHeader("Text");
				ImGui::InputText(TR(FONT), state.fontOverride.empty() ? (char*)TR(DEFAULT_FONT) : (char*)state.fontOverride.c_str(),
					state.fontOverride.size(), ImGuiInputTextFlags_ReadOnly);
				ImGui::SameLine();
				if (ImGui::Button(TR(CHANGE))) {
					Util::OpenFileDialog(TR(CHOOSE_FONT), "",
						[&](auto& result) {
							if (result.files.size() > 0) {
								state.fontOverride = result.files.back();
								OpenFunscripter::ptr->LoadOverrideFont(state.fontOverride);
								save = true;
							}
						}, false, { "*.ttf", "*.otf" }, "Fonts (*.ttf, *.otf)");
				}
				ImGui::SameLine();
				if (ImGui::Button(TR(CLEAR))) {
					state.fontOverride = "";
					EV::Enqueue<OFS_DeferEvent>([]()
					{
						// fonts can't be updated during a frame
						// this updates the font during event processing
						// which is not during the frame
						auto app = OpenFunscripter::ptr;
						app->LoadOverrideFont("");
					});
				}

				if (OFS::StepperInt("Font size", "##FontSize", (int32_t*)&state.defaultFontSize, 1, 8, 64)) {
					state.defaultFontSize = Util::Clamp(state.defaultFontSize, 8, 64);
					EV::Enqueue<OFS_DeferEvent>([stateHandle = prefStateHandle]() {
						// fonts can't be updated during a frame
						// this updates the font during event processing
						// which is not during the frame
						auto& state = PreferenceState::State(stateHandle);
						auto app = OpenFunscripter::ptr;
						app->LoadOverrideFont(state.fontOverride);
					});
					save = true;
				}
				if(ImGui::BeginCombo(TR_ID("LANGUAGE", Tr::LANGUAGE), state.languageCsv.empty() ? "English" : state.languageCsv.c_str()))
				{
					for(auto& file : translationFiles) {
						if(ImGui::Selectable(file.c_str(), file == state.languageCsv)) {
							if(OFS_Translator::ptr->LoadTranslation(file.c_str())) {
								state.languageCsv = file;
								OFS_DynFontAtlas::AddTranslationText();
							}
						}
					}
					ImGui::EndCombo();
				}
				if(ImGui::IsItemClicked(ImGuiMouseButton_Left))	{
					copyTranslationHelper();
					translationFiles.clear();
					std::error_code ec;
					std::filesystem::directory_iterator dirIt(Util::Prefpath(OFS_Translator::TranslationDir), ec);
					for (auto& pIt : dirIt) {
						if(pIt.path().extension() == ".csv") {
							translationFiles.emplace_back(pIt.path().filename().u8string());
						}
					}
				}
				ImGui::SameLine();
				if(ImGui::Button(TR(RESET))) {
					state.languageCsv = std::string();
					OFS_Translator::ptr->LoadDefaults();
				}
				ImGui::SameLine();
				if(ImGui::Button(FMT("%s###DIRECTORY_TRANSLATION", ICON_FOLDER_OPEN)))
				{
					Util::OpenFileExplorer(Util::Prefpath(OFS_Translator::TranslationDir));
				}
				OFS::Tooltip("Open the folder translations are loaded from.");
				break;
			}
			case SectionVideo:
			{
				sectionHeader(TR(VIDEOPLAYER), "How video is decoded. A change here takes effect once OFS is restarted.");
				if (ImGui::Checkbox(TR(FORCE_HW_DECODING), &state.forceHwDecoding)) {
					save = true;
				}
				OFS::Tooltip(TR(FORCE_HW_DECODING_TOOLTIP));
				break;
			}
			case SectionScripting:
			{
				sectionHeader(TR(SCRIPTING), "Stepping through video, and what happens when a project is created.");
				static constexpr int32_t oneFrame = 1;
				// With its unit, where a bare 6 did not say six of what.
				if (ImGui::InputScalar(TR(FAST_FRAME_STEP), ImGuiDataType_S32, &state.fastStepAmount, &oneFrame, &oneFrame, "%d frames")) {
					save = true;
					state.fastStepAmount = Util::Clamp<int32_t>(state.fastStepAmount, 2, 30);
				}
				OFS::Tooltip(TR(FAST_FRAME_STEP_TOOLTIP));
				if (ImGui::Checkbox(TR(SHOW_METADATA_DIALOG_ON_NEW_PROJECT), &state.showMetaOnNew)) {
					save = true;
				}
				break;
			}
			case SectionTimeline:
			{
				sectionHeader("Timeline", "How scripts are drawn on the timeline, and where points land "
					"when placed with the mouse. The timeline's right click menu has the most used "
					"of these too.");
				auto& overlayState = BaseOverlay::State();

				groupHeader("Drawing");
				ImGui::Checkbox(TR(SHOW_ACTION_LINES), &BaseOverlay::ShowLines);
				ImGui::SameLine();
				ImGui::Checkbox(TR(SHOW_ACTION_POINTS), &BaseOverlay::ShowPoints);
				if (ImGui::Checkbox(TR(SPLINE_MODE), &overlayState.SplineMode)) save = true;
				OFS::Tooltip("Draw the script as a smooth curve through its points instead of straight lines.");
				if (ImGui::Checkbox(TR(SHOW_VIDEO_POSITION), &overlayState.SyncLineEnable)) save = true;
				OFS::Tooltip(TR(SHOW_VIDEO_POSITION_TOOLTIP));

				groupHeader("Speed");
				if (ImGui::Checkbox(TR_ID("HighlightEnable", Tr::ENABLE_MAX_SPEED_HIGHLIGHT), &overlayState.ShowMaxSpeedHighlight)) {
					save = true;
				}
				ImGui::BeginDisabled(!overlayState.ShowMaxSpeedHighlight);
				// "Highlight above", where the translation string read "Highlight treshold".
				if (OFS::StepperFloat("Highlight above (units/s)", "##HighlightAbove", &overlayState.MaxSpeedPerSecond, 50.f, 1.f, 5000.f, "%.0f")) {
					save = true;
				}
				OFS::Tooltip("Lines faster than this are drawn in the highlight colour instead of their speed colour.");
				ImGui::ColorEdit3(TR_ID("HighlightColor", Tr::COLOR), &overlayState.MaxSpeedColor.Value.x, ImGuiColorEditFlags_NoInputs);
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					save = true;
				}
				ImGui::EndDisabled();

				groupHeader("Snapping");
				ImGui::Checkbox(TR(SNAP_TO_GRID), &overlayState.SnapToGrid);
				OFS::Tooltip("Snaps points placed or dragged with the mouse onto the grid of the "
							 "current mode: frames, or the tempo grid's division. Hold Alt to place "
							 "one point off the grid.");
				{
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted("Round position");
					ImGui::SameLine();
					DrawPositionRoundingSelector("##prefPosStep");
				}

				groupHeader(TR(WAVEFORM));
				app->scriptTimeline.DrawWaveformSettings();
				break;
			}
			case SectionSimulator:
			{
				sectionHeader(TR(SIMULATOR), "What the simulator draws. Its size, placement and colours "
					"are set in the Simulator panel itself, where the result can be seen as it changes.");
				app->simulator.DrawModeSelector("##prefSimulatorMode");
				break;
			}
		}

		ImGui::PopItemWidth();
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	return save;
}

void OFS_Preferences::SetTheme(OFS_Theme theme) noexcept
{
	auto& style = ImGui::GetStyle();
	auto& io = ImGui::GetIO();
	const auto& state = PreferenceState::State(prefStateHandle);

	// Start from stock spacing each time, so going from Sashimi to Dark or
	// Light gives the same layout as starting in them rather than keeping
	// Sashimi's padding.
	style = ImGuiStyle();

	switch (theme) {
		case OFS_Theme::Sashimi: {
			OFS_Sashimi::ApplyStyle(style);
			break;
		}
		case OFS_Theme::Dark: {
			ImGui::StyleColorsDark(&style);
			break;
		}
		case OFS_Theme::Light: {
			ImGui::StyleColorsLight(&style);
			// Stock Light draws frames white, the same as a popup, so an
			// unticked box or an empty field in a dialog was not drawn at all.
			style.FrameBorderSize = 1.f;
			break;
		}
	}

	OFS_Sashimi::SetRounding(style, state.roundedCorners);
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		// Windows dragged out of the main one become OS windows, whose own
		// corners suit a smaller radius.
		if (state.roundedCorners) style.WindowRounding = 6.f;
		style.Colors[ImGuiCol_WindowBg].w = 1.f;
		style.Colors[ImGuiCol_PopupBg].w = 1.f;
	}
	OFS_Sashimi::UpdateRoles(style, theme == OFS_Theme::Sashimi);
}

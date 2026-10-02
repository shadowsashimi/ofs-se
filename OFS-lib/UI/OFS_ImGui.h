#pragma once

#include "imgui.h"
#include "OFS_Util.h"
#include "OFS_Localization.h"
#include "OFS_SashimiTheme.h"

namespace OFS {
	// ExampleAppLog taken from "imgui_demo.cpp"
	struct AppLog
	{
		ImGuiTextBuffer     Buf;
		ImGuiTextFilter     Filter;
		ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
		bool                AutoScroll;  // Keep scrolling if already at the bottom.

		AppLog() noexcept
		{
			AutoScroll = true;
			Clear();
		}

		void Clear() noexcept
		{
			Buf.clear();
			LineOffsets.clear();
			LineOffsets.push_back(0);
		}

		inline int LogSizeBytes() const noexcept
		{
			return Buf.Buf.size_in_bytes() + LineOffsets.size_in_bytes();
		}

		inline int AllocatedSizeBytes() const noexcept
		{
			return Buf.Buf.Capacity + LineOffsets.Capacity*sizeof(int);
		}

		void AddLog(const char* fmt, ...) noexcept IM_FMTARGS(2)
		{
			int old_size = Buf.size();
			va_list args;
			va_start(args, fmt);
			Buf.appendfv(fmt, args);
			va_end(args);
			for (int new_size = Buf.size(); old_size < new_size; old_size++)
				if (Buf[old_size] == '\n')
					LineOffsets.push_back(old_size + 1);
		}

		void Draw(const char* title, bool* p_open = NULL) noexcept
		{
			if (!ImGui::Begin(title, p_open)) {
				ImGui::End();
				return;
			}

			// Options menu
			if (ImGui::BeginPopup(TR_ID("OPTIONS", Tr::OPTIONS))) {
				ImGui::Checkbox(TR(AUTO_SCROLL), &AutoScroll);
				ImGui::EndPopup();
			}

			// Main window
			if (ImGui::Button(TR(OPTIONS)))
				ImGui::OpenPopup(TR_ID("OPTIONS", Tr::OPTIONS));
			ImGui::SameLine();
			bool clear = ImGui::Button(TR(CLEAR));
			ImGui::SameLine();
			bool copy = ImGui::Button(TR(COPY));
			ImGui::SameLine();
			Filter.Draw(TR(FILTER), -100.0f);

			ImGui::Text("%s: %s", TR(USED), Util::FormatBytes(LogSizeBytes()));
			ImGui::SameLine();
			ImGui::Text("%s: %s", TR(ALLOCATED), Util::FormatBytes(AllocatedSizeBytes()));
			ImGui::Separator();
			ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

			if (clear)
				Clear();
			if (copy)
				ImGui::LogToClipboard();

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			const char* buf = Buf.begin();
			const char* buf_end = Buf.end();
			if (Filter.IsActive()) {
				for (int line_no = 0; line_no < LineOffsets.Size; line_no++) {
					const char* line_start = buf + LineOffsets[line_no];
					const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
					if (Filter.PassFilter(line_start, line_end))
						ImGui::TextUnformatted(line_start, line_end);
				}
			}
			else {
				ImGuiListClipper clipper;
				clipper.Begin(LineOffsets.Size);
				while (clipper.Step()) {
					for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++) {
						const char* line_start = buf + LineOffsets[line_no];
						const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
						ImGui::TextUnformatted(line_start, line_end);
					}
				}
				clipper.End();
			}
			ImGui::PopStyleVar();

			if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
				ImGui::SetScrollHereY(1.0f);

			ImGui::EndChild();
			ImGui::End();
		}
	};

	// same as ImGui::Image except it has an id
	void ImageWithId(ImGuiID id, ImTextureID user_texture_id, const ImVec2& size, const ImVec2& uv0 = ImVec2(0, 0), const ImVec2& uv1 = ImVec2(1, 1), const ImVec4& tint_col = ImVec4(1, 1, 1, 1), const ImVec4& border_col = ImVec4(0, 0, 0, 0)) noexcept;
    bool Spinner(const char* label, float radius, int thickness, const ImU32& color) noexcept;

	// Width a tooltip wraps at, in multiples of the font size. A tooltip that
	// explains something is a sentence or three, and unwrapped it lays itself
	// out as one line running off the side of the screen.
	constexpr float TooltipWrapEm = 30.f;

	inline void Tooltip(const char* tip) noexcept
	{
		// The normal delay rather than the short one: a tooltip that appears as
		// the mouse passes over a row of buttons covers the next button before
		// it can be reached.
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * TooltipWrapEm);
			ImGui::TextUnformatted(tip);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	// The same, for a tooltip that has to substitute something in. Separate
	// from Tooltip so the common case stays a plain string with no format
	// specifier to get wrong.
	inline void TooltipFmt(const char* fmt, ...) noexcept
	{
		// The normal delay rather than the short one: a tooltip that appears as
		// the mouse passes over a row of buttons covers the next button before
		// it can be reached.
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
			va_list args;
			va_start(args, fmt);
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * TooltipWrapEm);
			ImGui::TextV(fmt, args);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
			va_end(args);
		}
	}

	// SameLine for a checkbox labelled nextLabel, unless it would not fit in
	// what is left of the row, in which case it starts the next row instead.
	// A row of checkboxes laid out with plain SameLine ran off the side of any
	// panel narrower than the row.
	inline void SameLineIfCheckboxFits(const char* nextLabel) noexcept
	{
		const auto& style = ImGui::GetStyle();
		const float width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x
			+ ImGui::CalcTextSize(nextLabel, nullptr, true).x;
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
	}

	// A heading with a rule running from it to the edge of the window, to open
	// a group of related controls. ImGui's own SeparatorText arrived in 1.89.4,
	// after the version vendored here.
	inline void SeparatorText(const char* text) noexcept
	{
		ImGui::TextUnformatted(text);
		ImGui::SameLine();
		const float lineHeight = ImGui::GetTextLineHeight();
		const float available = ImGui::GetContentRegionAvail().x;
		// Components by hand: this header is included in places that do not
		// define IMGUI_DEFINE_MATH_OPERATORS, so ImVec2 has no + here.
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const ImVec2 start(cursor.x, cursor.y + (lineHeight * 0.5f));
		const ImVec2 end(cursor.x + available, start.y);
		if (available > 0.f) {
			ImGui::GetWindowDrawList()->AddLine(start, end, ImGui::GetColorU32(ImGuiCol_Separator));
		}
		ImGui::Dummy(ImVec2(available > 0.f ? available : 0.f, lineHeight));
	}

	// A button that stays lit while whatever it toggles is on, in the same
	// colours as a selected segment of SegmentedControl, so an on/off control
	// and a choice between options read as the same family.
	inline bool ToggleButton(const char* label, bool on, const ImVec2& size = ImVec2(0.f, 0.f)) noexcept
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
		if (on) {
			ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::PinkFill));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
			ImGui::PushStyleColor(ImGuiCol_Border, OFS_Sashimi::V4(OFS_Sashimi::Pink));
			ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::V4(OFS_Sashimi::PinkPale));
		}
		else {
			ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::Grey15));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::Grey25));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::Grey30));
			ImGui::PushStyleColor(ImGuiCol_Border, OFS_Sashimi::V4(OFS_Sashimi::Grey30));
			ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::V4(OFS_Sashimi::Grey80));
		}
		const bool clicked = ImGui::Button(label, size);
		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar();
		return clicked;
	}

	// A set of mutually exclusive options drawn as one bar of segments, in
	// place of a dropdown. Every option is readable without opening anything,
	// and switching is a single click rather than two.
	//
	// Segments share the row equally and are sized so the widest label fits: a
	// bar whose segments are all different widths reads as a row of unrelated
	// buttons rather than as one control. Where they cannot all fit across, the
	// rows are balanced rather than packed, so four options in a narrow panel
	// become two rows of two instead of three and a stray one. That also
	// answers the thing ImGui has no layout for, which is a row that has to
	// survive a longer translation or a panel dragged narrower.
	//
	// tips may be null, or hold a null entry, for a segment that needs none.
	// They are shown from inside here because hover is only testable for the
	// item last submitted.
	//
	// Returns the index chosen this frame, or -1 when nothing was clicked.
	inline int32_t SegmentedControl(const char* id, const char* const* labels,
		const char* const* tips, int32_t count, int32_t selected) noexcept
	{
		if (count <= 0) return -1;
		const auto& style = ImGui::GetStyle();

		float widest = 0.f;
		for (int32_t i = 0; i < count; i += 1) {
			const float w = ImGui::CalcTextSize(labels[i]).x;
			if (w > widest) widest = w;
		}
		const float segmentMin = widest + (style.FramePadding.x * 2.f);
		const float available = ImGui::GetContentRegionAvail().x;

		// Segments touch, with no gap between them, and share one outline, so a
		// bar reads as a single control and it is obvious which options belong
		// together. With a gap and a border each, the options of neighbouring
		// controls ran together into one long row of buttons.
		int32_t perRow = (int32_t)(available / segmentMin);
		perRow = Util::Clamp(perRow, 1, count);
		const int32_t rows = (count + perRow - 1) / perRow;
		perRow = (count + rows - 1) / rows;

		int32_t clicked = -1;
		ImGui::PushID(id);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, style.ItemSpacing.y));
		auto* drawList = ImGui::GetWindowDrawList();
		const ImVec2 groupMin = ImGui::GetCursorScreenPos();
		ImVec2 groupMax = groupMin;
		for (int32_t i = 0; i < count; i += 1) {
			const int32_t column = i % perRow;
			if (column != 0) ImGui::SameLine();

			// Recomputed per row, since the last row may hold fewer.
			const int32_t rowStart = i - column;
			const int32_t remaining = count - rowStart;
			const int32_t rowCount = perRow < remaining ? perRow : remaining;
			const float width = available / (float)rowCount;

			const bool isSelected = (i == selected);
			if (isSelected) {
				ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::PinkFill));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
				ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::V4(OFS_Sashimi::PinkPale));
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::Grey15));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::Grey25));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::Grey30));
				ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::V4(OFS_Sashimi::Grey80));
			}

			ImGui::PushID(i);
			if (ImGui::Button(labels[i], ImVec2(width, 0.f))) clicked = i;
			ImGui::PopID();
			ImGui::PopStyleColor(4);

			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 itemMax = ImGui::GetItemRectMax();
			if (itemMax.x > groupMax.x) groupMax.x = itemMax.x;
			if (itemMax.y > groupMax.y) groupMax.y = itemMax.y;
			// A thin divider between neighbouring segments.
			if (column != 0) {
				drawList->AddLine(ImVec2(itemMin.x, itemMin.y + 3.f), ImVec2(itemMin.x, itemMax.y - 3.f),
					OFS_Sashimi::Grey40, 1.f);
			}

			if (tips != nullptr && tips[i] != nullptr) Tooltip(tips[i]);
		}
		ImGui::PopStyleVar(3);
		drawList->AddRect(groupMin, groupMax, OFS_Sashimi::Grey50, style.FrameRounding, 0, 1.f);
		ImGui::PopID();
		return clicked;
	}

	// A number to type in, with a step down on its left and a step up on its
	// right: [-50] [ 300 ] [+50]. For a parameter, where a slider hid the
	// exact value and was fiddly to land on a round one. The label, when given,
	// sits above it, dimmed, naming the value and its unit. Fills the width
	// left on the line. Returns true when the value changed.
	inline bool StepperFloat(const char* label, const char* id, float* value, float step,
		float minValue, float maxValue, const char* format = "%.2f") noexcept
	{
		const auto& style = ImGui::GetStyle();
		if (label != nullptr) ImGui::TextDisabled("%s", label);
		ImGui::PushID(id);
		char minus[32];
		char plus[32];
		stbsp_snprintf(minus, sizeof(minus), "-%g", step);
		stbsp_snprintf(plus, sizeof(plus), "+%g", step);
		const float total = ImGui::GetContentRegionAvail().x;
		const float buttonWidth = Util::Max(ImGui::CalcTextSize(minus).x, ImGui::CalcTextSize(plus).x)
			+ (style.FramePadding.x * 2.f);
		const float gap = style.ItemInnerSpacing.x;
		bool changed = false;
		if (ImGui::Button(minus, ImVec2(buttonWidth, 0.f))) { *value -= step; changed = true; }
		ImGui::SameLine(0.f, gap);
		ImGui::SetNextItemWidth(Util::Max(ImGui::GetFontSize() * 2.f, total - (buttonWidth * 2.f) - (gap * 2.f)));
		if (ImGui::InputFloat("##value", value, 0.f, 0.f, format)) changed = true;
		ImGui::SameLine(0.f, gap);
		if (ImGui::Button(plus, ImVec2(buttonWidth, 0.f))) { *value += step; changed = true; }
		ImGui::PopID();
		if (changed) *value = Util::Clamp(*value, minValue, maxValue);
		return changed;
	}

	// The same for a whole number.
	inline bool StepperInt(const char* label, const char* id, int32_t* value, int32_t step,
		int32_t minValue, int32_t maxValue) noexcept
	{
		const auto& style = ImGui::GetStyle();
		if (label != nullptr) ImGui::TextDisabled("%s", label);
		ImGui::PushID(id);
		char minus[32];
		char plus[32];
		stbsp_snprintf(minus, sizeof(minus), "-%d", step);
		stbsp_snprintf(plus, sizeof(plus), "+%d", step);
		const float total = ImGui::GetContentRegionAvail().x;
		const float buttonWidth = Util::Max(ImGui::CalcTextSize(minus).x, ImGui::CalcTextSize(plus).x)
			+ (style.FramePadding.x * 2.f);
		const float gap = style.ItemInnerSpacing.x;
		bool changed = false;
		if (ImGui::Button(minus, ImVec2(buttonWidth, 0.f))) { *value -= step; changed = true; }
		ImGui::SameLine(0.f, gap);
		ImGui::SetNextItemWidth(Util::Max(ImGui::GetFontSize() * 2.f, total - (buttonWidth * 2.f) - (gap * 2.f)));
		if (ImGui::InputInt("##value", value, 0, 0)) changed = true;
		ImGui::SameLine(0.f, gap);
		if (ImGui::Button(plus, ImVec2(buttonWidth, 0.f))) { *value += step; changed = true; }
		ImGui::PopID();
		if (changed) *value = Util::Clamp(*value, minValue, maxValue);
		return changed;
	}

	// A length in hours, minutes and seconds, as three boxes reading left to
	// right the way the times everywhere else in the app are written. A single
	// seconds box would make the caller do the arithmetic that the format is
	// there to save them.
	inline bool InputDuration(const char* id, int32_t* seconds,
		int32_t minSeconds, int32_t maxSeconds) noexcept
	{
		const auto& style = ImGui::GetStyle();
		int32_t h = *seconds / 3600;
		int32_t m = (*seconds % 3600) / 60;
		int32_t sec = *seconds % 60;

		// Wide enough for two digits and the frame around them; the hours box
		// gets the same width so the three read as one field.
		const float boxWidth = ImGui::CalcTextSize("0000").x + (style.FramePadding.x * 2.f);
		const float gap = style.ItemInnerSpacing.x;

		bool changed = false;
		ImGui::PushID(id);
		ImGui::SetNextItemWidth(boxWidth);
		if (ImGui::InputInt("##h", &h, 0, 0)) changed = true;
		ImGui::SameLine(0.f, gap);
		ImGui::TextDisabled("h");
		ImGui::SameLine(0.f, gap);
		ImGui::SetNextItemWidth(boxWidth);
		if (ImGui::InputInt("##m", &m, 0, 0)) changed = true;
		ImGui::SameLine(0.f, gap);
		ImGui::TextDisabled("m");
		ImGui::SameLine(0.f, gap);
		ImGui::SetNextItemWidth(boxWidth);
		if (ImGui::InputInt("##s", &sec, 0, 0)) changed = true;
		ImGui::SameLine(0.f, gap);
		ImGui::TextDisabled("s");
		ImGui::PopID();

		if (changed) {
			// Each box is taken as it was typed and the total is clamped once,
			// so typing 90 into minutes means an hour and a half rather than
			// being rejected.
			int64_t total = (int64_t)Util::Max(h, 0) * 3600
				+ (int64_t)Util::Max(m, 0) * 60
				+ (int64_t)Util::Max(sec, 0);
			*seconds = (int32_t)Util::Clamp<int64_t>(total, minSeconds, maxSeconds);
		}
		return changed;
	}
}

struct OFS_ImGui
{
	// This can be used during rendering callbacks to get the current viewport
	static ImGuiViewport* CurrentlyRenderedViewport;
};
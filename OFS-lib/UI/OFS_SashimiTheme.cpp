#include "OFS_SashimiTheme.h"

namespace OFS_Sashimi
{

// Collapse a colour onto the neutral grey axis using perceptual luminance.
// Run over the whole palette first so that any ImGuiCol_ this file doesn't
// name explicitly (added by a newer imgui) still ends up monochrome.
static void desaturate(ImVec4& c) noexcept
{
    const float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    c.x = c.y = c.z = lum;
}

void ApplyStyle(ImGuiStyle& style) noexcept
{
    // Start from Dark so spacing/rounding defaults and any unnamed colour
    // slots have sane values, then strip all colour out of it.
    ImGui::StyleColorsDark(&style);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        desaturate(style.Colors[i]);
    }

    ImVec4* c = style.Colors;

    // ------------------------------------------------------------ surfaces
    c[ImGuiCol_Text]                  = V4(Grey95);
    c[ImGuiCol_TextDisabled]          = V4(Grey60);
    c[ImGuiCol_WindowBg]              = V4(Grey05);
    c[ImGuiCol_ChildBg]               = V4(Grey10, 0.f);
    // Menus, tooltips and combo lists are the surfaces that float over other
    // things, so they are the ones that get a little translucency: what is
    // underneath shows through faintly, which is as close to frosted glass as
    // a plain draw list gets. Borders are a hairline of light rather than a
    // darker grey, the way a sheet of glass catches the light along its edge.
    c[ImGuiCol_PopupBg]               = V4(Grey15, 0.90f);
    c[ImGuiCol_Border]                = V4(Grey95, 0.10f);
    c[ImGuiCol_BorderShadow]          = V4(Grey00, 0.f);
    c[ImGuiCol_MenuBarBg]             = V4(Grey10);

    // Two steps above anything a frame sits on. At Grey15 a frame matched the
    // popup background exactly, so an unticked checkbox or radio button in a
    // modal such as Preferences was not drawn at all, and on a window it was
    // barely there.
    c[ImGuiCol_FrameBg]               = V4(Grey25);
    c[ImGuiCol_FrameBgHovered]        = V4(Grey30);
    c[ImGuiCol_FrameBgActive]         = V4(Grey40);

    c[ImGuiCol_TitleBg]               = V4(Grey05);
    c[ImGuiCol_TitleBgCollapsed]      = V4(Grey05, 0.75f);
    // The focused window's bar is tinted rather than painted: still obvious
    // at a glance in a docked layout, without a solid block of colour.
    c[ImGuiCol_TitleBgActive]         = V4(PinkDeep, 0.55f);

    c[ImGuiCol_ScrollbarBg]           = V4(Grey05, 0.6f);
    c[ImGuiCol_ScrollbarGrab]         = V4(Grey30);
    c[ImGuiCol_ScrollbarGrabHovered]  = V4(Grey40);
    c[ImGuiCol_ScrollbarGrabActive]   = V4(Pink);

    // ------------------------------------------------------------- accents
    // Controls sit grey at rest and turn pink as you touch them.
    c[ImGuiCol_CheckMark]             = V4(Pink);
    c[ImGuiCol_SliderGrab]            = V4(Pink);
    c[ImGuiCol_SliderGrabActive]      = V4(PinkBright);

    c[ImGuiCol_Button]                = V4(Grey20);
    c[ImGuiCol_ButtonHovered]         = V4(Grey30);
    c[ImGuiCol_ButtonActive]          = V4(Pink);

    c[ImGuiCol_Header]                = V4(PinkFill);
    c[ImGuiCol_HeaderHovered]         = V4(PinkFillHi);
    c[ImGuiCol_HeaderActive]          = V4(Pink);

    c[ImGuiCol_Separator]             = V4(Grey30);
    c[ImGuiCol_SeparatorHovered]      = V4(Pink, 0.78f);
    c[ImGuiCol_SeparatorActive]       = V4(PinkBright);

    c[ImGuiCol_ResizeGrip]            = V4(Grey30, 0.5f);
    c[ImGuiCol_ResizeGripHovered]     = V4(Pink, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = V4(PinkBright);

    // ---------------------------------------------------------------- tabs
    c[ImGuiCol_Tab]                   = V4(Grey15);
    c[ImGuiCol_TabHovered]            = V4(PinkFillHi);
    c[ImGuiCol_TabActive]             = V4(PinkDeep);
    c[ImGuiCol_TabUnfocused]          = V4(Grey10);
    c[ImGuiCol_TabUnfocusedActive]    = V4(Grey20);

    c[ImGuiCol_DockingPreview]        = V4(Pink, 0.55f);
    c[ImGuiCol_DockingEmptyBg]        = V4(Grey00);

    // --------------------------------------------------------------- plots
    c[ImGuiCol_PlotLines]             = V4(Grey80);
    c[ImGuiCol_PlotLinesHovered]      = V4(PinkBright);
    c[ImGuiCol_PlotHistogram]         = V4(Pink);
    c[ImGuiCol_PlotHistogramHovered]  = V4(PinkBright);

    // -------------------------------------------------------------- tables
    c[ImGuiCol_TableHeaderBg]         = V4(Grey20);
    c[ImGuiCol_TableBorderStrong]     = V4(Grey30);
    c[ImGuiCol_TableBorderLight]      = V4(Grey20);
    c[ImGuiCol_TableRowBg]            = V4(Grey00, 0.f);
    c[ImGuiCol_TableRowBgAlt]         = V4(Grey95, 0.035f);

    // ---------------------------------------------------------- selection
    c[ImGuiCol_TextSelectedBg]        = V4(Pink, 0.4f);
    c[ImGuiCol_DragDropTarget]        = V4(PinkBright);
    c[ImGuiCol_NavHighlight]          = V4(Pink);
    c[ImGuiCol_NavWindowingHighlight] = V4(PinkPale, 0.7f);
    c[ImGuiCol_NavWindowingDimBg]     = V4(Grey00, 0.6f);
    c[ImGuiCol_ModalWindowDimBg]      = V4(Grey00, 0.65f);

    // ----------------------------------------------------------- geometry
    // One radius family throughout: windows and popups at the large size,
    // frames and grabs at the small one, so every corner in the app matches.
    // A little more air in the padding, since the flat greys read as cramped
    // with stock spacing.
    style.WindowRounding    = 10.f;
    style.ChildRounding     = 8.f;
    style.PopupRounding     = 10.f;
    style.FrameRounding     = 6.f;
    style.ScrollbarRounding = 8.f;
    style.GrabRounding      = 6.f;
    style.TabRounding       = 6.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 0.f;
    style.PopupBorderSize   = 1.f;
    style.WindowPadding     = ImVec2(10.f, 10.f);
    style.FramePadding      = ImVec2(8.f, 4.f);
    style.ItemSpacing       = ImVec2(8.f, 6.f);
    style.ItemInnerSpacing  = ImVec2(6.f, 4.f);
    style.ScrollbarSize     = 12.f;
    style.GrabMinSize       = 10.f;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
}

}

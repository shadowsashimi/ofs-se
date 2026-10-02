#include "ScriptPositionsOverlayMode.h"
#include "OFS_ScriptTimeline.h"
#include "OFS_Profiling.h"
#include "OFS_Localization.h"
#include "FunscriptHeatmap.h"
#include "OFS_SashimiTheme.h"

#include "state/states/BaseOverlayState.h"

#include <cmath>

std::vector<BaseOverlay::ColoredLine> BaseOverlay::ColoredLines;

constexpr float MaxPointSize = 8.f;
float BaseOverlay::PointSize = MaxPointSize;

uint32_t BaseOverlay::StateHandle = 0xFFFF'FFFF;

bool BaseOverlay::ShowLines = true;
bool BaseOverlay::ShowPoints = true;

static constexpr auto SelectedLineColor = OFS_Sashimi::PinkBright;

BaseOverlay::BaseOverlay(ScriptTimeline* timeline) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    StateHandle = BaseOverlayState::RegisterStatic();
    this->timeline = timeline;
}

void BaseOverlay::update() noexcept
{
    OFS_PROFILE(__FUNCTION__);
}

void BaseOverlay::DrawSettings() noexcept
{

}

float BaseOverlay::logicalFrameTime(float realFrameTime) noexcept
{
    return realFrameTime;
}

void EmptyOverlay::DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    timeline->DrawAudioWaveform(ctx);
    BaseOverlay::DrawActionLines(ctx);
    BaseOverlay::DrawActionPoints(ctx);
}

float EmptyOverlay::steppingIntervalForward(float realFrameTime, float fromTime) noexcept
{
    return realFrameTime;
}

float EmptyOverlay::steppingIntervalBackward(float realFrameTime, float fromTime) noexcept
{
    return -realFrameTime;
}

inline static void getActionLineColor(
    ImColor* speedColor, 
    const ImGradient& speedGradient,
    FunscriptAction action,
    FunscriptAction prevAction,
    const BaseOverlayState& overlay) noexcept
{
    float speed = std::abs(action.pos - prevAction.pos) / ((action.atS - prevAction.atS));
    if(overlay.ShowMaxSpeedHighlight && speed >= overlay.MaxSpeedPerSecond) {
        *speedColor = overlay.MaxSpeedColor;
        return;
    }
    float relSpeed = Util::Clamp<float>(speed / FunscriptHeatmap::MaxSpeedPerSecond, 0.f, 1.f);
    speedGradient.getColorAt(relSpeed, &speedColor->Value.x);
    speedColor->Value.w = 1.f;
}

ImVec2 BaseOverlay::GetPointForAction(const OverlayDrawingCtx& ctx, FunscriptAction action) noexcept
{
    float relative_x = (float)(action.atS - ctx.offsetTime) / ctx.visibleTime;
    float x = (ctx.canvasSize.x) * relative_x;
    float y = (ctx.canvasSize.y) * (1 - (action.pos / 100.f));
    x += ctx.canvasPos.x;
    y += ctx.canvasPos.y;
    return ImVec2(x, y);
};

void BaseOverlay::drawActionLinesSpline(const OverlayDrawingCtx& ctx, const BaseOverlayState& state) noexcept
{
    auto drawSpline = [](const OverlayDrawingCtx& ctx, FunscriptAction startAction, FunscriptAction endAction, uint32_t color, float width, bool background = true) noexcept
    {
        constexpr float SamplesPerTwothousandPixels = 150.f;
        const float MaximumSamples = SamplesPerTwothousandPixels * (ctx.canvasSize.x / 2000.f);

        auto getPointForTimePos = [](const OverlayDrawingCtx& ctx, float time, float pos) noexcept {
            float relative_x = (float)(time - ctx.offsetTime) / ctx.visibleTime;
            float x = (ctx.canvasSize.x) * relative_x;
            float y = (ctx.canvasSize.y) * (1 - (pos / 100.f));
            x += ctx.canvasPos.x;
            y += ctx.canvasPos.y;
            return ImVec2(x, y);
        };
        auto putPoint = [getPointForTimePos](auto& ctx, float time) noexcept {
            float pos = Util::Clamp<float>(ctx.DrawingScript()->Spline(time) * 100.f, 0.f, 100.f);
            ctx.drawList->PathLineTo(getPointForTimePos(ctx, time, pos));
        };

        ctx.drawList->PathClear();
        float visibleDuration;
        float currentTime;
        float endTime;
        if (startAction.atS >= ctx.offsetTime && endAction.atS <= (ctx.offsetTime + ctx.visibleTime)) {
            currentTime = startAction.atS;
            endTime = endAction.atS;
            visibleDuration = endTime - currentTime;
        }
        else if (startAction.atS < ctx.offsetTime && endAction.atS > (ctx.offsetTime + ctx.visibleTime)) {
            // clip at the invisible area in both direction
            currentTime = ctx.offsetTime;
            endTime = (ctx.offsetTime + ctx.visibleTime);
            visibleDuration = ctx.visibleTime;
        }
        else if (startAction.atS < ctx.offsetTime) {
            // clip invisible area on the left
            currentTime = ctx.offsetTime;
            endTime = endAction.atS;
            visibleDuration = endAction.atS - ctx.offsetTime;
        }
        else if (endAction.atS > (ctx.offsetTime + ctx.visibleTime)) {
            // clip invisble area on the right
            currentTime = startAction.atS;
            endTime = (ctx.offsetTime + ctx.visibleTime) + 0.001f;
            visibleDuration = endTime - startAction.atS;
        }

        // detail gets dynamically reduced by increasing the timeStep,
        // at which is being sampled from the spline
        const float ratio = visibleDuration / ctx.visibleTime;
        const float SampleCount = MaximumSamples * ratio;

        const float timeStep = visibleDuration / SampleCount;
        
        if (SampleCount < 3.f) {
            auto p1 = BaseOverlay::GetPointForAction(ctx, startAction);
            auto p2 = BaseOverlay::GetPointForAction(ctx, endAction);
            if (background) {
                ctx.drawList->PathLineTo(p1);
                ctx.drawList->PathLineTo(p2);
                ctx.drawList->PathStroke(IM_COL32_BLACK, false, 7.f);
            }
            ColoredLines.emplace_back(std::move(BaseOverlay::ColoredLine{ p1, p2, color }));
        }
        else {
            putPoint(ctx, currentTime);
            currentTime += timeStep;
            while (currentTime < endTime) {
                putPoint(ctx, currentTime);
                currentTime += timeStep;
            }
            putPoint(ctx, endAction.atS);
            auto tmpSize = ctx.drawList->_Path.Size;
            ctx.drawList->PathStroke(IM_COL32_BLACK, false, 7.f);
            ctx.drawList->_Path.Size = tmpSize;
            ctx.drawList->PathStroke(color, false, width);
        }
    };

    auto& drawingScript = ctx.DrawingScript();
    {
        auto startIt = drawingScript->Actions().begin() + ctx.actionFromIdx;
        auto endIt = drawingScript->Actions().begin() + ctx.actionToIdx;

        // The selection is sorted the same way as the action list, so walking a
        // parallel iterator is enough to know whether an action is selected.
        // Doing it in one pass avoids drawing selected segments twice, and means
        // a highlight can no longer span actions that are not themselves
        // selected, which the old second pass would happily do.
        auto selectionIt = drawingScript->Selection().begin() + ctx.selectionFromIdx;
        auto selectionEndIt = drawingScript->Selection().begin() + ctx.selectionToIdx;

        bool prevActionSelected = false;
        const FunscriptAction* prevAction = nullptr;
        while (startIt != endIt) {
            auto& action = *startIt;

            // Both ranges are sorted by time but their windows are derived
            // independently, so the selection iterator can start behind the
            // action window. Skip past anything earlier or the two never
            // resynchronise and later selected actions stop highlighting.
            while (selectionIt != selectionEndIt && *selectionIt < action) ++selectionIt;
            const bool actionSelected = selectionIt != selectionEndIt && *selectionIt == action;

            if (prevAction != nullptr) {
                uint32_t color;
                if (actionSelected && prevActionSelected) {
                    color = SelectedLineColor;
                }
                else {
                    ImColor speedColor;
                    getActionLineColor(&speedColor, FunscriptHeatmap::LineColors, action, *prevAction, state);
                    color = ImGui::ColorConvertFloat4ToU32(speedColor);
                }
                drawSpline(ctx, *prevAction, action, color, 3.f);
            }

            prevAction = &action;
            prevActionSelected = actionSelected;

            ++startIt;
        }
    }
}

void BaseOverlay::drawActionLinesLinear(const OverlayDrawingCtx& ctx, const BaseOverlayState& state) noexcept
{
    auto drawLine = [](const OverlayDrawingCtx& ctx, ImVec2 p1, ImVec2 p2, uint32_t color) noexcept
    {
        ctx.drawList->AddLine(p1, p2, IM_COL32(0, 0, 0, 255), 7.0f); // border
        ColoredLines.emplace_back(std::move(BaseOverlay::ColoredLine{ p1, p2, color }));
    };

    auto& drawingScript = ctx.DrawingScript();
    {
        auto startIt = drawingScript->Actions().begin() + ctx.actionFromIdx;
        auto endIt = drawingScript->Actions().begin() + ctx.actionToIdx;

        // Single pass, see the note in drawActionLinesSpline.
        auto selectionIt = drawingScript->Selection().begin() + ctx.selectionFromIdx;
        auto selectionEndIt = drawingScript->Selection().begin() + ctx.selectionToIdx;

        bool prevActionSelected = false;
        const FunscriptAction* prevAction = nullptr;
        ImVec2 prevPoint;
        while (startIt != endIt) {
            auto& action = *startIt;
            auto point = BaseOverlay::GetPointForAction(ctx, action);

            // Both ranges are sorted by time but their windows are derived
            // independently, so the selection iterator can start behind the
            // action window. Skip past anything earlier or the two never
            // resynchronise and later selected actions stop highlighting.
            while (selectionIt != selectionEndIt && *selectionIt < action) ++selectionIt;
            const bool actionSelected = selectionIt != selectionEndIt && *selectionIt == action;

            if (prevAction != nullptr) {
                uint32_t color;
                if (actionSelected && prevActionSelected) {
                    color = SelectedLineColor;
                }
                else {
                    ImColor speedColor;
                    getActionLineColor(&speedColor, FunscriptHeatmap::LineColors, action, *prevAction, state);
                    color = ImGui::ColorConvertFloat4ToU32(speedColor);
                }
                drawLine(ctx, point, prevPoint, color);
            }

            prevAction = &action;
            prevPoint = point;
            prevActionSelected = actionSelected;

            ++startIt;
        }
    }
}

void BaseOverlay::DrawActionLines(const OverlayDrawingCtx& ctx) noexcept
{
    if (!BaseOverlay::ShowLines) return;
    OFS_PROFILE(__FUNCTION__);
    auto& drawingScript = ctx.DrawingScript();
    auto& state = BaseOverlayState::State(StateHandle);

    auto startIt = drawingScript->Actions().begin() + ctx.actionFromIdx;
    auto endIt = drawingScript->Actions().begin() + ctx.actionToIdx;
    ColoredLines.clear();
    
    if(state.SplineMode)
    {
        drawActionLinesSpline(ctx, state);
    }
    else 
    {
        drawActionLinesLinear(ctx, state);
    }

    // this is so that the black background line gets rendered first
    for (auto&& line : ColoredLines) {
        ctx.drawList->AddLine(line.p1, line.p2, line.color, 3.f);
    }
}

void BaseOverlay::DrawActionPoints(const OverlayDrawingCtx& ctx) noexcept
{
    if (!BaseOverlay::ShowPoints) return;
    OFS_PROFILE(__FUNCTION__);
	auto applyEasing = [](float t) noexcept -> float {
		return t * t; 
	};

    float opacity = 1.f;

    if(BaseOverlay::ShowLines) 
    {
        opacity = 20.f / ctx.visibleTime;
        opacity = Util::Clamp(opacity, 0.f, 1.f);
        BaseOverlay::PointSize = MaxPointSize * opacity;
        opacity = applyEasing(opacity);
    }

    if(opacity >= 0.25f) 
    {
        auto& drawingScript = ctx.DrawingScript();
        const int opacityInt = 255 * opacity;
        const auto borderColor = IM_COL32(0, 0, 0, opacityInt);
        const auto pointColor = IM_COL32(0xE6, 0xE6, 0xE6, opacityInt);
        const auto selectedColor = IM_COL32(0xFF, 0x70, 0xA2, opacityInt);

        // One pass, walking the selection alongside the actions the same way
        // the lines are drawn. A selected point used to be drawn grey with the
        // rest and then again in pink on top, which with a large selection
        // doubled the circles submitted for nothing.
        auto startIt = drawingScript->Actions().begin() + ctx.actionFromIdx;
        auto endIt = drawingScript->Actions().begin() + ctx.actionToIdx;
        auto selectionIt = drawingScript->Selection().begin() + ctx.selectionFromIdx;
        auto selectionEndIt = drawingScript->Selection().begin() + ctx.selectionToIdx;
        for (; startIt != endIt; ++startIt)
        {
            while (selectionIt != selectionEndIt && *selectionIt < *startIt) ++selectionIt;
            const bool selected = selectionIt != selectionEndIt && *selectionIt == *startIt;

            auto p = BaseOverlay::GetPointForAction(ctx, *startIt);
            ctx.drawList->AddCircleFilled(p, BaseOverlay::PointSize, borderColor, 4);
            ctx.drawList->AddCircleFilled(p, BaseOverlay::PointSize * 0.7f, selected ? selectedColor : pointColor, 4);
        }
    }
}

void BaseOverlay::DrawSecondsLabel(const OverlayDrawingCtx& ctx) noexcept
{
    auto& style = ImGui::GetStyle();
    if (ctx.drawingScriptIdx == ctx.drawnScriptCount - 1) {
        OFS_PROFILE(__FUNCTION__);
        auto tmp = FMT("%.2f %s", ctx.visibleTime, TR(TIMELINE_SECONDS));
        auto textSize = ImGui::CalcTextSize(tmp);
        // Bottom right. Bottom left, with several lanes each only a little
        // taller than a line of text, it ran into the last lane's name tag,
        // which sits at the lane's left edge.
        ctx.drawList->AddText(
            ctx.canvasPos + ImVec2(ctx.canvasSize.x - textSize.x - style.FramePadding.x, ctx.canvasSize.y - textSize.y - style.FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_Text),
            tmp
        );
    }
}

void BaseOverlay::DrawHeightLines(const OverlayDrawingCtx& ctx) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // height indicators
    for (int i = 0; i < 9; i += 1) {
        auto color = (i == 4) ? IM_COL32(150, 150, 150, 255) : IM_COL32(80, 80, 80, 255);
        auto thickness = (i == 4) ? 2.f : 1.0f;
        ctx.drawList->AddLine(
            ctx.canvasPos + ImVec2(0.0, (ctx.canvasSize.y / 10.f) * (i + 1)),
            ctx.canvasPos + ImVec2(ctx.canvasSize.x, (ctx.canvasSize.y / 10.f) * (i + 1)),
            color,
            thickness
        );
    }
}


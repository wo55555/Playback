#include "TimelinePanel.h"

#include "playback/editor/ui/FormatUtils.h"
#include "playback/editor/ui/ReplayUILayout.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace playback::editor::ui {

TimelineScale chooseTimelineScale(int totalTicks, float timelineWidth) {
    float const targetTicksPerMajor =
        static_cast<float>(std::max(1, totalTicks)) * 60.0f / std::max(1.0f, timelineWidth);
    if (targetTicksPerMajor < 5.0f) return {1, 5, true};
    if (targetTicksPerMajor < 8.0f) return {2, 5, true};
    return {std::max(5, static_cast<int>(std::ceil(targetTicksPerMajor / 20.0f)) * 5), 4, false};
}

void drawSkipControl(ImDrawList& drawList, ImVec2 center, float size, bool forwards, ImU32 color) {
    ImVec2 const min(center.x - size * 0.5f, center.y - size * 0.5f);
    if (forwards) {
        drawList.AddTriangleFilled(
            ImVec2(min.x, min.y),
            ImVec2(min.x + size * 0.67f, center.y),
            ImVec2(min.x, min.y + size),
            color
        );
        drawList.AddRectFilled(ImVec2(min.x + size * 0.67f, min.y), ImVec2(min.x + size, min.y + size), color);
    } else {
        drawList.AddRectFilled(ImVec2(min.x, min.y), ImVec2(min.x + size * 0.33f, min.y + size), color);
        drawList.AddTriangleFilled(
            ImVec2(min.x + size * 0.33f, center.y),
            ImVec2(min.x + size, min.y),
            ImVec2(min.x + size, min.y + size),
            color
        );
    }
}

void drawRateControl(ImDrawList& drawList, ImVec2 center, float size, bool forwards, ImU32 color) {
    float const x = center.x - size * 0.5f;
    float const y = center.y - size * 0.5f;
    for (int half = 0; half < 2; ++half) {
        float const start = x + size * 0.5f * static_cast<float>(half);
        if (forwards) {
            drawList.AddTriangleFilled(
                ImVec2(start, y),
                ImVec2(start + size * 0.5f, center.y),
                ImVec2(start, y + size),
                color
            );
        } else {
            drawList.AddTriangleFilled(
                ImVec2(start, center.y),
                ImVec2(start + size * 0.5f, y),
                ImVec2(start + size * 0.5f, y + size),
                color
            );
        }
    }
}

void drawPlaybackAction(ImDrawList& drawList, ImVec2 center, float size, bool paused, ImU32 color) {
    float const x = center.x - size * 0.5f;
    float const y = center.y - size * 0.5f;
    if (paused) {
        drawList.AddTriangleFilled(
            ImVec2(x + size / 12.0f, y),
            ImVec2(x + size, center.y),
            ImVec2(x + size / 12.0f, y + size),
            color
        );
    } else {
        drawList.AddRectFilled(ImVec2(x, y), ImVec2(x + size / 3.0f, y + size), color);
        drawList.AddRectFilled(ImVec2(x + size * 2.0f / 3.0f, y), ImVec2(x + size, y + size), color);
    }
}

void drawCenteredFittedText(
    ImDrawList& drawList,
    float       x,
    float       y,
    float       availableWidth,
    ImU32       color,
    char const* text
) {
    float const baseFontSize = ImGui::GetFontSize();
    float const baseWidth    = ImGui::CalcTextSize(text).x;
    float const fontSize =
        baseWidth > availableWidth ? std::max(9.0f, baseFontSize * availableWidth / baseWidth) : baseFontSize;
    float const width = baseWidth * fontSize / baseFontSize;
    drawList.AddText(ImGui::GetFont(), fontSize, ImVec2(x + (availableWidth - width) * 0.5f, y), color, text);
}

void drawTimelinePanel(EditorState const& state, ReplayUILayout const& layout, std::vector<EditorAction>& actions) {
    using ll::i18n_literals::operator""_tr;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 320.0f || io.DisplaySize.y < 180.0f) return;

    if (layout.timelineHeight < 1.0f) return;

    float const  scale = layout.scale;
    ImVec2 const windowPos(0.0f, layout.gameViewportBottom);
    ImVec2 const windowSize(io.DisplaySize.x, layout.timelineHeight);

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar
                                     | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    bool const visible = ImGui::Begin("##PlaybackReplayTimeline", nullptr, flags);
    if (visible) {
        ImDrawList&  drawList = *ImGui::GetWindowDrawList();
        ImVec2 const origin   = ImGui::GetWindowPos();
        ImVec2 const size     = ImGui::GetWindowSize();

        ImU32 const textColor         = IM_COL32(240, 240, 248, 255);
        ImU32 const disabledColor     = IM_COL32(160, 160, 170, 110);
        ImU32 const separatorColor    = IM_COL32(96, 96, 108, 255);
        ImU32 const subSecondColor    = IM_COL32(128, 128, 140, 255);
        ImU32 const timelineBodyColor = IM_COL32(22, 22, 26, 255);
        ImU32 const progressBgColor   = IM_COL32(42, 42, 48, 255);
        ImU32 const progressFillColor = IM_COL32(70, 130, 210, 210);
        ImU32 const playheadColor     = IM_COL32(255, 255, 255, 255);
        ImU32 const accentLineColor   = IM_COL32(70, 130, 210, 255);

        float const leftWidth     = std::min(240.0f * scale, std::max(160.0f * scale, size.x * 0.42f));
        float const rulerTop      = origin.y + 4.0f * scale;
        float const rulerBottom   = origin.y + 40.0f * scale;
        float const dividerX      = origin.x + leftWidth;
        float const zoomHeight    = std::min(12.0f * scale, std::max(4.0f, size.y * 0.08f));
        float const bodyBottom    = origin.y + size.y - zoomHeight;
        float const trackTop      = rulerBottom + 8.0f * scale;
        float const trackBottom   = std::min(trackTop + 26.0f * scale, bodyBottom - 8.0f * scale);
        float const timelineLeft  = dividerX + 6.0f * scale;
        float const timelineRight = origin.x + size.x - 6.0f * scale;
        float const timelineWidth = std::max(1.0f, timelineRight - timelineLeft);
        int const   totalTicks    = std::max(1, state.totalTicks);

        static bool scrubbing{};
        static int  scrubTick{};
        static int  committedTick{-1};
        ImGui::SetCursorScreenPos(ImVec2(timelineLeft, origin.y));
        ImGui::InvisibleButton("##PlaybackTimelineScrub", ImVec2(timelineWidth, std::max(1.0f, bodyBottom - origin.y)));
        bool const scrubActive  = ImGui::IsItemActive();
        bool const scrubHovered = ImGui::IsItemHovered();
        if (scrubActive) {
            float const ratio  = std::clamp((io.MousePos.x - timelineLeft) / timelineWidth, 0.0f, 1.0f);
            scrubTick          = std::clamp(static_cast<int>(std::lround(ratio * totalTicks)), 0, state.totalTicks);
            auto const tooltip = "playback.editor.timeline.scrub"_tr(utils::formatTimestamp(scrubTick), scrubTick);
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        if (committedTick >= 0 && state.currentTick == committedTick) committedTick = -1;
        if (scrubbing && !scrubActive) {
            committedTick = scrubTick;
            actions.push_back({EditorActionType::Seek, scrubTick});
        }
        scrubbing           = scrubActive;
        int const shownTick = scrubActive ? scrubTick : (committedTick >= 0 ? committedTick : state.currentTick);

        drawList.AddLine(ImVec2(dividerX, origin.y), ImVec2(dividerX, origin.y + size.y), separatorColor);
        drawList.AddLine(ImVec2(timelineLeft, rulerBottom), ImVec2(timelineRight, rulerBottom), separatorColor);
        drawList.AddLine(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y), accentLineColor);
        drawList.AddRectFilled(ImVec2(dividerX, rulerBottom), ImVec2(origin.x + size.x, bodyBottom), timelineBodyColor);

        float const  controlSize = 24.0f * scale;
        ImVec2 const controlCenter(origin.x + leftWidth * 0.5f, origin.y + 20.0f * scale);
        auto         controlButton = [&](char const* id, ImVec2 center, char const* tooltip) {
            ImGui::SetCursorScreenPos(ImVec2(center.x - controlSize * 0.5f, center.y - controlSize * 0.5f));
            ImGui::InvisibleButton(id, ImVec2(controlSize, controlSize));
            bool const hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetTooltip("%s", tooltip);
            return std::pair{ImGui::IsItemClicked(), hovered};
        };

        if (leftWidth >= 200.0f * scale) {
            ImVec2 const      skipBackCenter(origin.x + leftWidth / 6.0f, controlCenter.y);
            ImVec2 const      slowerCenter(origin.x + leftWidth * 2.0f / 6.0f, controlCenter.y);
            ImVec2 const      fasterCenter(origin.x + leftWidth * 4.0f / 6.0f, controlCenter.y);
            ImVec2 const      skipForwardCenter(origin.x + leftWidth * 5.0f / 6.0f, controlCenter.y);
            std::string const slowerTooltip               = "playback.editor.timeline.slowDown"_tr(state.playbackSpeed);
            std::string const fasterTooltip               = "playback.editor.timeline.speedUp"_tr(state.playbackSpeed);
            auto const [skipBackClicked, skipBackHovered] = controlButton(
                "##PlaybackSkipBack",
                skipBackCenter,
                "playback.editor.timeline.jumpToStart"_tr().c_str()
            );
            auto const [slowerClicked, slowerHovered] =
                controlButton("##PlaybackSlower", slowerCenter, slowerTooltip.c_str());
            auto const [pauseClicked, pauseHovered] = controlButton(
                "##PlaybackPause",
                controlCenter,
                (state.paused ? "playback.editor.timeline.play"_tr() : "playback.editor.timeline.pause"_tr()).c_str()
            );
            auto const [fasterClicked, fasterHovered] =
                controlButton("##PlaybackFaster", fasterCenter, fasterTooltip.c_str());
            auto const [skipForwardClicked, skipForwardHovered] = controlButton(
                "##PlaybackSkipForward",
                skipForwardCenter,
                "playback.editor.timeline.jumpToEnd"_tr().c_str()
            );

            if (skipBackClicked) actions.push_back({EditorActionType::SkipToStart});
            if (slowerClicked) actions.push_back({EditorActionType::DecreaseSpeed});
            if (pauseClicked) actions.push_back({EditorActionType::TogglePause});
            if (fasterClicked) actions.push_back({EditorActionType::IncreaseSpeed});
            if (skipForwardClicked) actions.push_back({EditorActionType::SkipToEnd});

            ImU32 const slowerColor = state.playbackSpeed < 1.0f ? IM_COL32(255, 128, 128, 255) : textColor;
            ImU32 const fasterColor = state.playbackSpeed > 1.0f ? IM_COL32(128, 255, 128, 255) : textColor;
            drawSkipControl(
                drawList,
                skipBackCenter,
                controlSize,
                false,
                skipBackHovered ? IM_COL32(255, 255, 255, 255) : textColor
            );
            drawRateControl(
                drawList,
                slowerCenter,
                controlSize,
                false,
                slowerHovered ? IM_COL32(255, 255, 255, 255) : slowerColor
            );
            drawPlaybackAction(
                drawList,
                controlCenter,
                controlSize,
                state.paused,
                pauseHovered ? IM_COL32(255, 255, 255, 255) : textColor
            );
            drawRateControl(
                drawList,
                fasterCenter,
                controlSize,
                true,
                fasterHovered ? IM_COL32(255, 255, 255, 255) : fasterColor
            );
            drawSkipControl(
                drawList,
                skipForwardCenter,
                controlSize,
                true,
                skipForwardHovered ? IM_COL32(255, 255, 255, 255) : textColor
            );
        } else {
            auto const [pauseClicked, pauseHovered] = controlButton(
                "##PlaybackPause",
                controlCenter,
                (state.paused ? "playback.editor.timeline.play"_tr() : "playback.editor.timeline.pause"_tr()).c_str()
            );
            if (pauseClicked) actions.push_back({EditorActionType::TogglePause});
            drawPlaybackAction(
                drawList,
                controlCenter,
                controlSize,
                state.paused,
                pauseHovered ? IM_COL32(255, 255, 255, 255) : textColor
            );
        }

        std::string const timeText =
            utils::formatTimestamp(shownTick) + " / " + utils::formatTimestamp(state.totalTicks);
        std::string const tickText =
            "playback.editor.timeline.ticks"_tr(std::max(0, shownTick), std::max(0, state.totalTicks));
        drawCenteredFittedText(
            drawList,
            origin.x + 8.0f * scale,
            rulerBottom + 12.0f * scale,
            leftWidth - 16.0f * scale,
            textColor,
            timeText.c_str()
        );
        drawCenteredFittedText(
            drawList,
            origin.x + 8.0f * scale,
            rulerBottom + 16.0f * scale + ImGui::GetFontSize(),
            leftWidth - 16.0f * scale,
            disabledColor,
            tickText.c_str()
        );

        auto const timelineScale = chooseTimelineScale(totalTicks, timelineWidth);

        drawList.PushClipRect(ImVec2(dividerX, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), true);
        for (int tick = 0; tick <= totalTicks;) {
            float const ratio = static_cast<float>(tick) / static_cast<float>(totalTicks);
            float const x     = timelineLeft + timelineWidth * ratio;
            bool const  major = (tick / timelineScale.ticksPerMinor) % timelineScale.minorsPerMajor == 0;
            float const top   = rulerTop + (major ? 0.0f : 10.0f * layout.scale);
            drawList.AddLine(ImVec2(x, top), ImVec2(x, rulerBottom), major ? textColor : separatorColor);

            if (major) {
                std::string const timestamp      = utils::formatTimestamp(tick);
                float const       timestampWidth = ImGui::CalcTextSize(timestamp.c_str()).x;
                float const       timestampX =
                    std::clamp(x - timestampWidth * 0.5f, timelineLeft, timelineRight - timestampWidth);
                drawList.AddText(ImVec2(timestampX, rulerTop - 2.0f), textColor, timestamp.c_str());
                if (timelineScale.showSubSeconds) {
                    std::string const subSecond = "/" + std::to_string(tick % 20);
                    drawList.AddText(
                        ImVec2(std::min(timelineRight, timestampX + timestampWidth), rulerTop - 2.0f),
                        subSecondColor,
                        subSecond.c_str()
                    );
                }
            }

            if (totalTicks - tick < timelineScale.ticksPerMinor) break;
            tick += timelineScale.ticksPerMinor;
        }

        float const progress =
            state.totalTicks > 0
                ? std::clamp(static_cast<float>(shownTick) / static_cast<float>(state.totalTicks), 0.0f, 1.0f)
                : 0.0f;
        if (trackBottom > trackTop) {
            float const rounding = 2.0f * scale;
            drawList.AddRectFilled(
                ImVec2(timelineLeft, trackTop),
                ImVec2(timelineRight, trackBottom),
                scrubHovered ? IM_COL32(48, 48, 56, 255) : progressBgColor,
                rounding
            );
            if (progress > 0.0f) {
                float const fillRight = timelineLeft + timelineWidth * progress;
                drawList.AddRectFilled(
                    ImVec2(timelineLeft, trackTop),
                    ImVec2(fillRight, trackBottom),
                    progressFillColor,
                    rounding
                );
                drawList.AddRectFilled(
                    ImVec2(fillRight - layout.scale, trackTop),
                    ImVec2(fillRight + layout.scale, trackBottom),
                    IM_COL32(150, 200, 255, 230)
                );
            }
            drawList
                .AddRect(ImVec2(timelineLeft, trackTop), ImVec2(timelineRight, trackBottom), separatorColor, rounding);
        }

        float const playheadX = timelineLeft + timelineWidth * progress;
        drawList.AddTriangleFilled(
            ImVec2(playheadX, rulerBottom),
            ImVec2(playheadX - 8.0f * layout.scale, rulerTop + 8.0f * layout.scale),
            ImVec2(playheadX + 8.0f * layout.scale, rulerTop + 8.0f * layout.scale),
            playheadColor
        );
        drawList.AddRectFilled(
            ImVec2(playheadX - 1.5f * layout.scale, rulerBottom),
            ImVec2(playheadX + 1.5f * layout.scale, bodyBottom),
            playheadColor
        );

        float const zoomTop = bodyBottom;
        drawList.AddRectFilled(
            ImVec2(dividerX, zoomTop),
            ImVec2(origin.x + size.x, origin.y + size.y),
            IM_COL32(48, 48, 54, 255)
        );
        drawList.AddRectFilled(
            ImVec2(timelineLeft, zoomTop + zoomHeight * 0.25f),
            ImVec2(timelineRight, origin.y + size.y - zoomHeight * 0.25f),
            IM_COL32(210, 210, 216, 255),
            zoomHeight * 0.5f
        );
        drawList.PopClipRect();
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace playback::editor::ui

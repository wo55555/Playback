#include "RenderMode.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

std::string exportStateLabel(exporting::ExportState state) {
    switch (state) {
    case exporting::ExportState::Preparing:
        return "playback.refactorEditor.export.state.preparing"_tr();
    case exporting::ExportState::Running:
        return "playback.refactorEditor.export.state.running"_tr();
    case exporting::ExportState::Finalizing:
        return "playback.refactorEditor.export.state.finalizing"_tr();
    case exporting::ExportState::Cancelling:
        return "playback.refactorEditor.export.state.cancelling"_tr();
    case exporting::ExportState::Completed:
        return "playback.refactorEditor.export.state.completed"_tr();
    case exporting::ExportState::Cancelled:
        return "playback.refactorEditor.export.state.cancelled"_tr();
    case exporting::ExportState::Faulted:
        return "playback.refactorEditor.export.state.faulted"_tr();
    case exporting::ExportState::Idle:
        return "playback.refactorEditor.export.state.idle"_tr();
    }
    return {};
}

void drawSpinner(ImDrawList* drawList, ImVec2 center, float radius, float thickness, ImU32 color) {
    constexpr float Pi    = 3.14159265358979323846f;
    float const     angle = std::fmod(static_cast<float>(ImGui::GetTime()) * 4.0f, Pi * 2.0f);
    drawList->PathArcTo(center, radius, angle, angle + Pi * 1.55f, 32);
    drawList->PathStroke(color, ImDrawFlags_None, thickness);
}

// h:mm:ss once past an hour, mm:ss below it.
std::string formatDuration(double seconds) {
    auto const total = static_cast<long long>(std::max(0.0, seconds) + 0.5);
    char       value[32]{};
    if (total >= 3600) {
        std::snprintf(value, sizeof(value), "%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    } else {
        std::snprintf(value, sizeof(value), "%02lld:%02lld", total / 60, total % 60);
    }
    return value;
}

std::string pathUtf8(std::filesystem::path const& path) {
    auto const utf8 = path.generic_u8string();
    return {reinterpret_cast<char const*>(utf8.data()), utf8.size()};
}

} // namespace

void RenderMode::reset() {
    mStartedAt       = Clock::now();
    mFirstFrameAt    = {};
    mFirstFrameCount = 0;
    mHasFirstFrame   = false;
}

void RenderMode::draw(PanelContext const& ctx) {
    auto&        editor      = ReplayEditor::getInstance();
    auto const&  status      = ctx.state.exportStatus;
    ImVec2 const displaySize = ImGui::GetIO().DisplaySize;

    auto* const background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled({0.0f, 0.0f}, displaySize, IM_COL32(0, 0, 0, 255));
    if (auto const texture = editor.gameTexture()) {
        background->AddImage(ImTextureRef(texture), {0.0f, 0.0f}, displaySize);
    }

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(displaySize);
    // Only a light dim: the frame being rendered should stay readable behind the dialog.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.10f));
    ImGui::Begin(
        "##ExportModalShield",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoBringToFrontOnFocus
    );
    ImGui::End();
    ImGui::PopStyleColor();

    float const  uiScale = metrics::scale();
    auto const&  style   = ImGui::GetStyle();
    ImVec2 const padding{24.0f * uiScale, 22.0f * uiScale};
    float const  modalWidth = std::max(1.0f, std::min(480.0f * uiScale, displaySize.x - 24.0f));
    ImGui::SetNextWindowPos({displaySize.x * 0.5f, displaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    // Height fits the rows; every row is laid out in every state, so the height never changes.
    ImGui::SetNextWindowSize({modalWidth, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({modalWidth, 0.0f}, {modalWidth, std::max(1.0f, displaySize.y - 24.0f)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * uiScale);
    ImGui::SetNextWindowBgAlpha(0.84f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    bool const progressVisible = ImGui::Begin(
        "##ExportProgress",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
    );
    ImGui::PopStyleVar(2);
    if (progressVisible) {
        // The usual dim grey washes out over a bright scene.
        constexpr ImU32 SecondaryText = IM_COL32(0xb4, 0xb4, 0xb4, 0xff);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, SecondaryText);
        bool const  cancelling = status.state == exporting::ExportState::Cancelling;
        ImU32 const tone       = cancelling ? theme::kWarning : theme::kAccent;

        auto const  capturedFrames = std::min(status.submittedFrames, status.totalFrames);
        auto const  writtenFrames  = std::min(status.writtenFrames, status.totalFrames);
        auto const  progressFrames = std::max(capturedFrames, writtenFrames);
        float const progress =
            status.totalFrames > 0 ? static_cast<float>(progressFrames) / static_cast<float>(status.totalFrames) : 0.0f;

        // Speed is measured from the first delivered frame so warm-up does not drag the estimate down.
        auto const now = Clock::now();
        if (!mHasFirstFrame && progressFrames > 0) {
            mHasFirstFrame   = true;
            mFirstFrameAt    = now;
            mFirstFrameCount = progressFrames;
        }
        double const elapsed = std::chrono::duration<double>(now - mStartedAt).count();
        double       speed   = 0.0;
        if (mHasFirstFrame && progressFrames > mFirstFrameCount) {
            double const span = std::chrono::duration<double>(now - mFirstFrameAt).count();
            if (span >= 1.0) speed = static_cast<double>(progressFrames - mFirstFrameCount) / span;
        }
        bool const   running   = status.state == exporting::ExportState::Running;
        double const remaining = speed > 0.0 ? static_cast<double>(status.totalFrames - progressFrames) / speed : 0.0;

        std::string const format     = status.format == exporting::ExportFormat::Mp4Video
                                         ? "playback.refactorEditor.export.mp4"_tr()
                                         : "playback.refactorEditor.export.pngSequence"_tr();
        std::string       outputPath = pathUtf8(status.outputPath);
        std::string const fileName   = status.outputPath.empty() ? format : pathUtf8(status.outputPath.filename());

        // Header: tinted tile with the activity spinner, state as the title and the output file beneath it.
        {
            float const  box    = metrics::iconButton() * 1.2f;
            ImVec2 const origin = ImGui::GetCursorScreenPos();
            auto*        dl     = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                origin,
                {origin.x + box, origin.y + box},
                theme::withAlpha(tone, 0x40),
                theme::kFrameRounding * 2.0f
            );
            drawSpinner(
                dl,
                {origin.x + box * 0.5f, origin.y + box * 0.5f},
                box * 0.28f,
                2.5f * uiScale,
                cancelling ? theme::kWarning : theme::kAccentHover
            );
            std::string const title = exportStateLabel(status.state);
            float const       textX = origin.x + box + style.ItemSpacing.x * 3.0f;
            float const       lineH = ImGui::GetFontSize();
            float const       gap   = 3.0f * uiScale;
            float const       topY  = origin.y + (box - lineH * 2.0f - gap) * 0.5f;
            float const       right = origin.x + ImGui::GetContentRegionAvail().x;
            dl->AddText({textX, topY}, cancelling ? theme::kWarning : theme::kText, title.c_str());
            dl->PushClipRect({textX, origin.y}, {right, origin.y + box}, true);
            dl->AddText({textX, topY + lineH + gap}, SecondaryText, fileName.c_str());
            dl->PopClipRect();
            ImGui::Dummy({0.0f, box});
        }
        ImGui::Dummy({0.0f, 10.0f * uiScale});

        // Progress bar with the percentage inside and the frame count and estimate on the line below.
        {
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(progress * 100.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, tone);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::withAlpha(theme::kInputBg, 0xb0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding * 2.0f);
            ImGui::ProgressBar(progress, {-FLT_MIN, ImGui::GetFrameHeight()}, percent);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            std::string const frames =
                "playback.refactorEditor.render.frameCount"_tr(progressFrames, status.totalFrames);
            std::string const estimate =
                !running      ? std::string{}
                : speed > 0.0 ? "playback.refactorEditor.render.remainingValue"_tr(formatDuration(remaining))
                              : "playback.refactorEditor.render.estimating"_tr();
            ImGui::TextDisabled("%s", frames.c_str());
            if (!estimate.empty()) {
                float const estimateWidth = ImGui::CalcTextSize(estimate.c_str()).x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - estimateWidth)
                );
                ImGui::TextDisabled("%s", estimate.c_str());
            }
        }
        ImGui::Dummy({0.0f, 8.0f * uiScale});

        float const labelWidth = std::clamp(modalWidth * 0.3f, 90.0f * uiScale, 140.0f * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * uiScale, 4.0f * uiScale});
        if (ImGui::BeginTable(
                "##export-progress-details",
                2,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp
            )) {
            ImGui::TableSetupColumn("##export-progress-label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##export-progress-value", ImGuiTableColumnFlags_WidthStretch);
            auto const row = [](std::string const& label, std::string const& value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", label.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(value.c_str());
            };
            row("playback.refactorEditor.render.capturedLabel"_tr(),
                "playback.refactorEditor.render.frameCount"_tr(capturedFrames, status.totalFrames));
            row("playback.refactorEditor.render.writtenLabel"_tr(),
                "playback.refactorEditor.render.frameCount"_tr(writtenFrames, status.totalFrames));
            row("playback.refactorEditor.render.elapsedLabel"_tr(), formatDuration(elapsed));
            row("playback.refactorEditor.render.speedLabel"_tr(),
                speed > 0.0 ? "playback.refactorEditor.render.speedValue"_tr(speed) : std::string("-"));
            row("playback.refactorEditor.render.formatLabel"_tr(), format);
            // Drawn even before the path is known, so the row count stays fixed.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.render.outputLabel"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::withAlpha(theme::kInputBg, 0xb0));
            ImGui::InputText(
                "##render-output-path",
                outputPath.data(),
                outputPath.size() + 1,
                ImGuiInputTextFlags_ReadOnly
            );
            ImGui::PopStyleColor();
            if (!outputPath.empty()) widgets::itemTooltip(outputPath.c_str());
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // No status.message line: while active it only repeats the title, and it would shift the button.
        ImGui::Dummy({0.0f, 14.0f * uiScale});
        // Pill sized to the icon and label, so it reads as one compact control rather than a wide bar.
        std::string const cancelLabel  = std::string(ICON_CLOSE) + " " + "playback.refactorEditor.render.cancel"_tr();
        float const       buttonHeight = ImGui::GetFontSize() + 16.0f * uiScale;
        float const       buttonWidth  = std::min(
            ImGui::GetContentRegionAvail().x,
            ImGui::CalcTextSize(cancelLabel.c_str()).x + buttonHeight * 1.2f
        );
        float const buttonRounding = buttonHeight * 0.5f;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f)
        );
        if (cancelling) {
            // Same footprint as the button, holding only a spinner while the writer winds down.
            ImVec2 const min = ImGui::GetCursorScreenPos();
            ImVec2 const max{min.x + buttonWidth, min.y + buttonHeight};
            ImGui::Dummy({buttonWidth, buttonHeight});
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(min, max, theme::withAlpha(theme::kButton, 0xcc), buttonRounding);
            drawSpinner(
                dl,
                {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f},
                ImGui::GetFontSize() * 0.45f,
                2.0f * uiScale,
                theme::kWarning
            );
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, buttonRounding);
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kButton);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::kError, 0x80));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::kError, 0xb0));
            if (ImGui::Button(cancelLabel.c_str(), {buttonWidth, buttonHeight})) {
                ctx.submitAction({EditorActionType::CancelExport});
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

} // namespace playback::editor::ui

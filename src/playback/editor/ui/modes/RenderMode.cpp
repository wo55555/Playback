#include "RenderMode.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

void drawSpinner(float radius, float thickness) {
    constexpr float Pi = 3.14159265358979323846f;

    float const size      = radius * 2.0f;
    float const available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available - size) * 0.5f));
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({size, size});

    float const  angle = std::fmod(static_cast<float>(ImGui::GetTime()) * 4.0f, Pi * 2.0f);
    ImVec2 const center{origin.x + radius, origin.y + radius};
    auto* const  drawList = ImGui::GetWindowDrawList();
    drawList->PathArcTo(center, radius - thickness * 0.5f, angle, angle + Pi * 1.55f, 32);
    drawList->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark), ImDrawFlags_None, thickness);
}

} // namespace

void RenderMode::draw() {
    auto&        editor      = ReplayEditor::getInstance();
    auto const&  status      = editor.state().exportStatus;
    ImVec2 const displaySize = ImGui::GetIO().DisplaySize;

    auto* const background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled({0.0f, 0.0f}, displaySize, IM_COL32(0, 0, 0, 255));
    if (auto const texture = editor.gameTexture()) {
        float const aspect = std::clamp(editor.videoAspectRatio(), 0.25f, 4.0f);
        ImVec2      imageSize{displaySize.x, displaySize.x / aspect};
        if (imageSize.y > displaySize.y) imageSize = {displaySize.y * aspect, displaySize.y};
        ImVec2 const imageMin{(displaySize.x - imageSize.x) * 0.5f, (displaySize.y - imageSize.y) * 0.5f};
        ImVec2 const imageMax{imageMin.x + imageSize.x, imageMin.y + imageSize.y};
        background->AddImage(ImTextureRef(texture), imageMin, imageMax);
    }

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(displaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.32f));
    ImGui::Begin(
        "##ExportModalShield",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoBringToFrontOnFocus
    );
    ImGui::End();
    ImGui::PopStyleColor();

    float modalWidth =
        std::min(std::clamp(displaySize.x * 0.52f, 360.0f, 640.0f), std::max(1.0f, displaySize.x - 24.0f));
    float modalHeight =
        std::min(std::clamp(displaySize.y * 0.58f, 360.0f, 500.0f), std::max(1.0f, displaySize.y - 40.0f));
    ImGui::SetNextWindowPos({displaySize.x * 0.5f, displaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({modalWidth, modalHeight}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    bool const progressVisible = ImGui::Begin(
        "##ExportProgress",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings
    );
    if (progressVisible) {
        drawSpinner(18.0f, 3.0f);
        ImGui::Spacing();

        std::string const phase      = exportStateLabel(status.state);
        float const       phaseWidth = ImGui::CalcTextSize(phase.c_str()).x;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - phaseWidth) * 0.5f)
        );
        ImGui::TextUnformatted(phase.c_str());
        ImGui::Spacing();

        auto const  capturedFrames = std::min(status.submittedFrames, status.totalFrames);
        auto const  writtenFrames  = std::min(status.writtenFrames, status.totalFrames);
        auto const  progressFrames = std::max(capturedFrames, writtenFrames);
        float const progress =
            status.totalFrames > 0 ? static_cast<float>(progressFrames) / static_cast<float>(status.totalFrames) : 0.0f;
        char progressLabel[32];
        std::snprintf(progressLabel, sizeof(progressLabel), "%d%%", static_cast<int>(progress * 100.0f));
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 24.0f), progressLabel);
        ImGui::Spacing();

        std::string const format     = status.format == exporting::ExportFormat::Mp4Video
                                         ? "playback.refactorEditor.export.mp4"_tr()
                                         : "playback.refactorEditor.export.pngSequence"_tr();
        auto const        outputUtf8 = status.outputPath.generic_u8string();
        std::string       outputPath{reinterpret_cast<char const*>(outputUtf8.data()), outputUtf8.size()};
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.0f, 5.0f});
        if (ImGui::BeginTable(
                "##export-progress-details",
                2,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp
            )) {
            ImGui::TableSetupColumn("##export-progress-label", ImGuiTableColumnFlags_WidthFixed, 140.0f);
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
            row("playback.refactorEditor.render.formatLabel"_tr(), format);
            if (!outputPath.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.render.outputLabel"_tr().c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText(
                    "##render-output-path",
                    outputPath.data(),
                    outputPath.size() + 1,
                    ImGuiInputTextFlags_ReadOnly
                );
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        if (!status.message.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextDisabled("%s", status.message.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::Spacing();
        float const buttonWidth  = std::min(180.0f, ImGui::GetContentRegionAvail().x);
        float const buttonHeight = 32.0f;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f)
        );
        bool const cancelling = status.state == exporting::ExportState::Cancelling;
        ImGui::BeginDisabled(cancelling);
        std::string const cancelLabel =
            cancelling ? "playback.refactorEditor.render.cancelling"_tr()
                       : std::string(ICON_CLOSE) + "  " + "playback.refactorEditor.render.cancel"_tr();
        if (ImGui::Button(cancelLabel.c_str(), ImVec2(buttonWidth, buttonHeight))) {
            editor.submitAction({EditorActionType::CancelExport});
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace playback::editor::ui

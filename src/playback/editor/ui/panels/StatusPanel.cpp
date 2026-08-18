#include "StatusPanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace playback::editor::ui {

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

ImVec4 exportStateColor(exporting::ExportState state) {
    switch (state) {
    case exporting::ExportState::Completed:
        return {0.40f, 0.78f, 0.48f, 1.0f};
    case exporting::ExportState::Faulted:
        return {0.94f, 0.38f, 0.36f, 1.0f};
    case exporting::ExportState::Cancelled:
        return {0.68f, 0.68f, 0.68f, 1.0f};
    default:
        return {0.35f, 0.67f, 0.96f, 1.0f};
    }
}

std::string pathText(std::filesystem::path const& path) {
    auto const utf8 = path.generic_u8string();
    return {reinterpret_cast<char const*>(utf8.data()), utf8.size()};
}

} // namespace

void StatusPanel::draw() {
    auto const& state        = ReplayEditor::getInstance().state();
    auto const& exportStatus = state.exportStatus;
    std::string statusText;
    ImVec4      color;
    if (exportStatus.state == exporting::ExportState::Idle) {
        statusText = state.capabilities.videoExport ? "playback.refactorEditor.status.ready"_tr()
                                                    : "playback.refactorEditor.status.exportUnavailable"_tr();
        color      = {0.62f, 0.62f, 0.62f, 1.0f};
    } else {
        statusText = exportStateLabel(exportStatus.state);
        if (exporting::isExportActive(exportStatus.state) && exportStatus.totalFrames > 0) {
            auto const completed =
                std::min(exportStatus.totalFrames, std::max(exportStatus.submittedFrames, exportStatus.writtenFrames));
            int const percent  = static_cast<int>(completed * 100 / exportStatus.totalFrames);
            statusText        += "  " + std::to_string(percent) + "%";
        }
        color = exportStateColor(exportStatus.state);
    }

    std::string const replayText = "playback.refactorEditor.status.replay"_tr();
    std::string const tickText   = "playback.refactorEditor.status.tick"_tr(state.currentTick, state.totalTicks);
    char              speedText[32]{};
    std::snprintf(speedText, sizeof(speedText), "%.2fx", state.playbackSpeed);

    auto const& style         = ImGui::GetStyle();
    float const contentStart  = ImGui::GetWindowContentRegionMin().x;
    float const rightEdge     = ImGui::GetWindowContentRegionMax().x;
    float const contentWidth  = std::max(0.0f, rightEdge - contentStart);
    float const statusWidth   = ImGui::CalcTextSize(statusText.c_str()).x;
    float const playbackWidth = ImGui::CalcTextSize(replayText.c_str()).x + ImGui::CalcTextSize(tickText.c_str()).x
                              + ImGui::CalcTextSize(speedText).x + style.ItemSpacing.x * 2.0f;
    bool const showPlaybackSummary = playbackWidth + statusWidth + 12.0f <= contentWidth;

    if (showPlaybackSummary) {
        ImGui::TextUnformatted(replayText.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(tickText.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(speedText);
        ImGui::SameLine(std::max(contentStart, rightEdge - statusWidth));
    } else {
        ImGui::SetCursorPosX(std::max(contentStart, rightEdge - statusWidth));
    }
    ImGui::TextColored(color, "%s", statusText.c_str());
    if (ImGui::IsItemHovered() && (!exportStatus.message.empty() || !exportStatus.outputPath.empty())) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
        if (!exportStatus.message.empty()) ImGui::TextWrapped("%s", exportStatus.message.c_str());
        if (!exportStatus.outputPath.empty()) {
            if (!exportStatus.message.empty()) ImGui::Separator();
            ImGui::TextWrapped("%s", pathText(exportStatus.outputPath).c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace playback::editor::ui

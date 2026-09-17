#include "StatusPanel.h"

#include "playback/editor/ui/EditorTheme.h"

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
        return ImGui::ColorConvertU32ToFloat4(theme::kSuccess);
    case exporting::ExportState::Faulted:
        return ImGui::ColorConvertU32ToFloat4(theme::kError);
    case exporting::ExportState::Cancelled:
        return ImGui::ColorConvertU32ToFloat4(theme::kTextDim);
    default:
        return ImGui::ColorConvertU32ToFloat4(theme::kAccent);
    }
}

std::string pathText(std::filesystem::path const& path) {
    auto const utf8 = path.generic_u8string();
    return {reinterpret_cast<char const*>(utf8.data()), utf8.size()};
}

} // namespace

void StatusPanel::draw(PanelContext const& ctx) {
    auto const& state        = ctx.state;
    auto const& exportStatus = state.exportStatus;
    std::string statusText;
    ImVec4      color;
    if (exportStatus.state == exporting::ExportState::Idle) {
        statusText = state.capabilities.videoExport ? "playback.refactorEditor.status.ready"_tr()
                                                    : "playback.refactorEditor.status.exportUnavailable"_tr();
        color      = ImGui::ColorConvertU32ToFloat4(theme::kTextDim);
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

    // Left is the edited file and its save state, right is export state; the timeline row carries the timecode.
    std::string leftText;
    if (!state.persistence.projectFile.empty()) {
        leftText = state.persistence.dirty
                     ? "playback.refactorEditor.status.projectUnsaved"_tr(state.persistence.projectFile)
                     : "playback.refactorEditor.status.projectSaved"_tr(state.persistence.projectFile);
    } else {
        leftText = "playback.refactorEditor.status.tick"_tr(state.currentTick, state.totalTicks);
    }

    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    float const  width    = ImGui::GetContentRegionAvail().x;
    float const  height   = std::max(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().y);
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme::kBgHeader);
    drawList->AddLine(origin, {origin.x + width, origin.y}, theme::kBorder);

    float const pad         = theme::kPanelPadding;
    float const textY       = origin.y + (height - ImGui::GetFontSize()) * 0.5f;
    float const statusWidth = ImGui::CalcTextSize(statusText.c_str()).x;
    if (ImGui::CalcTextSize(leftText.c_str()).x + statusWidth + pad * 4.0f <= width) {
        drawList->AddText({origin.x + pad, textY}, theme::kTextDim, leftText.c_str());
    }
    ImGui::SetCursorScreenPos({origin.x + std::max(pad, width - statusWidth - pad), textY});
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

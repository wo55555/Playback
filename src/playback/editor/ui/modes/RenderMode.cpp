#include "RenderMode.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
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

} // namespace

void RenderMode::draw() {
    auto&       editor        = ReplayEditor::getInstance();
    auto const& status        = editor.state().exportStatus;
    auto const& style         = ImGui::GetStyle();
    float const kMenuHeight   = ImGui::GetFrameHeight() + style.WindowBorderSize * 2.0f;
    float const kStatusHeight = ImGui::GetFontSize() + style.WindowPadding.y * 2.0f;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    bool const popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGuiWindowFlags const inputBlock = popupOpen ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None;

    auto drawMenuBar = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin(
            "##RenderMenuBar",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar
        );
        editor.mMenuBar.draw();
        ImGui::End();
    };

    {
        float const workspaceHeight = std::max(1.0f, displaySize.y - kMenuHeight - kStatusHeight);
        ImGui::SetNextWindowPos(ImVec2(0.0f, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, workspaceHeight));
        ImGui::Begin(
            "##RenderWorkspace",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                | inputBlock
        );

        ImVec2 const available    = ImGui::GetContentRegionAvail();
        float const contentWidth = std::min(620.0f, std::max(1.0f, available.x));
        float const contentLeft  = ImGui::GetCursorPosX() + std::max(0.0f, (available.x - contentWidth) * 0.5f);
        float const blockHeight  = 350.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (available.y - blockHeight) * 0.5f));
        auto const centerItem = [contentLeft, contentWidth](float width) {
            ImGui::SetCursorPosX(contentLeft + std::max(0.0f, (contentWidth - width) * 0.5f));
        };

        centerItem(ImGui::CalcTextSize(ICON_RENDER).x);
        ImGui::Text("%s", ICON_RENDER);

        std::string const phase = exportStateLabel(status.state);
        centerItem(ImGui::CalcTextSize(phase.c_str()).x);
        ImGui::TextUnformatted(phase.c_str());

        ImGui::Spacing();

        auto const capturedFrames = std::min(status.submittedFrames, status.totalFrames);
        auto const writtenFrames  = std::min(status.writtenFrames, status.totalFrames);
        auto const progressFrames = std::max(capturedFrames, writtenFrames);
        float const progress      = status.totalFrames > 0
                                      ? static_cast<float>(progressFrames) / static_cast<float>(status.totalFrames)
                                      : 0.0f;
        char        progressLabel[32];
        std::snprintf(progressLabel, sizeof(progressLabel), "%d%%", static_cast<int>(progress * 100.0f));

        ImGui::SetCursorPosX(contentLeft);
        ImGui::ProgressBar(progress, ImVec2(contentWidth, 24.0f), progressLabel);
        ImGui::Spacing();

        std::string const format = status.format == exporting::ExportFormat::Mp4Video
                                     ? "playback.refactorEditor.export.mp4"_tr()
                                     : "playback.refactorEditor.export.pngSequence"_tr();
        auto const  outputUtf8 = status.outputPath.generic_u8string();
        std::string outputPath{reinterpret_cast<char const*>(outputUtf8.data()), outputUtf8.size()};
        ImGui::SetCursorPosX(contentLeft);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.0f, 5.0f});
        if (ImGui::BeginTable(
                "##export-progress-details",
                2,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                {contentWidth, 0.0f}
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
            row(
                "playback.refactorEditor.render.capturedLabel"_tr(),
                "playback.refactorEditor.render.frameCount"_tr(capturedFrames, status.totalFrames)
            );
            row(
                "playback.refactorEditor.render.writtenLabel"_tr(),
                "playback.refactorEditor.render.frameCount"_tr(writtenFrames, status.totalFrames)
            );
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
            ImGui::SetCursorPosX(contentLeft);
            ImGui::PushTextWrapPos(contentLeft + contentWidth);
            ImGui::TextDisabled("%s", status.message.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::Spacing();
        float const buttonWidth  = std::min(180.0f, contentWidth);
        float const buttonHeight = 32.0f;
        centerItem(buttonWidth);
        bool const cancelling = status.state == exporting::ExportState::Cancelling;
        ImGui::BeginDisabled(cancelling);
        std::string const cancelLabel = std::string(cancelling ? ICON_LOADER : ICON_CLOSE) + "  "
                                      + (cancelling ? "playback.refactorEditor.render.cancelling"_tr()
                                                    : "playback.refactorEditor.render.cancel"_tr());
        if (ImGui::Button(cancelLabel.c_str(), ImVec2(buttonWidth, buttonHeight))) {
            editor.submitAction({EditorActionType::CancelExport});
        }
        ImGui::EndDisabled();

        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin(
            "##RenderStatus",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mStatusPanel.draw();
        ImGui::End();
    }

    // Keep the menu and its modal export dialog above every workspace window.
    drawMenuBar();
}

} // namespace playback::editor::ui

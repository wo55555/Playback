#include "RecordingStatusOverlay.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cfloat>
#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void drawRecordingStatusOverlay(
    functions::RecordingStatusSnapshot const& status,
    config::RecordingControlsConfig const&    config,
    ImVec2                                     displaySize
) {
    if (!config.showStatusOverlay) return;
    if (status.state != functions::RecordingState::Recording && status.state != functions::RecordingState::Paused) return;

    auto const seconds = std::max<int64_t>(0, status.elapsed.count());
    char       duration[32]{};
    std::snprintf(duration, sizeof(duration), "%lld:%02lld", seconds / 60, seconds % 60);
    std::string const text = (status.state == functions::RecordingState::Paused
                                  ? "playback.recording.status.paused"_tr()
                                  : "playback.recording.status.recording"_tr())
                           + " " + duration;
    ImFont* const font = ImGui::GetFont();
    float const fontSize = ImGui::GetFontSize() * 3.0f;
    ImVec2 const dotSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "●");
    ImVec2 const textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
    float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    ImVec2 const totalSize{dotSize.x + spacing + textSize.x, std::max(dotSize.y, textSize.y)};
    constexpr float margin = 20.0f;
    ImVec2 position{margin, margin};
    switch (config.overlayPosition) {
    case config::RecordingOverlayPosition::TopRight:
        position = {displaySize.x - totalSize.x - margin, margin};
        break;
    case config::RecordingOverlayPosition::BottomLeft:
        position = {margin, displaySize.y - totalSize.y - margin};
        break;
    case config::RecordingOverlayPosition::BottomRight:
        position = {displaySize.x - totalSize.x - margin, displaySize.y - totalSize.y - margin};
        break;
    case config::RecordingOverlayPosition::TopLeft:
    default:
        break;
    }

    auto* drawList = ImGui::GetForegroundDrawList();
    float const baselineY = position.y + (totalSize.y - textSize.y) * 0.5f;
    bool const blinkOn = status.state != functions::RecordingState::Recording
                      || (std::chrono::steady_clock::now().time_since_epoch().count()
                          / std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count())
                             % 2 == 0;
    if (blinkOn) {
        drawList->AddText(font, fontSize, {position.x + 1.0f, baselineY + 1.0f}, IM_COL32(0, 0, 0, 190), "●");
        drawList->AddText(font, fontSize, position, IM_COL32(235, 55, 55, 255), "●");
    }
    ImVec2 const textPosition{position.x + dotSize.x + spacing, baselineY};
    drawList->AddText(font, fontSize, {textPosition.x + 1.0f, textPosition.y + 1.0f}, IM_COL32(0, 0, 0, 190), text.c_str());
    drawList->AddText(font, fontSize, textPosition, IM_COL32(255, 255, 255, 255), text.c_str());
}

} // namespace playback::editor::ui

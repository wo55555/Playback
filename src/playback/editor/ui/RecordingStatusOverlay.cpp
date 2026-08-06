#include "RecordingStatusOverlay.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

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
    std::string const label = "● "
                            + (status.state == functions::RecordingState::Paused
                                   ? "playback.recording.status.paused"_tr()
                                   : "playback.recording.status.recording"_tr())
                            + " " + duration;

    ImVec2 const textSize = ImGui::CalcTextSize(label.c_str());
    constexpr float margin = 20.0f;
    ImVec2 position{margin, margin};
    switch (config.overlayPosition) {
    case config::RecordingOverlayPosition::TopRight:
        position = {displaySize.x - textSize.x - margin, margin};
        break;
    case config::RecordingOverlayPosition::BottomLeft:
        position = {margin, displaySize.y - textSize.y - margin};
        break;
    case config::RecordingOverlayPosition::BottomRight:
        position = {displaySize.x - textSize.x - margin, displaySize.y - textSize.y - margin};
        break;
    case config::RecordingOverlayPosition::TopLeft:
    default:
        break;
    }

    auto* drawList = ImGui::GetForegroundDrawList();
    drawList->AddText({position.x + 1.0f, position.y + 1.0f}, IM_COL32(0, 0, 0, 190), label.c_str());
    drawList->AddText(position, IM_COL32(255, 80, 80, 255), label.c_str());
}

} // namespace playback::editor::ui

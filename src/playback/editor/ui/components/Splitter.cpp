#include "playback/editor/ui/components/Splitter.h"

#include "playback/editor/ui/EditorTheme.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

namespace {

struct Band {
    bool active{};
    bool activated{};
};

Band drawBand(char const* id, bool vertical) {
    ImVec2 const min  = ImGui::GetWindowPos();
    ImVec2 const size = ImGui::GetWindowSize();
    ImVec2 const max{min.x + size.x, min.y + size.y};
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, size);
    bool const hovered = ImGui::IsItemHovered();
    Band const band{ImGui::IsItemActive(), ImGui::IsItemActivated()};
    if (hovered || band.active) ImGui::SetMouseCursor(band.active ? theme::kCursorGrabbing : theme::kCursorGrab);

    // Its own window already sits above the panels; the foreground list would cover tooltips instead.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, max, false);
    if (hovered || band.active) {
        dl->AddRectFilled(min, max, band.active ? theme::kAccent : theme::withAlpha(theme::kAccent, 0xa0));
    } else if (vertical) {
        float const x = (min.x + max.x) * 0.5f;
        dl->AddRectFilled({x - 1, min.y}, {x + 1, max.y}, theme::kBorder);
    } else {
        float const y = (min.y + max.y) * 0.5f;
        dl->AddRectFilled({min.x, y - 1}, {max.x, y + 1}, theme::kBorder);
    }
    dl->PopClipRect();
    return band;
}

} // namespace

// Dragging follows the mouse delta, so grabbing the band off-centre does not jump the split.
float Splitter::drawVerticalSplit(float ratio, Rect area, float minR, float maxR) {
    Band const band = drawBand("##details-splitter", true);
    if (band.activated) mDragStartRatio = ratio;
    if (band.active && area.GetWidth() > 0.0f) {
        float const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x / area.GetWidth();
        ratio             = std::clamp(mDragStartRatio - delta, minR, maxR);
    }
    return ratio;
}

float Splitter::drawHorizontalSplit(float ratio, Rect area, float minR, float maxR) {
    Band const band = drawBand("##timeline-splitter", false);
    if (band.activated) mDragStartRatio = ratio;
    if (band.active && area.GetHeight() > 0.0f) {
        float const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y / area.GetHeight();
        ratio             = std::clamp(mDragStartRatio + delta, minR, maxR);
    }
    return ratio;
}

} // namespace playback::editor::ui

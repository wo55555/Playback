#include "playback/editor/ui/components/Splitter.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

namespace {

constexpr float kSplitterThickness = 4.0f;

} // namespace

float Splitter::drawVerticalSplit(float ratio, Rect area, float minR, float maxR) {
    float splitX = area.max.x - area.GetWidth() * ratio;

    ImGui::SetCursorScreenPos({splitX - kSplitterThickness / 2, area.min.y});
    ImGui::InvisibleButton("##details-splitter", {kSplitterThickness, area.GetHeight()});

    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float newRatio = (area.max.x - ImGui::GetMousePos().x) / area.GetWidth();
        ratio          = std::clamp(newRatio, minR, maxR);
        splitX         = area.max.x - area.GetWidth() * ratio;
    }

    ImDrawList* dl    = ImGui::GetForegroundDrawList();
    ImU32 color = active ? IM_COL32(240, 192, 32, 255)
                           : (hovered ? IM_COL32(120, 120, 120, 255)
                                      : IM_COL32(60, 60, 60, 255));
    dl->AddRectFilled({splitX - 1, area.min.y}, {splitX + 1, area.max.y}, color);

    return ratio;
}

float Splitter::drawHorizontalSplit(float ratio, Rect area, float minR, float maxR) {
    float splitY = area.min.y + area.GetHeight() * ratio;

    ImGui::SetCursorScreenPos({area.min.x, splitY - kSplitterThickness / 2});
    ImGui::InvisibleButton("##timeline-splitter", {area.GetWidth(), kSplitterThickness});

    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float mouseY    = ImGui::GetMousePos().y;
        float newRatio  = (mouseY - area.min.y) / area.GetHeight();
        ratio           = std::clamp(newRatio, minR, maxR);
        splitY          = area.min.y + area.GetHeight() * ratio;
    }

    ImDrawList* dl   = ImGui::GetForegroundDrawList();
    ImU32       color = active ? IM_COL32(240, 192, 32, 255)
                               : (hovered ? IM_COL32(120, 120, 120, 255)
                                          : IM_COL32(60, 60, 60, 255));
    dl->AddRectFilled({area.min.x, splitY - 1}, {area.max.x, splitY + 1}, color);

    return ratio;
}

} // namespace playback::editor::ui

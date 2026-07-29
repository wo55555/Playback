#include "Splitter.h"

#include "imgui.h"

namespace playback::refactor::editor {

namespace {

constexpr float kSplitterHeight = 4.0f;

} // namespace

float Splitter::drawVerticalSplit(float ratio, Rect area, float minR, float maxR) {
    float splitY = area.min.y + area.GetHeight() * ratio;

    ImGui::SetCursorScreenPos({area.min.x, splitY - kSplitterHeight / 2});
    ImGui::InvisibleButton("##splitter", {area.GetWidth(), kSplitterHeight});

    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float mouseY    = ImGui::GetMousePos().y;
        float newRatio  = (mouseY - area.min.y) / area.GetHeight();
        ratio           = std::clamp(newRatio, minR, maxR);
    }

    // Draw splitter visual
    ImDrawList* dl   = ImGui::GetForegroundDrawList();
    ImU32       color = active ? IM_COL32(240, 192, 32, 255)
                               : (hovered ? IM_COL32(120, 120, 120, 255)
                                          : IM_COL32(60, 60, 60, 255));
    dl->AddRectFilled({area.min.x, splitY - 1}, {area.max.x, splitY + 1}, color);

    return ratio;
}

} // namespace playback::refactor::editor
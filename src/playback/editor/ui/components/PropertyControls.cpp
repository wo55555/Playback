#include "PropertyControls.h"

#include "playback/editor/ui/EditorTheme.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui::property {

namespace {

constexpr ImU32 kInspectorHeaderColor = theme::kInspectorHeader;
constexpr ImU32 kSectionColor         = theme::kSection;
constexpr ImU32 kSectionHoverColor    = theme::kSectionHover;
constexpr ImU32 kSectionAccentColor   = theme::kAccent;
constexpr ImU32 kFineDividerColor     = theme::kFineDivider;
constexpr float kFineDividerThickness = theme::kFineDividerWidth;

// Padding follows the font, so the inspector keeps its proportions at every UI scale.
float uiScale() { return std::max(1.0f, ImGui::GetFontSize() / 14.0f); }

} // namespace

void beginInspector(std::string_view title, std::string_view objectName) {
    float const  scale    = uiScale();
    float const  height   = ImGui::GetFontSize() * 2.0f + 10.0f * scale;
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    float const  width    = ImGui::GetContentRegionAvail().x;
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, kInspectorHeaderColor);
    drawList->AddRectFilled(origin, {origin.x + 2.0f * scale, origin.y + height}, kSectionAccentColor);
    drawList->AddLine(
        {origin.x, origin.y + height},
        {origin.x + width, origin.y + height},
        kFineDividerColor,
        kFineDividerThickness
    );
    ImGui::SetCursorScreenPos({origin.x + 10.0f * scale, origin.y + 4.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kIconActive);
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({origin.x + 10.0f * scale, origin.y + ImGui::GetFontSize() + 6.0f * scale});
    ImGui::TextDisabled("%.*s", static_cast<int>(objectName.size()), objectName.data());
    ImGui::SetCursorScreenPos({origin.x, origin.y + height + 6.0f * scale});
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * scale, 3.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Header, kSectionColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSectionHoverColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSectionHoverColor);
    bool const   open    = ImGui::TreeNodeEx(label, flags);
    ImVec2 const minimum = ImGui::GetItemRectMin();
    ImVec2 const maximum = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()
        ->AddRectFilled(minimum, {minimum.x + 2.0f * scale, maximum.y}, kSectionAccentColor, 1.0f * scale);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return open;
}

void endSection() {
    ImGui::TreePop();
    ImGui::Dummy({0.0f, 4.0f * uiScale()});
}

void textRow(char const* label, char const* value) {
    float const width      = ImGui::GetContentRegionAvail().x;
    float const labelWidth = std::clamp(width * 0.38f, 72.0f, 160.0f);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(labelWidth);
    ImGui::TextUnformatted(value);
    separator();
}

void separator() {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const  width  = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, kFineDividerColor, kFineDividerThickness);
    ImGui::Dummy({0.0f, 5.0f * uiScale()});
}

bool actionButton(char const* label, bool enabled) {
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, kFineDividerThickness);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Border, kFineDividerColor);
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(label, {-1.0f, ImGui::GetFontSize() + 8.0f * scale});
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

} // namespace playback::editor::ui::property

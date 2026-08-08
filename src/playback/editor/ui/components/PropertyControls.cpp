#include "PropertyControls.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui::property {

namespace {

constexpr ImU32 kInspectorHeaderColor = IM_COL32(32, 32, 32, 255);
constexpr ImU32 kSectionColor         = IM_COL32(45, 45, 45, 255);
constexpr ImU32 kSectionHoverColor    = IM_COL32(56, 56, 56, 255);
constexpr ImU32 kSectionAccentColor   = IM_COL32(58, 140, 240, 255);
constexpr ImU32 kFineDividerColor     = IM_COL32(66, 66, 66, 210);
constexpr float kFineDividerThickness = 0.75f;

float uiScale() {
    return std::max(1.0f, ImGui::GetIO().FontGlobalScale);
}

} // namespace

void beginInspector(std::string_view title, std::string_view objectName) {
    float const scale = uiScale();
    float const height = ImGui::GetFontSize() * 2.0f + 14.0f * scale;
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const width = ImGui::GetContentRegionAvail().x;
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, kInspectorHeaderColor);
    drawList->AddRectFilled(origin, {origin.x + 3.0f * scale, origin.y + height}, kSectionAccentColor);
    drawList->AddLine(
        {origin.x, origin.y + height},
        {origin.x + width, origin.y + height},
        kFineDividerColor,
        kFineDividerThickness
    );
    ImGui::SetCursorScreenPos({origin.x + 12.0f * scale, origin.y + 6.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(236, 238, 242, 255));
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({origin.x + 12.0f * scale, origin.y + ImGui::GetFontSize() + 10.0f * scale});
    if (objectName.empty()) ImGui::TextDisabled("No selection");
    else ImGui::TextDisabled("%.*s", static_cast<int>(objectName.size()), objectName.data());
    ImGui::SetCursorScreenPos({origin.x, origin.y + height + 8.0f * scale});
}

void searchBar(char const* id, char const* hint, char* buffer, size_t bufferSize) {
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f * scale, 4.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(29, 29, 29, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(39, 39, 39, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(46, 46, 46, 255));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::Dummy({0.0f, 5.0f * scale});
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f * scale, 5.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Header, kSectionColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSectionHoverColor);
    bool const open = ImGui::TreeNodeEx(label, flags);
    ImVec2 const minimum = ImGui::GetItemRectMin();
    ImVec2 const maximum = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        minimum,
        {minimum.x + 3.0f * scale, maximum.y},
        kSectionAccentColor,
        1.5f * scale
    );
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    return open;
}

void endSection() {
    ImGui::TreePop();
    ImGui::Dummy({0.0f, 5.0f * uiScale()});
}

void textRow(char const* label, char const* value) {
    float const width = ImGui::GetContentRegionAvail().x;
    float const labelWidth = std::clamp(width * 0.42f, 110.0f, 180.0f);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(labelWidth);
    ImGui::TextUnformatted(value);
    separator();
}

void separator() {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, kFineDividerColor, kFineDividerThickness);
    ImGui::Dummy({0.0f, 7.0f * uiScale()});
}

bool actionButton(char const* label, bool enabled) {
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, kFineDividerThickness);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(52, 52, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(68, 68, 68, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(76, 76, 76, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, kFineDividerColor);
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(label, {-1.0f, ImGui::GetFontSize() + 8.0f * scale});
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

}

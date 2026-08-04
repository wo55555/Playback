#include "PropertyControls.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui::property {

namespace {

constexpr ImU32 kFineDividerColor = IM_COL32(66, 66, 66, 210);
constexpr float kFineDividerThickness = 0.75f;

float rowHeight() {
    return ImGui::GetFontSize() + 12.0f;
}

}

void beginInspector(std::string_view title, std::string_view objectName) {
    float const height = rowHeight();
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const width = ImGui::GetContentRegionAvail().x;
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(37, 37, 37, 255));
    drawList->AddLine({origin.x, origin.y + height}, {origin.x + width, origin.y + height}, kFineDividerColor, kFineDividerThickness);
    ImGui::SetCursorScreenPos({origin.x + 10.0f, origin.y + 6.0f});
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::SetCursorScreenPos({origin.x + 10.0f, origin.y + height + 8.0f});
    ImGui::TextDisabled("%s", objectName.empty() ? "No selection" : std::string(objectName).c_str());
    ImGui::SetCursorScreenPos({origin.x, origin.y + height + ImGui::GetFontSize() + 18.0f});
}

void searchBar(char const* id, char const* hint, char* buffer, size_t bufferSize) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
    ImGui::PopStyleVar();
    ImGui::Spacing();
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(45, 45, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(55, 55, 55, 255));
    bool const open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopStyleColor(2);
    return open;
}

void endSection() {
    ImGui::TreePop();
    ImGui::Spacing();
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
    ImGui::Dummy({0.0f, 7.0f});
}

bool actionButton(char const* label, bool enabled) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, kFineDividerThickness);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(52, 52, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(68, 68, 68, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(76, 76, 76, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, kFineDividerColor);
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(label, {-1.0f, 0.0f});
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

}

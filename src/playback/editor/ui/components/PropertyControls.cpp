#include "PropertyControls.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace playback::editor::ui::property {

namespace {

constexpr ImU32 kInspectorHeaderColor = theme::kInspectorHeader;
constexpr ImU32 kSectionColor         = theme::kSection;
constexpr ImU32 kSectionHoverColor    = theme::kSectionHover;
constexpr ImU32 kSectionAccentColor   = theme::kAccent;
constexpr ImU32 kFineDividerColor     = theme::kFineDivider;

// Padding follows the font, so the inspector keeps its proportions at every UI scale.
float uiScale() { return metrics::scale(); }

struct ButtonPalette {
    ImU32 fill;
    ImU32 hover;
    ImU32 active;
    ImU32 border;
    ImU32 text;
};

ButtonPalette paletteFor(ButtonVariant variant) {
    switch (variant) {
    case ButtonVariant::Primary:
        return {
            theme::kButtonPrimary,
            theme::kButtonPrimaryHover,
            theme::kButtonPrimaryActive,
            theme::kButtonPrimaryBorder,
            theme::kIconHighlight,
        };
    case ButtonVariant::Danger:
        return {
            theme::kButtonDanger,
            theme::kButtonDangerHover,
            theme::kButtonDangerActive,
            theme::kButtonDangerBorder,
            theme::kButtonDangerText,
        };
    case ButtonVariant::Subtle:
        return {
            theme::kButtonSubtle,
            theme::kButtonSubtleHover,
            theme::kButtonSubtleActive,
            theme::kButtonSubtleBorder,
            theme::kText,
        };
    case ButtonVariant::Secondary:
    default:
        return {
            theme::kButton,
            theme::kButtonHover,
            theme::kButtonActive,
            theme::kButtonBorder,
            theme::kIconActive,
        };
    }
}

std::string buildButtonLabel(char const* label, char const* icon) {
    if (!icon || *icon == '\0') return label ? std::string(label) : std::string{};
    return std::string(icon) + "  " + (label ? label : "");
}

bool drawStyledButton(
    std::string const& fullLabel,
    float              width,
    bool               enabled,
    ButtonVariant      variant,
    char const*        tooltip
) {
    float const   scale   = uiScale();
    float const   height  = std::round(ImGui::GetFontSize() + 11.0f * scale);
    ButtonPalette palette = paletteFor(variant);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kButtonRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f * scale, 4.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Button, palette.fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.active);
    ImGui::PushStyleColor(ImGuiCol_Border, palette.border);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.text);
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(fullLabel.c_str(), {width, height});
    ImGui::EndDisabled();
    if (tooltip && *tooltip != '\0') widgets::itemTooltip(tooltip);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    return clicked;
}

} // namespace

void beginInspector(std::string_view title, std::string_view objectName) {
    float const  scale    = uiScale();
    float const  fontSize = ImGui::GetFontSize();
    float const  padY     = 7.0f * scale;
    float const  lineGap  = 3.0f * scale;
    float const  height   = std::round(fontSize * 2.0f + padY * 2.0f + lineGap);
    float const  width    = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const  rounding = theme::kButtonRounding * scale;
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    ImVec2 const maximum  = {origin.x + width, origin.y + height};
    auto*        drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(origin, maximum, kInspectorHeaderColor, rounding);
    drawList->AddRect(origin, maximum, kFineDividerColor, rounding, 0, 1.0f);

    // Rounded accent pill inset cleanly along the left edge.
    float const barW     = std::max(2.5f, 3.0f * scale);
    float const barInset = 5.0f * scale;
    drawList->AddRectFilled(
        {origin.x + 3.0f * scale, origin.y + barInset},
        {origin.x + 3.0f * scale + barW, maximum.y - barInset},
        kSectionAccentColor,
        barW * 0.5f
    );

    float const textX     = origin.x + 12.0f * scale;
    float const textRight = std::max(textX + 1.0f, maximum.x - 8.0f * scale);
    drawList->PushClipRect({textX, origin.y}, {textRight, maximum.y}, true);
    drawList->AddText({textX, origin.y + padY}, theme::kIconActive, title.data(), title.data() + title.size());
    drawList->AddText(
        {textX, origin.y + padY + fontSize + lineGap},
        theme::kTextDim,
        objectName.data(),
        objectName.data() + objectName.size()
    );
    drawList->PopClipRect();

    ImGui::Dummy({width, height});
    ImGui::Dummy({0.0f, 3.0f * scale});
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    float const scale = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kButtonRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f * scale, 5.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, kSectionColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSectionHoverColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSectionHoverColor);
    ImGui::PushStyleColor(ImGuiCol_Border, kFineDividerColor);
    bool const   open    = ImGui::TreeNodeEx(label, flags);
    ImVec2 const minimum = ImGui::GetItemRectMin();
    ImVec2 const maximum = ImGui::GetItemRectMax();
    float const  barW    = std::max(2.0f, 2.5f * scale);
    float const  insetY  = 4.0f * scale;
    ImGui::GetWindowDrawList()->AddRectFilled(
        {minimum.x + 2.5f * scale, minimum.y + insetY},
        {minimum.x + 2.5f * scale + barW, maximum.y - insetY},
        kSectionAccentColor,
        barW * 0.5f
    );
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    if (open) {
        ImGui::PushID(label);
        ImGui::Dummy({0.0f, 1.0f * scale});
    }
    return open;
}

void endSection() {
    ImGui::PopID();
    ImGui::Dummy({0.0f, 6.0f * uiScale()});
}

void textRow(char const* label, char const* value) {
    float const scale      = uiScale();
    float const startX     = ImGui::GetCursorPosX();
    float const width      = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const labelWidth = std::clamp(width * 0.36f, 64.0f * scale, 136.0f * scale);
    float const textEndX   = startX + ImGui::CalcTextSize(label).x + 10.0f * scale;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(std::max(startX + labelWidth, textEndX));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value);
    ImGui::PopTextWrapPos();
}

void formattedRow(std::string_view text) {
    constexpr std::string_view kFullwidthColon = "\xEF\xBC\x9A";
    size_t const               fwPos           = text.find(kFullwidthColon);
    size_t const               asciiPos        = text.find(':');
    size_t                     splitPos        = std::string_view::npos;
    size_t                     sepLen          = 0;
    if (fwPos != std::string_view::npos && (asciiPos == std::string_view::npos || fwPos < asciiPos)) {
        splitPos = fwPos;
        sepLen   = kFullwidthColon.size();
    } else if (asciiPos != std::string_view::npos) {
        splitPos = asciiPos;
        sepLen   = 1;
    }
    if (splitPos == std::string_view::npos) {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        return;
    }
    std::string label(text.substr(0, splitPos));
    std::string value(text.substr(splitPos + sepLen));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    textRow(label.c_str(), value.c_str());
}

void separator() {
    float const scale = uiScale();
    ImGui::Dummy({0.0f, 2.0f * scale});
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const  width  = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, kFineDividerColor, 1.0f);
    ImGui::Dummy({0.0f, 3.0f * scale});
}

bool actionButton(char const* label, bool enabled, ButtonVariant variant, char const* icon, char const* tooltip) {
    return drawStyledButton(buildButtonLabel(label, icon), -1.0f, enabled, variant, tooltip);
}

int actionButtonPair(ActionButtonSpec const& first, ActionButtonSpec const& second) {
    float const       scale       = uiScale();
    float const       gap         = 6.0f * scale;
    float const       availW      = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const       halfW       = (availW - gap) * 0.5f;
    std::string const firstLabel  = buildButtonLabel(first.label, first.icon);
    std::string const secondLabel = buildButtonLabel(second.label, second.icon);
    float const       padX        = 16.0f * scale;
    float const       firstNeed   = ImGui::CalcTextSize(firstLabel.c_str(), nullptr, true).x + padX;
    float const       secondNeed  = ImGui::CalcTextSize(secondLabel.c_str(), nullptr, true).x + padX;

    int clicked = -1;
    if (firstNeed <= halfW && secondNeed <= halfW) {
        if (drawStyledButton(firstLabel, std::floor(halfW), first.enabled, first.variant, first.tooltip)) clicked = 0;
        ImGui::SameLine(0.0f, gap);
        if (drawStyledButton(secondLabel, -1.0f, second.enabled, second.variant, second.tooltip)) clicked = 1;
    } else {
        if (drawStyledButton(firstLabel, -1.0f, first.enabled, first.variant, first.tooltip)) clicked = 0;
        if (drawStyledButton(secondLabel, -1.0f, second.enabled, second.variant, second.tooltip)) clicked = 1;
    }
    return clicked;
}

bool choiceChip(char const* label, bool active, float width, ImU32 accent) {
    float const scale  = uiScale();
    float const height = std::round(ImGui::GetFontSize() + 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kButtonRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * scale, 2.0f * scale});
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(accent, 0x44));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(accent, 0x5c));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(accent, 0x72));
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kIconHighlight);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kButtonSubtle);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Border, theme::kButtonSubtleBorder);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
    }
    bool const clicked = ImGui::Button(label, {width, height});
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    return clicked;
}

} // namespace playback::editor::ui::property

#include "PropertyControls.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
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

float availContentWidth() { return std::max(1.0f, ImGui::GetContentRegionAvail().x); }

ImU32 brightenAccent(ImU32 color) {
    auto const mix = [](uint32_t channel) { return static_cast<int>(channel + ((255u - channel) * 35u) / 100u); };
    return IM_COL32(
        mix((color >> IM_COL32_R_SHIFT) & 0xffu),
        mix((color >> IM_COL32_G_SHIFT) & 0xffu),
        mix((color >> IM_COL32_B_SHIFT) & 0xffu),
        0xff
    );
}

struct StatusPillLayout {
    ImFont* font{};
    float   fontSize{};
    float   padX{};
    float   padY{};
    ImVec2  textSize{};
    ImVec2  size{};
};

StatusPillLayout measureStatusPill(std::string_view text) {
    float const   scale    = uiScale();
    ImFont* const font     = ImGui::GetFont();
    float const   fontSize = std::max(10.0f, std::round(ImGui::GetFontSize() * 0.84f));
    float const   padX     = 6.0f * scale;
    float const   padY     = 2.0f * scale;
    ImVec2 const  textSize = text.empty()
                               ? ImVec2{0.0f, fontSize}
                               : font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
    ImVec2 const  size{std::ceil(textSize.x + padX * 2.0f), std::ceil(fontSize + padY * 2.0f)};
    return {font, fontSize, padX, padY, textSize, size};
}

void drawStatusPill(
    ImDrawList*      drawList,
    ImVec2           minPos,
    std::string_view text,
    ImU32            accentColor,
    float            maxWidth = -1.0f
) {
    if (text.empty()) return;
    StatusPillLayout const layout = measureStatusPill(text);
    float const  width    = (maxWidth > 0.0f) ? std::min(layout.size.x, std::max(1.0f, maxWidth)) : layout.size.x;
    float const  height   = layout.size.y;
    ImVec2 const maxPos   = {minPos.x + width, minPos.y + height};
    float const  rounding = height * 0.5f;

    drawList->AddRectFilled(minPos, maxPos, theme::withAlpha(accentColor, 0x2a), rounding);
    drawList->AddRect(minPos, maxPos, theme::withAlpha(accentColor, 0x78), rounding, 0, 1.0f);

    float const textLeft  = minPos.x + layout.padX;
    float const textRight = std::max(textLeft + 1.0f, maxPos.x - layout.padX);
    float const textY     = minPos.y + std::floor((height - layout.textSize.y) * 0.5f);
    drawList->PushClipRect({textLeft, minPos.y}, {textRight, maxPos.y}, true);
    drawList->AddText(
        layout.font,
        layout.fontSize,
        {textLeft, textY},
        brightenAccent(accentColor),
        text.data(),
        text.data() + text.size()
    );
    drawList->PopClipRect();
}

void drawSwitchControl(ImDrawList* drawList, ImVec2 minPos, ImVec2 size, bool value, bool hovered, bool disabled) {
    float const  scale    = uiScale();
    float const  rounding = size.y * 0.5f;
    ImVec2 const maxPos   = {minPos.x + size.x, minPos.y + size.y};

    ImU32 trackFill   = value ? (hovered && !disabled ? theme::kButtonPrimaryHover : theme::kAccent)
                              : (hovered && !disabled ? theme::kButtonHover : theme::kInputBgActive);
    ImU32 trackBorder = value ? theme::kButtonPrimaryBorder : theme::kButtonBorder;
    ImU32 knobColor   = value ? theme::kIconHighlight : theme::kTextDim;
    if (disabled) {
        trackFill   = theme::withAlpha(trackFill, 0x60);
        trackBorder = theme::withAlpha(trackBorder, 0x60);
        knobColor   = theme::withAlpha(knobColor, 0x70);
    }

    drawList->AddRectFilled(minPos, maxPos, trackFill, rounding);
    drawList->AddRect(minPos, maxPos, trackBorder, rounding, 0, 1.0f);

    float const pad   = 2.0f * scale;
    float const knobR = std::max(2.0f, (size.y - pad * 2.0f) * 0.5f);
    float const knobX = value ? (maxPos.x - pad - knobR) : (minPos.x + pad + knobR);
    float const knobY = minPos.y + size.y * 0.5f;
    drawList->AddCircleFilled({knobX, knobY}, knobR, knobColor);
}

void drawChevron(ImDrawList* drawList, ImVec2 center, float radius, bool open, ImU32 color, float thickness) {
    std::array<ImVec2, 3> pts{};
    if (open) {
        pts = {
            ImVec2{center.x - radius, center.y - radius * 0.45f},
            ImVec2{center.x,          center.y + radius * 0.55f},
            ImVec2{center.x + radius, center.y - radius * 0.45f},
        };
    } else {
        pts = {
            ImVec2{center.x - radius * 0.45f, center.y - radius},
            ImVec2{center.x + radius * 0.55f, center.y         },
            ImVec2{center.x - radius * 0.45f, center.y + radius},
        };
    }
    drawList->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, ImDrawFlags_None, thickness);
}

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
    case ButtonVariant::Warning:
        return {
            theme::kButtonWarning,
            theme::kButtonWarningHover,
            theme::kButtonWarningActive,
            theme::kButtonWarningBorder,
            theme::kButtonWarningText,
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
    float const   scale         = uiScale();
    float const   height        = std::round(ImGui::GetFontSize() + 11.0f * scale);
    float const   resolvedWidth = (width < 0.0f) ? availContentWidth() : width;
    ButtonPalette palette       = paletteFor(variant);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kButtonRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f * scale, 4.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_Button, palette.fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.active);
    ImGui::PushStyleColor(ImGuiCol_Border, palette.border);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.text);
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(fullLabel.c_str(), {resolvedWidth, height});
    ImGui::EndDisabled();
    if (tooltip && *tooltip != '\0') widgets::itemTooltip(tooltip);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    return clicked;
}

} // namespace

void beginInspector(std::string_view title, std::string_view objectName) {
    beginInspector(title, objectName, InspectorHeaderOptions{});
}

void beginInspector(std::string_view title, std::string_view subtitle, InspectorHeaderOptions const& options) {
    float const  scale    = uiScale();
    float const  fontSize = ImGui::GetFontSize();
    float const  padY     = 7.0f * scale;
    float const  lineGap  = 3.0f * scale;
    bool const   hasLine2 = !subtitle.empty() || !options.badgeText.empty();
    float const  minSlotH = options.renderRightSlot ? (metrics::iconButton() + 8.0f * scale) : 0.0f;
    float const  textH =
        hasLine2 ? std::round(fontSize * 2.0f + padY * 2.0f + lineGap) : std::round(fontSize + padY * 2.0f);
    float const  height   = std::max(textH, std::round(minSlotH));
    float const  width    = availContentWidth();
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
        options.accentColor,
        barW * 0.5f
    );

    ImGui::Dummy({width, height});

    float const textX     = origin.x + 12.0f * scale;
    float       textRight = std::max(textX + 1.0f, maximum.x - 8.0f * scale);

    if (options.renderRightSlot) {
        float const rightPad  = 8.0f * scale;
        float const reservedW = options.rightSlotWidth > 0.0f ? options.rightSlotWidth : metrics::iconButton();
        float const slotX     = std::max(textX + 24.0f * scale, maximum.x - rightPad - reservedW);
        float const slotY     = origin.y + std::floor((height - metrics::iconButton()) * 0.5f);
        ImGui::SetCursorScreenPos({slotX, slotY});
        ImGui::PushID("##InspectorHeaderRight");
        ImGui::BeginGroup();
        options.renderRightSlot();
        ImGui::Dummy({0.0f, 0.0f});
        ImGui::EndGroup();
        ImGui::PopID();
        textRight = std::max(textX + 1.0f, ImGui::GetItemRectMin().x - 6.0f * scale);
    }

    float const titleY = hasLine2 ? (origin.y + padY) : (origin.y + std::floor((height - fontSize) * 0.5f));
    drawList->PushClipRect({textX, origin.y}, {textRight, maximum.y}, true);
    drawList->AddText({textX, titleY}, theme::kIconActive, title.data(), title.data() + title.size());

    if (hasLine2) {
        float const line2Y = origin.y + padY + fontSize + lineGap;
        float       line2X = textX;
        if (!options.badgeText.empty() && line2X < textRight) {
            StatusPillLayout const pillLayout = measureStatusPill(options.badgeText);
            float const            maxPillW   = std::max(1.0f, textRight - line2X);
            float const            pillW      = std::min(pillLayout.size.x, maxPillW);
            float const            pillY      = line2Y + std::floor((fontSize - pillLayout.size.y) * 0.5f);
            drawStatusPill(drawList, {line2X, pillY}, options.badgeText, options.badgeColor, maxPillW);
            line2X += pillW + 6.0f * scale;
        }
        float const minSubtitleW = options.badgeText.empty() ? 1.0f : 24.0f * scale;
        if (!subtitle.empty() && (textRight - line2X) >= minSubtitleW) {
            drawList->PushClipRect({line2X, line2Y}, {textRight, maximum.y}, true);
            drawList->AddText({line2X, line2Y}, theme::kTextDim, subtitle.data(), subtitle.data() + subtitle.size());
            drawList->PopClipRect();
        }
    }
    drawList->PopClipRect();

    ImGui::SetCursorScreenPos({origin.x, maximum.y});
    ImGui::Dummy({0.0f, 3.0f * scale});
}

float inspectorHeaderIconSlotWidth(int buttonCount) {
    if (buttonCount <= 0) return 0.0f;
    return static_cast<float>(buttonCount) * metrics::iconButton()
         + static_cast<float>(buttonCount - 1) * ImGui::GetStyle().ItemSpacing.x;
}

bool beginCard(char const* id, char const* title, CardOptions const& options) {
    char const* const safeId    = (id && *id != '\0') ? id : "##card";
    char const* const safeTitle = title ? title : "";
    if (options.toggleChanged) *options.toggleChanged = false;

    float const scale    = uiScale();
    float const fontSize = ImGui::GetFontSize();
    float const padX     = 8.0f * scale;
    float const padY     = 6.0f * scale;
    float const rounding = theme::kButtonRounding * scale;
    float const headerH  = std::round(fontSize + 12.0f * scale);
    float const availW   = availContentWidth();

    ImGui::PushID(safeId);
    ImGuiID const stateId = ImGui::GetID("##card_open");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool          open    = options.collapsible ? storage->GetBool(stateId, options.defaultOpen) : true;
    ImGui::PopID();

    auto const drawHeaderControls = [&](ImDrawList* drawList,
                                        ImVec2      cardMin,
                                        float       cardMaxX,
                                        float       headerMaxY,
                                        float       hitTopY,
                                        float       hitLeftX,
                                        float       hitRightX,
                                        bool        inChild) {
        float const switchW       = std::round(fontSize * 1.9f);
        float const switchH       = std::round(fontSize * 1.05f);
        float       rightX        = cardMaxX - padX;
        float       toggleHitMinX = hitRightX;
        ImVec2      switchMin{};
        if (options.toggleValue) {
            switchMin     = {rightX - switchW, cardMin.y + std::floor((headerH - switchH) * 0.5f)};
            toggleHitMinX = std::min(hitRightX, switchMin.x - 4.0f * scale);
            rightX        = switchMin.x - 6.0f * scale;
        }

        float const hitH          = std::max(1.0f, headerMaxY - hitTopY);
        float const headerBtnW    = std::max(1.0f, toggleHitMinX - hitLeftX);
        bool        headerHovered = false;
        ImGui::SetCursorScreenPos({hitLeftX, hitTopY});
        if (options.collapsible) {
            if (ImGui::InvisibleButton("##card_header", {headerBtnW, hitH})) {
                open = !open;
                storage->SetBool(stateId, open);
            }
            headerHovered = ImGui::IsItemHovered();
        } else {
            ImGui::Dummy({headerBtnW, hitH});
        }

        bool toggleHovered = false;
        if (options.toggleValue) {
            ImGui::SetCursorScreenPos({toggleHitMinX, hitTopY});
            ImGui::BeginDisabled(options.toggleDisabled);
            if (ImGui::InvisibleButton("##card_toggle", {std::max(1.0f, hitRightX - toggleHitMinX), hitH})) {
                *options.toggleValue = !(*options.toggleValue);
                if (options.toggleChanged) *options.toggleChanged = true;
            }
            toggleHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::EndDisabled();
        }

        ImU32 const headerBg = headerHovered ? theme::kSectionHover : theme::kInspectorCardHeader;
        if (inChild) {
            drawList->AddRectFilled(
                {cardMin.x + 1.0f, cardMin.y + 1.0f},
                {cardMaxX - 1.0f, headerMaxY},
                headerBg,
                std::max(0.0f, rounding - 1.0f),
                ImDrawFlags_RoundCornersTop
            );
            drawList->AddLine(
                {cardMin.x + 1.0f, headerMaxY},
                {cardMaxX - 1.0f, headerMaxY},
                theme::kInspectorCardBorder,
                1.0f
            );
        } else {
            drawList->AddRectFilled(cardMin, {cardMaxX, headerMaxY}, headerBg, rounding);
            drawList->AddRect(cardMin, {cardMaxX, headerMaxY}, theme::kInspectorCardBorder, rounding, 0, 1.0f);
        }

        if (options.toggleValue) {
            drawSwitchControl(
                drawList,
                switchMin,
                {switchW, switchH},
                *options.toggleValue,
                toggleHovered,
                options.toggleDisabled
            );
        }

        if (options.badgeText && *options.badgeText != '\0') {
            StatusPillLayout const pillLayout = measureStatusPill(options.badgeText);
            float const            titleGap   = 8.0f * scale;
            float const            leftMinW   = (options.collapsible ? 13.0f * scale : 0.0f) + padX + 40.0f * scale;
            float const            maxPillW   = std::max(0.0f, rightX - (cardMin.x + leftMinW + titleGap));
            float const            pillW      = std::min(pillLayout.size.x, maxPillW);
            if (pillW >= 12.0f * scale) {
                ImVec2 const pillPos{rightX - pillW, cardMin.y + std::floor((headerH - pillLayout.size.y) * 0.5f)};
                drawStatusPill(drawList, pillPos, options.badgeText, options.badgeColor, maxPillW);
                rightX = pillPos.x - titleGap;
            }
        }

        float titleX = cardMin.x + padX;
        if (options.collapsible) {
            ImU32 const caretColor = headerHovered ? theme::kIconActive : theme::kTextDim;
            drawChevron(
                drawList,
                {cardMin.x + padX + 4.0f * scale, cardMin.y + headerH * 0.5f},
                3.0f * scale,
                inChild,
                caretColor,
                std::max(1.3f, 1.4f * scale)
            );
            titleX += 13.0f * scale;
        }

        float const titleRight = std::max(titleX + 1.0f, rightX);
        float const titleY     = cardMin.y + std::floor((headerH - fontSize) * 0.5f);
        drawList->PushClipRect({titleX, cardMin.y}, {titleRight, headerMaxY}, true);
        drawList->AddText({titleX, titleY}, theme::kIconActive, safeTitle);
        drawList->PopClipRect();
    };

    if (!open) {
        ImVec2 const cardMin    = ImGui::GetCursorScreenPos();
        float const  cardMaxX   = cardMin.x + availW;
        float const  headerMaxY = cardMin.y + headerH;
        ImGui::PushID(safeId);
        drawHeaderControls(
            ImGui::GetWindowDrawList(),
            cardMin,
            cardMaxX,
            headerMaxY,
            cardMin.y,
            cardMin.x,
            cardMaxX,
            false
        );
        ImGui::PopID();
        return false;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kInspectorCard);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::kInspectorCardBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padX, padY});
    bool const childVisible = ImGui::BeginChild(
        safeId,
        {availW, 0.0f},
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );
    if (!childVisible) {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return false;
    }

    ImVec2 const cardMin    = ImGui::GetWindowPos();
    float const  cardW      = ImGui::GetWindowSize().x;
    float const  cardMaxX   = cardMin.x + cardW;
    float const  headerMaxY = cardMin.y + headerH;
    drawHeaderControls(
        ImGui::GetWindowDrawList(),
        cardMin,
        cardMaxX,
        headerMaxY,
        cardMin.y + 1.0f,
        cardMin.x + 1.0f,
        cardMaxX - 1.0f,
        true
    );

    ImGui::SetCursorScreenPos({cardMin.x + padX, headerMaxY});
    ImGui::Dummy({0.0f, std::max(0.0f, padY - ImGui::GetStyle().ItemSpacing.y)});
    ImGui::PushItemWidth(-FLT_MIN);
    return true;
}

void endCard() {
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void statusPill(char const* text, ImU32 accentColor, bool rightAligned) {
    if (!text || *text == '\0') return;
    StatusPillLayout const layout = measureStatusPill(text);
    float const            availW = availContentWidth();
    float const            pillW  = std::min(layout.size.x, availW);
    if (rightAligned && availW > pillW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - pillW));
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    drawStatusPill(ImGui::GetWindowDrawList(), origin, text, accentColor, availW);
    ImGui::Dummy({pillW, layout.size.y});
}

bool triStateCheckbox(char const* label, TriState state, char const* mixedHint) {
    float const scale     = uiScale();
    float const rowRightX = ImGui::GetCursorScreenPos().x + availContentWidth();
    bool        checked   = (state == TriState::On);
    bool        clicked   = false;

    if (state == TriState::Mixed) {
        clicked               = ImGui::Checkbox(label, &checked);
        float const  squareSz = ImGui::GetFrameHeight();
        ImVec2 const boxMin   = ImGui::GetItemRectMin();
        ImVec2 const boxMax   = {boxMin.x + squareSz, boxMin.y + squareSz};
        auto* const  drawList = ImGui::GetWindowDrawList();

        float const pad       = std::max(3.5f, squareSz * 0.24f);
        float const thickness = std::max(2.0f, std::round(squareSz * 0.14f));
        float const midY      = boxMin.y + squareSz * 0.5f;
        drawList
            ->AddRect(boxMin, boxMax, theme::withAlpha(theme::kAccent, 0x88), theme::kFrameRounding * scale, 0, 1.0f);
        drawList->AddRectFilled(
            {boxMin.x + pad, midY - thickness * 0.5f},
            {boxMax.x - pad, midY + thickness * 0.5f},
            ImGui::GetColorU32(ImGuiCol_CheckMark),
            thickness * 0.5f
        );

        if (mixedHint && *mixedHint != '\0') {
            StatusPillLayout const pillLayout = measureStatusPill(mixedHint);
            float const            itemEndX   = ImGui::GetItemRectMax().x;
            float const            remaining  = rowRightX - itemEndX - 6.0f * scale;
            if (remaining >= 24.0f * scale) {
                float const  pillW = std::min(pillLayout.size.x, remaining);
                ImVec2 const pillPos{rowRightX - pillW, boxMin.y + std::floor((squareSz - pillLayout.size.y) * 0.5f)};
                drawStatusPill(drawList, pillPos, mixedHint, theme::kTextDim, remaining);
            }
        }
    } else {
        clicked = ImGui::Checkbox(label, &checked);
    }
    return clicked;
}

void beginDangerFooter() {
    float const scale = uiScale();
    ImGui::Dummy({0.0f, 6.0f * scale});
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const  width  = availContentWidth();
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, kFineDividerColor, 1.0f);
    ImGui::Dummy({0.0f, 6.0f * scale});
    ImGui::PushID("##DangerFooter");
}

void endDangerFooter() {
    ImGui::PopID();
    ImGui::Dummy({0.0f, 2.0f * uiScale()});
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
    float const width      = availContentWidth();
    float const labelWidth = std::clamp(width * 0.36f, 64.0f * scale, 136.0f * scale);
    float const textEndX   = startX + ImGui::CalcTextSize(label).x + 10.0f * scale;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(std::max(startX + labelWidth, textEndX));
    ImGui::PushTextWrapPos(startX + width);
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
    float const  width  = availContentWidth();
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, kFineDividerColor, 1.0f);
    ImGui::Dummy({0.0f, 3.0f * scale});
}

bool actionButton(char const* label, bool enabled, ButtonVariant variant, char const* icon, char const* tooltip) {
    return drawStyledButton(buildButtonLabel(label, icon), -1.0f, enabled, variant, tooltip);
}

int actionButtonPair(ActionButtonSpec const& first, ActionButtonSpec const& second) {
    float const       scale       = uiScale();
    float const       gap         = 6.0f * scale;
    float const       availW      = availContentWidth();
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
    bool const clicked = ImGui::Button(label, {(width < 0.0f) ? availContentWidth() : width, height});
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    return clicked;
}

} // namespace playback::editor::ui::property

#include "playback/editor/ui/components/Widgets.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace playback::editor::ui::widgets {

using namespace ll::i18n_literals;

namespace {

// The icon glyphs live in the 3-byte UTF-8 range; ImGui's own decoder is internal-only.
[[nodiscard]] std::optional<unsigned int> decodeUtf8(char const* text, int& length) {
    length = 1;
    if (!text || *text == '\0') return std::nullopt;
    auto const* bytes = reinterpret_cast<unsigned char const*>(text);
    if (bytes[0] < 0x80) return bytes[0];
    if ((bytes[0] & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        length = 2;
        return static_cast<unsigned int>((bytes[0] & 0x1F) << 6 | (bytes[1] & 0x3F));
    }
    if ((bytes[0] & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
        length = 3;
        return static_cast<unsigned int>((bytes[0] & 0x0F) << 12 | (bytes[1] & 0x3F) << 6 | (bytes[2] & 0x3F));
    }
    if ((bytes[0] & 0xF8) == 0xF0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80
        && (bytes[3] & 0xC0) == 0x80) {
        length = 4;
        return static_cast<unsigned int>(
            (bytes[0] & 0x07) << 18 | (bytes[1] & 0x3F) << 12 | (bytes[2] & 0x3F) << 6 | (bytes[3] & 0x3F)
        );
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<unsigned int> decodeUtf8(char const* text) {
    int length = 1;
    return decodeUtf8(text, length);
}

struct InkBounds {
    float y0;
    float y1;
};

// Union of glyph ink over a run at the current font size. CJK ideographs sit well below the Latin
// cap height, so centring on 'H' alone leaves Chinese labels visibly low next to icons.
[[nodiscard]] std::optional<InkBounds> textInkBounds(char const* text) {
    if (!text) return std::nullopt;
    ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(ImGui::GetFontSize());
    if (!baked) return std::nullopt;
    std::optional<InkBounds> bounds;
    for (char const* cursor = text; *cursor != '\0';) {
        int        length     = 1;
        auto const codepoint  = decodeUtf8(cursor, length);
        cursor               += length;
        if (!codepoint || *codepoint == ' ') continue;
        ImFontGlyph const* glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        if (!glyph || !glyph->Visible) continue;
        if (!bounds) bounds = InkBounds{glyph->Y0, glyph->Y1};
        else {
            bounds->y0 = std::min(bounds->y0, glyph->Y0);
            bounds->y1 = std::max(bounds->y1, glyph->Y1);
        }
    }
    return bounds;
}

} // namespace

std::string formatTick(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    int const centiseconds = tick % kTicksPerSecond * (100 / kTicksPerSecond);
    std::snprintf(value, sizeof(value), "%02d:%02d.%02d", totalSeconds / 60, totalSeconds % 60, centiseconds);
    return value;
}

std::string formatTickCompact(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    std::snprintf(value, sizeof(value), "%d:%02d", totalSeconds / 60, totalSeconds % 60);
    return value;
}

float iconButtonSize() { return metrics::iconButton(); }

float iconGlyphSize() { return metrics::iconGlyph(); }

void itemTooltip(char const* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    hoverTooltip(text);
}

void hoverTooltip(char const* text) {
    if (!text) return;

    // Panels are separate top-level windows, so a tooltip window can land behind a later one. The
    // foreground draw list is always rendered last, which makes the layering deterministic.
    ImVec2 const pad      = {6.0f * metrics::scale(), 4.0f * metrics::scale()};
    ImVec2 const textSize = ImGui::CalcTextSize(text);
    ImVec2 const display  = ImGui::GetIO().DisplaySize;
    ImVec2 const mouse    = ImGui::GetMousePos();
    float const  width    = textSize.x + pad.x * 2.0f;
    float const  height   = textSize.y + pad.y * 2.0f;
    float const  offset   = 16.0f * metrics::scale();
    ImVec2       origin{mouse.x + offset, mouse.y + offset};
    if (origin.x + width > display.x) origin.x = std::max(0.0f, mouse.x - width - offset * 0.5f);
    if (origin.y + height > display.y) origin.y = std::max(0.0f, mouse.y - height - offset * 0.5f);

    // Lifted well above the panel greys with a shadow: at kBgHeader the box vanished against the panels.
    auto*        drawList = ImGui::GetForegroundDrawList();
    ImVec2 const max{origin.x + width, origin.y + height};
    float const  shadow = 3.0f * metrics::scale();
    drawList->AddRectFilled(
        {origin.x + shadow * 0.5f, origin.y + shadow},
        {max.x + shadow * 0.5f, max.y + shadow},
        theme::kTooltipShadow,
        theme::kFrameRounding * 2.0f
    );
    drawList->AddRectFilled(origin, max, theme::kTooltipBg, theme::kFrameRounding * 2.0f);
    drawList->AddRect(origin, max, theme::kTooltipBorder, theme::kFrameRounding * 2.0f);
    drawList->AddText({origin.x + pad.x, origin.y + pad.y}, theme::kText, text);
}

namespace {

struct ButtonFrame {
    ImVec2 origin;
    float  size;
    bool   clicked;
    ImU32  color;
};

// Square hit box with the shared hover/held/active fill; callers only add the glyph on top.
ButtonFrame squareButton(char const* id, bool enabled, bool active) {
    ButtonFrame frame{};
    frame.size   = iconButtonSize();
    frame.origin = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    frame.clicked      = ImGui::InvisibleButton(id, {frame.size, frame.size});
    bool const hovered = ImGui::IsItemHovered();
    bool const held    = ImGui::IsItemActive();
    ImGui::EndDisabled();

    ImU32 fill = active ? theme::withAlpha(theme::kAccent, 0x59) : IM_COL32(0, 0, 0, 0);
    if (enabled && held) fill = active ? theme::kAccent : theme::kButtonActive;
    else if (enabled && hovered)
        fill = active ? theme::withAlpha(theme::kAccent, 0x80) : theme::withAlpha(theme::kButtonHover, 0x80);

    if ((fill & IM_COL32_A_MASK) != 0) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            frame.origin,
            {frame.origin.x + frame.size, frame.origin.y + frame.size},
            fill,
            theme::kFrameRounding
        );
    }
    frame.color = !enabled ? theme::kTextDim : (hovered || active) ? theme::kIconHighlight : theme::kIconInactive;
    return frame;
}

// ImGui::Button centres by advance width, which drifts for monospaced icon glyphs; draw the glyph
// ourselves at the exact centre so every icon button looks identically aligned.
bool iconButtonImpl(char const* id, char const* icon, char const* tooltip, bool enabled, bool active) {
    ButtonFrame const frame = squareButton(id, enabled, active);
    drawIconCentred(ImGui::GetWindowDrawList(), icon, frame.origin, frame.size, frame.color);
    itemTooltip(tooltip);
    return frame.clicked;
}

// Play-style triangles: the centroid, not the bounding box, is what the eye reads as the centre.
void addTriangle(
    ImDrawList*   drawList,
    ImVec2 const& c,
    float         left,
    float         right,
    float         h,
    bool          pointsRight,
    ImU32         color
) {
    if (pointsRight) {
        drawList->AddTriangleFilled({c.x + left, c.y - h}, {c.x + left, c.y + h}, {c.x + right, c.y}, color);
    } else {
        drawList->AddTriangleFilled({c.x + right, c.y - h}, {c.x + right, c.y + h}, {c.x + left, c.y}, color);
    }
}

void addBar(ImDrawList* drawList, ImVec2 const& c, float left, float right, float h, ImU32 color) {
    float const rounding = (right - left) * 0.35f;
    drawList->AddRectFilled({c.x + left, c.y - h}, {c.x + right, c.y + h}, color, rounding);
}

} // namespace

float textOffsetInBox(float boxHeight) {
    float const fontSize = ImGui::GetFontSize();
    if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(fontSize)) {
        if (ImFontGlyph const* cap = baked->FindGlyphNoFallback('H')) {
            return std::floor((boxHeight - (cap->Y0 + cap->Y1)) * 0.5f + 0.5f);
        }
    }
    return std::floor((boxHeight - fontSize) * 0.5f + 0.5f);
}

float textOffsetInBox(float boxHeight, char const* text) {
    if (auto const ink = textInkBounds(text)) return std::floor((boxHeight - (ink->y0 + ink->y1)) * 0.5f + 0.5f);
    return textOffsetInBox(boxHeight);
}

float textYForCentre(float centreY, char const* text) {
    if (auto const ink = textInkBounds(text)) return std::floor(centreY - (ink->y0 + ink->y1) * 0.5f + 0.5f);
    return std::floor(centreY - ImGui::GetFontSize() * 0.5f + 0.5f);
}

void drawVectorIcon(ImDrawList* drawList, ImVec2 const& centre, float boxSize, ImU32 color, VectorIcon icon) {
    // 0.26 puts these marks at roughly the same ink height as the Lucide glyphs beside them.
    float const h      = boxSize * 0.26f;
    float const stroke = std::max(1.5f, boxSize * 0.045f);
    switch (icon) {
    case VectorIcon::Play:
        addTriangle(drawList, centre, -h * 0.62f, h * 1.24f, h, true, color);
        break;
    case VectorIcon::Pause:
        addBar(drawList, centre, -h * 0.9f, -h * 0.3f, h, color);
        addBar(drawList, centre, h * 0.3f, h * 0.9f, h, color);
        break;
    case VectorIcon::StepBack:
        addTriangle(drawList, centre, -h * 1.15f, -h * 0.05f, h * 0.85f, false, color);
        addTriangle(drawList, centre, h * 0.05f, h * 1.15f, h * 0.85f, false, color);
        break;
    case VectorIcon::StepForward:
        addTriangle(drawList, centre, -h * 1.15f, -h * 0.05f, h * 0.85f, true, color);
        addTriangle(drawList, centre, h * 0.05f, h * 1.15f, h * 0.85f, true, color);
        break;
    case VectorIcon::SkipStart:
        addBar(drawList, centre, -h * 1.15f, -h * 0.75f, h, color);
        addTriangle(drawList, centre, -h * 0.5f, h * 1.15f, h, false, color);
        break;
    case VectorIcon::SkipEnd:
        addTriangle(drawList, centre, -h * 1.15f, h * 0.5f, h, true, color);
        addBar(drawList, centre, h * 0.75f, h * 1.15f, h, color);
        break;
    case VectorIcon::Maximize: {
        float const r = h * 0.95f;
        drawList->AddRect({centre.x - r, centre.y - r}, {centre.x + r, centre.y + r}, color, stroke, 0, stroke);
        break;
    }
    case VectorIcon::Restore: {
        // Two offset frames, the front one filled so the overlap reads as depth rather than a grid.
        float const r = h * 0.7f;
        float const o = h * 0.35f;
        drawList->AddRect(
            {centre.x - r + o, centre.y - r - o},
            {centre.x + r + o, centre.y + r - o},
            color,
            stroke,
            0,
            stroke
        );
        drawList->AddRectFilled(
            {centre.x - r - o, centre.y - r + o},
            {centre.x + r - o, centre.y + r + o},
            theme::kBgHeader,
            stroke
        );
        drawList->AddRect(
            {centre.x - r - o, centre.y - r + o},
            {centre.x + r - o, centre.y + r + o},
            color,
            stroke,
            0,
            stroke
        );
        break;
    }
    }
}

bool vectorIconButton(char const* id, VectorIcon icon, char const* tooltip, bool active, bool enabled) {
    ButtonFrame const frame = squareButton(id, enabled, active);
    drawVectorIcon(
        ImGui::GetWindowDrawList(),
        {frame.origin.x + frame.size * 0.5f, frame.origin.y + frame.size * 0.5f},
        frame.size,
        frame.color,
        icon
    );
    itemTooltip(tooltip);
    return frame.clicked;
}

void drawIconCentred(ImDrawList* drawList, char const* icon, ImVec2 const& origin, float boxSize, ImU32 color) {
    float const iconSize = iconGlyphSize();
    // CalcTextSize reports the line box, so centring on it leaves icons visibly high. The glyph's own
    // X0/Y0/X1/Y1 are the real ink bounds, which is what should sit in the middle of the button.
    ImFontGlyph const* glyph = nullptr;
    if (auto const codepoint = decodeUtf8(icon)) {
        if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(iconSize)) {
            glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        }
    }

    ImVec2 position{origin.x, origin.y};
    if (glyph) {
        position.x += (boxSize - (glyph->X1 + glyph->X0)) * 0.5f;
        position.y += (boxSize - (glyph->Y1 + glyph->Y0)) * 0.5f;
        // Half-pixel positions blur the glyph and read as misalignment next to snapped text.
        position.y = std::floor(position.y + 0.5f);
    } else {
        ImVec2 const fallback  = ImGui::CalcTextSize(icon);
        position.x            += (boxSize - fallback.x) * 0.5f;
        position.y            += (boxSize - fallback.y) * 0.5f;
    }
    drawList->AddText(ImGui::GetFont(), iconSize, position, color, icon);
}

void drawIconAtCentre(ImDrawList* drawList, char const* icon, ImVec2 const& centre, ImU32 color) {
    float const        iconSize = iconGlyphSize();
    ImFontGlyph const* glyph    = nullptr;
    if (auto const codepoint = decodeUtf8(icon)) {
        if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(iconSize)) {
            glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        }
    }
    if (!glyph) {
        ImVec2 const fallback = ImGui::CalcTextSize(icon);
        drawIconCentred(drawList, icon, {centre.x - fallback.x * 0.5f, centre.y - fallback.y * 0.5f}, 0.0f, color);
        return;
    }
    ImVec2 const position{
        std::floor(centre.x - (glyph->X0 + glyph->X1) * 0.5f + 0.5f),
        std::floor(centre.y - (glyph->Y0 + glyph->Y1) * 0.5f + 0.5f)
    };
    drawList->AddText(ImGui::GetFont(), iconSize, position, color, icon);
}

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled) {
    return iconButtonImpl(id, icon, tooltip, enabled, false);
}

bool iconToggle(char const* id, char const* icon, char const* tooltip, bool active, bool enabled) {
    return iconButtonImpl(id, icon, tooltip, enabled, active);
}

float dropdownChipWidth(char const* label, char const* icon) {
    float const padX    = 8.0f * metrics::scale();
    float const iconBox = icon ? metrics::iconGlyph() + padX * 0.5f : 0.0f;
    return padX + iconBox + ImGui::CalcTextSize(label).x + padX * 0.5f + metrics::iconButton() * 0.5f + padX * 0.5f;
}

bool dropdownChip(char const* id, char const* label, char const* tooltip, bool enabled, char const* icon) {
    float const  height    = metrics::iconButton();
    float const  padX      = 8.0f * metrics::scale();
    float const  iconBox   = icon ? metrics::iconGlyph() : 0.0f;
    float const  caretW    = height * 0.5f;
    float const  textWidth = ImGui::CalcTextSize(label).x;
    float const  width     = dropdownChipWidth(label, icon);
    ImVec2 const origin    = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::InvisibleButton(id, {width, height});
    bool const hovered = ImGui::IsItemHovered();
    bool const held    = ImGui::IsItemActive();
    ImGui::EndDisabled();

    auto*       drawList = ImGui::GetWindowDrawList();
    ImU32 const fill     = held ? theme::kInputBgActive : hovered ? theme::kInputBgHover : theme::kInputBg;
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, fill, theme::kFrameRounding);
    ImU32 const textColor = enabled ? theme::kText : theme::kTextDim;
    ImU32 const iconColor = enabled ? (hovered ? theme::kIconHighlight : theme::kIconInactive) : theme::kTextDim;

    float x = origin.x + padX;
    if (icon) {
        drawIconAtCentre(drawList, icon, {x + iconBox * 0.5f, origin.y + height * 0.5f}, iconColor);
        x += iconBox + padX * 0.5f;
    }
    ImVec2 const textPos{x, textYForCentre(origin.y + height * 0.5f, label)};
    drawList->AddText(textPos, textColor, label);
    x += textWidth + padX * 0.5f;

    // The caret is a stroked chevron so it matches the other vector marks instead of a text glyph.
    float const  stroke = std::max(1.5f, height * 0.045f);
    ImVec2 const c{x + caretW * 0.5f, origin.y + height * 0.5f};
    float const  w = height * 0.11f;
    drawList->AddLine({c.x - w, c.y - w * 0.5f}, {c.x, c.y + w * 0.5f}, iconColor, stroke);
    drawList->AddLine({c.x, c.y + w * 0.5f}, {c.x + w, c.y - w * 0.5f}, iconColor, stroke);

    itemTooltip(tooltip);
    return clicked;
}

void noActiveProjectPlaceholder() {
    ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
}

} // namespace playback::editor::ui::widgets

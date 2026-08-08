#include "SelectReplayScreen.h"

#include "playback/editor/renderer/ImGuiRenderer.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/editor/ui/SettingsPage.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/utils/StringUtils.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <windows.h>

#include <commdlg.h>

namespace playback::screen::select_replay {

using namespace ll::i18n_literals;

namespace {

constexpr float kBaseFontSize       = 14.0f;
constexpr float kNavHeight          = 84.0f;
constexpr float kActionBarHeight    = 82.0f;
constexpr float kScreenMargin       = 24.0f;
constexpr float kCardGap            = 16.0f;
constexpr float kControlHeight      = 52.0f;
constexpr float kPanelWidthScale    = 0.86f;
constexpr float kPanelHeightScale   = 0.90f;
constexpr float kPanelMinimumMargin = 24.0f;
constexpr float kTextOpticalOffsetY = -2.0f;
constexpr float kAnimationDuration  = 0.2f;

struct NavigationLayoutConfig {
    float titleFontSize       = 28.0f;
    float backIconFontSize    = 24.0f;
    float controlFontSize     = 20.0f;
    float horizontalMargin    = 24.0f;
    float controlGap          = 12.0f;
    float viewGroupGap        = 24.0f;
    float viewSegmentMinWidth = 88.0f;
    float viewRounding        = 4.0f;
    float iconTextGap         = 10.0f;
    float buttonPadding       = 16.0f;
    float popupItemHeight     = 36.0f;
    float searchWidth         = 300.0f;
};

constexpr NavigationLayoutConfig kNavigationLayout{};

struct ContentLayoutConfig {
    float horizontalMargin    = kScreenMargin;
    float bottomMargin        = 16.0f;
    float locationHeight      = 42.0f;
    float locationVerticalGap = 12.0f;
};

constexpr ContentLayoutConfig kContentLayout{};

struct CardLayoutConfig {
    static constexpr int metadataRowCount = 3;

    float previewInset      = 2.0f;
    float horizontalPadding = 14.0f;
    float verticalPadding   = 14.0f;
    float titleFontSize     = 20.0f;
    float metadataFontSize  = 16.0f;
    // These values are actual gaps between adjacent text boxes, not the more ambiguous baseline advances.
    float titleToMetadataGap = 22.0f;
    float metadataRowGap     = 18.0f;

    [[nodiscard]] constexpr float titleToMetadataAdvance() const { return titleFontSize + titleToMetadataGap; }

    [[nodiscard]] constexpr float metadataRowAdvance() const { return metadataFontSize + metadataRowGap; }

    [[nodiscard]] constexpr float bodyHeight() const {
        return verticalPadding * 2.0f + titleFontSize + titleToMetadataGap + metadataFontSize * metadataRowCount
             + metadataRowGap * (metadataRowCount - 1);
    }
};

constexpr CardLayoutConfig kCardLayout{};

struct DetailsLayoutConfig {
    static constexpr int metadataRowCount = 7;

    float panelGap                    = 16.0f;
    float stackedBreakpoint           = 1080.0f;
    float listWidthRatio              = 0.35f;
    float listMinWidth                = 440.0f;
    float listMaxWidth                = 560.0f;
    float stackedListHeightRatio      = 0.38f;
    float stackedListMinHeight        = 280.0f;
    float stackedListMaxHeight        = 420.0f;
    float panelRounding               = 6.0f;
    float panelPadding                = 16.0f;
    float listFooterHeight            = 42.0f;
    float listFooterPaddingX          = 16.0f;
    float listItemHeight              = 120.0f;
    float listItemGap                 = 10.0f;
    float listItemHorizontalPadding   = 8.0f;
    float listTextVerticalPadding     = 12.0f;
    float thumbnailToTextGap          = 14.0f;
    float footerGroupGap              = 12.0f;
    float thumbnailWidthRatio         = 0.34f;
    float thumbnailMinWidth           = 112.0f;
    float thumbnailMaxWidth           = 168.0f;
    float listTitleFontSize           = 20.0f;
    float listMetadataFontSize        = 16.0f;
    float detailPreviewAspectRatio    = 16.0f / 9.0f;
    float detailPreviewMaxHeightRatio = 0.54f;
    float detailPreviewMinHeight      = 180.0f;
    float previewToMetadataGap        = 16.0f;
    float detailContentBottomGap      = 16.0f;
    float detailMetadataFontSize      = 18.0f;
    float metadataLabelWidth          = 150.0f;
    float metadataCellPaddingX        = 16.0f;
    float metadataCellPaddingY        = 10.0f;
    float metadataWrapReserve         = 18.0f;
    float actionButtonGap             = 12.0f;

    [[nodiscard]] constexpr float actionAreaHeight() const { return kControlHeight + panelPadding * 2.0f; }
};

constexpr DetailsLayoutConfig kDetailsLayout{};

constexpr float kFontScaleCardMeta    = kCardLayout.metadataFontSize / kBaseFontSize;
constexpr float kFontScaleSmall       = 18.0f / kBaseFontSize;
constexpr float kFontScaleCardTitle   = kCardLayout.titleFontSize / kBaseFontSize;
constexpr float kFontScaleListTitle   = kDetailsLayout.listTitleFontSize / kBaseFontSize;
constexpr float kFontScaleListMeta    = kDetailsLayout.listMetadataFontSize / kBaseFontSize;
constexpr float kFontScaleBody        = 24.0f / kBaseFontSize;
constexpr float kFontScaleLarge       = 30.0f / kBaseFontSize;
constexpr float kFontScaleNavTitle    = kNavigationLayout.titleFontSize / kBaseFontSize;
constexpr float kFontScaleNavBackIcon = kNavigationLayout.backIconFontSize / kBaseFontSize;
constexpr float kFontScaleNavControl  = kNavigationLayout.controlFontSize / kBaseFontSize;

constexpr ImU32 kColorAccent       = IM_COL32(58, 140, 240, 255);
constexpr ImU32 kColorAccentHover  = IM_COL32(78, 158, 250, 255);
constexpr ImU32 kColorBg           = IM_COL32(22, 23, 25, 255);
constexpr ImU32 kColorPanelBg      = IM_COL32(30, 32, 35, 255);
constexpr ImU32 kColorCardBg       = IM_COL32(25, 27, 29, 255);
constexpr ImU32 kColorCardSelected = IM_COL32(70, 72, 76, 255);
constexpr ImU32 kColorListSelected = IM_COL32(70, 72, 76, 255);
constexpr ImU32 kColorCardBorder   = IM_COL32(76, 80, 86, 220);
constexpr ImU32 kColorCardHover    = IM_COL32(104, 110, 120, 255);
constexpr ImU32 kColorButton       = IM_COL32(48, 50, 54, 255);
constexpr ImU32 kColorButtonHover  = IM_COL32(64, 67, 72, 255);
constexpr ImU32 kColorButtonActive = IM_COL32(78, 81, 88, 255);
constexpr ImU32 kColorPreviewBg    = IM_COL32(30, 42, 58, 255);
constexpr ImU32 kColorDanger       = IM_COL32(210, 60, 60, 255);
constexpr ImU32 kColorText         = IM_COL32(238, 240, 244, 255);
constexpr ImU32 kColorTextDim      = IM_COL32(164, 168, 176, 255);
constexpr ImU32 kColorBackdrop     = IM_COL32(0, 0, 0, 88);

float cardPreviewHeight(float width) { return std::max(0.0f, width - kCardLayout.previewInset * 2.0f) * 9.0f / 16.0f; }

float cardHeight(float width) { return kCardLayout.previewInset + cardPreviewHeight(width) + kCardLayout.bodyHeight(); }

std::string formatSize(std::uintmax_t bytes) {
    constexpr std::array<char const*, 4> units{"B", "KB", "MB", "GB"};
    double                               size = static_cast<double>(bytes);
    size_t                               unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << size << ' ' << units[unit];
    return stream.str();
}

std::string formatDuration(playback::editor::ReplayBrowserEntry const& replay) {
    int seconds = (replay.totalTicks > 0 ? replay.totalTicks : replay.durationTicks) / 20;
    return "playback.replayBrowser.duration"_tr(seconds / 60, seconds % 60);
}

std::string formatModifiedTime(std::filesystem::file_time_type const& time) {
    if (time == std::filesystem::file_time_type{}) return "playback.replayBrowser.unknown"_tr();
    auto const sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    std::time_t const t = std::chrono::system_clock::to_time_t(sysTime);
    std::tm           tm{};
    localtime_s(&tm, &t);
    std::array<char, 64> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &tm);
    return buf.data();
}

float textWidth(std::string_view text) {
    if (text.empty()) return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

ImU32 lerpColor(ImU32 from, ImU32 to, float amount) {
    auto channel = [from, to, amount](int shift) {
        auto const start = static_cast<float>((from >> shift) & 0xffu);
        auto const end   = static_cast<float>((to >> shift) & 0xffu);
        return static_cast<int>(std::round(start + (end - start) * amount));
    };
    return IM_COL32(channel(0), channel(8), channel(16), channel(24));
}

ImWchar firstCodepoint(char const* text) {
    auto const* bytes = reinterpret_cast<unsigned char const*>(text);
    if ((bytes[0] & 0x80u) == 0) return bytes[0];
    if ((bytes[0] & 0xe0u) == 0xc0u) {
        return static_cast<ImWchar>(((bytes[0] & 0x1fu) << 6) | (bytes[1] & 0x3fu));
    }
    return static_cast<ImWchar>(((bytes[0] & 0x0fu) << 12) | ((bytes[1] & 0x3fu) << 6) | (bytes[2] & 0x3fu));
}

ImFontGlyph const* iconGlyph(char const* icon) {
    auto* baked = ImGui::GetFontBaked();
    return baked ? baked->FindGlyphNoFallback(firstCodepoint(icon)) : nullptr;
}

float iconVisualWidth(char const* icon) {
    if (auto const* glyph = iconGlyph(icon)) return glyph->X1 - glyph->X0;
    return ImGui::CalcTextSize(icon).x;
}

float iconDrawX(char const* icon, float visualLeft) {
    if (auto const* glyph = iconGlyph(icon)) return visualLeft - glyph->X0;
    return visualLeft;
}

float centeredIconY(char const* icon, float minimumY, float maximumY) {
    if (auto const* glyph = iconGlyph(icon)) {
        return minimumY + (maximumY - minimumY - (glyph->Y1 - glyph->Y0)) * 0.5f - glyph->Y0;
    }
    return minimumY + (maximumY - minimumY - ImGui::CalcTextSize(icon).y) * 0.5f;
}

float centeredIconX(char const* icon, float minimumX, float maximumX) {
    float const visualLeft = minimumX + (maximumX - minimumX - iconVisualWidth(icon)) * 0.5f;
    return iconDrawX(icon, visualLeft);
}

float iconLabelWidth(char const* icon, std::string_view text, float gap = 6.0f, char const* trailingIcon = nullptr) {
    float const leadingGap    = text.empty() ? 0.0f : gap;
    float const trailingGap   = trailingIcon ? gap : 0.0f;
    float const trailingWidth = trailingIcon ? iconVisualWidth(trailingIcon) : 0.0f;
    return iconVisualWidth(icon) + leadingGap + textWidth(text) + trailingGap + trailingWidth;
}

float toolbarButtonWidth(char const* icon, std::string_view text, char const* trailingIcon = nullptr) {
    return std::ceil(
        iconLabelWidth(icon, text, kNavigationLayout.iconTextGap, trailingIcon) + kNavigationLayout.buttonPadding * 2.0f
    );
}

bool beginAnchoredPopup(char const* popupId, ImVec2 buttonMinimum, ImVec2 buttonMaximum) {
    constexpr float popupGap      = 4.0f;
    constexpr float popupMinWidth = 200.0f;
    float const     popupWidth    = std::max(buttonMaximum.x - buttonMinimum.x, popupMinWidth);

    ImGui::SetNextWindowPos({buttonMinimum.x, buttonMaximum.y + popupGap}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({popupWidth, 0.0f}, {popupWidth, FLT_MAX});
    return ImGui::BeginPopup(popupId, ImGuiWindowFlags_NoMove);
}

void drawClippedText(std::string_view text, ImVec2 position, float clipRight, ImU32 color) {
    if (text.empty() || clipRight <= position.x) return;
    ImVec4 const clip{position.x, position.y, clipRight, position.y + ImGui::GetFontSize() + 2.0f};
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        position,
        color,
        text.data(),
        text.data() + text.size(),
        0.0f,
        &clip
    );
}

void drawIconTextLine(
    char const*      icon,
    std::string_view text,
    ImVec2           position,
    float            clipRight,
    ImU32            color,
    float            gap = 6.0f
) {
    float const lineBottom = position.y + ImGui::GetFontSize();
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {iconDrawX(icon, position.x), centeredIconY(icon, position.y, lineBottom)},
        color,
        icon
    );
    drawClippedText(
        text,
        {position.x + iconVisualWidth(icon) + gap, position.y + kTextOpticalOffsetY},
        clipRight,
        color
    );
}

std::string sortLabel(BrowserSort sort) {
    switch (sort) {
    case BrowserSort::ReplayName:
        return "playback.replayBrowser.sort.name"_tr();
    case BrowserSort::WorldName:
        return "playback.replayBrowser.sort.world"_tr();
    case BrowserSort::Duration:
        return "playback.replayBrowser.sort.duration"_tr();
    case BrowserSort::FileSize:
        return "playback.replayBrowser.sort.size"_tr();
    default:
        return "playback.replayBrowser.sort.date"_tr();
    }
}

int compareText(std::string_view left, std::string_view right) {
    auto lower = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    };
    auto const normalizedLeft  = lower(left);
    auto const normalizedRight = lower(right);
    if (normalizedLeft < normalizedRight) return -1;
    if (normalizedLeft > normalizedRight) return 1;
    return 0;
}

std::string filterLabel(ReplayFilter filter) {
    switch (filter) {
    case ReplayFilter::Playable:
        return "playback.replayBrowser.filter.playable"_tr();
    case ReplayFilter::Broken:
        return "playback.replayBrowser.filter.broken"_tr();
    default:
        return "playback.replayBrowser.filter.all"_tr();
    }
}

void tooltip(char const* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

void styleButton(bool active = false) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kColorAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorAccent);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kColorButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
    }
}

void popButtonStyle() { ImGui::PopStyleColor(3); }

void drawCenteredIconLabel(
    ImVec2           minimum,
    ImVec2           maximum,
    char const*      icon,
    std::string_view text,
    char const*      trailingIcon = nullptr
) {
    float const iconWidth   = iconVisualWidth(icon);
    float const leadingGap  = text.empty() ? 0.0f : kNavigationLayout.iconTextGap;
    float const labelWidth  = textWidth(text);
    float const trailingGap = trailingIcon ? kNavigationLayout.iconTextGap : 0.0f;
    float const groupWidth  = iconLabelWidth(icon, text, kNavigationLayout.iconTextGap, trailingIcon);
    float const groupX      = minimum.x + (maximum.x - minimum.x - groupWidth) * 0.5f;
    float const iconX =
        text.empty() && !trailingIcon ? centeredIconX(icon, minimum.x, maximum.x) : iconDrawX(icon, groupX);

    ImDrawList* const draw  = ImGui::GetWindowDrawList();
    ImU32 const       color = ImGui::GetColorU32(kColorText);
    draw->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {iconX, centeredIconY(icon, minimum.y, maximum.y)},
        color,
        icon
    );
    if (!text.empty()) {
        ImVec2 const textSize = ImGui::CalcTextSize(text.data(), text.data() + text.size());
        draw->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {
                groupX + iconWidth + leadingGap,
                minimum.y + (maximum.y - minimum.y - textSize.y) * 0.5f + kTextOpticalOffsetY,
            },
            color,
            text.data(),
            text.data() + text.size()
        );
    }
    if (trailingIcon) {
        float const trailingX = groupX + iconWidth + leadingGap + labelWidth + trailingGap;
        draw->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {iconDrawX(trailingIcon, trailingX), centeredIconY(trailingIcon, minimum.y, maximum.y)},
            color,
            trailingIcon
        );
    }
}

bool toolbarButton(
    char const*      id,
    char const*      icon,
    std::string_view text,
    float            width,
    bool             active       = false,
    char const*      trailingIcon = nullptr
) {
    styleButton(active);
    bool const clicked = ImGui::Button(id, {width, kControlHeight});
    popButtonStyle();

    drawCenteredIconLabel(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), icon, text, trailingIcon);
    return clicked;
}

bool popupIconMenuItem(char const* id, char const* icon, std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kColorButtonActive);
    bool const clicked = ImGui::Selectable(
        id,
        false,
        ImGuiSelectableFlags_None,
        {ImGui::GetContentRegionAvail().x, kNavigationLayout.popupItemHeight}
    );
    ImGui::PopStyleColor(2);

    drawCenteredIconLabel(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), icon, text);
    return clicked;
}

bool viewToggleButton(
    char const*      id,
    std::string_view overviewText,
    std::string_view detailsText,
    float            segmentWidth,
    bool             overviewSelected
) {
    ImVec2 const size{segmentWidth * 2.0f, kControlHeight};
    bool const   clicked  = ImGui::InvisibleButton(id, size);
    bool const   hovered  = ImGui::IsItemHovered();
    bool const   held     = ImGui::IsItemActive();
    ImVec2 const minimum  = ImGui::GetItemRectMin();
    ImVec2 const maximum  = ImGui::GetItemRectMax();
    float const  dividerX = minimum.x + segmentWidth;

    auto const backgroundFor = [hovered, held](bool selected) {
        if (selected) return hovered ? kColorButtonHover : kColorCardSelected;
        if (held) return kColorButtonActive;
        if (hovered) return kColorButtonHover;
        return kColorButton;
    };

    ImDrawList* const draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(
        minimum,
        {dividerX, maximum.y},
        backgroundFor(overviewSelected),
        kNavigationLayout.viewRounding,
        ImDrawFlags_RoundCornersLeft
    );
    draw->AddRectFilled(
        {dividerX, minimum.y},
        maximum,
        backgroundFor(!overviewSelected),
        kNavigationLayout.viewRounding,
        ImDrawFlags_RoundCornersRight
    );

    auto const drawLabel = [draw, minimum, segmentWidth](std::string_view text, float segmentMinimumX) {
        ImVec2 const textSize = ImGui::CalcTextSize(text.data(), text.data() + text.size());
        draw->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {
                segmentMinimumX + (segmentWidth - textSize.x) * 0.5f,
                minimum.y + (kControlHeight - textSize.y) * 0.5f + kTextOpticalOffsetY,
            },
            kColorText,
            text.data(),
            text.data() + text.size()
        );
    };
    drawLabel(overviewText, minimum.x);
    drawLabel(detailsText, dividerX);

    draw->AddRect(minimum, maximum, kColorCardBorder, kNavigationLayout.viewRounding, ImDrawFlags_RoundCornersAll);
    draw->AddLine({dividerX, minimum.y}, {dividerX, maximum.y}, kColorCardBorder);
    return clicked;
}

bool backButton(char const* id, char const* icon, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorButtonActive);
    bool const clicked = ImGui::Button(id, {size, size});
    ImGui::PopStyleColor(3);

    ImVec2 const minimum = ImGui::GetItemRectMin();
    ImVec2 const maximum = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {
            centeredIconX(icon, minimum.x, maximum.x),
            centeredIconY(icon, minimum.y, maximum.y),
        },
        ImGui::GetColorU32(kColorText),
        icon
    );
    return clicked;
}

bool searchInput(
    char const*      id,
    char const*      icon,
    std::string_view hint,
    char*            buffer,
    std::size_t      bufferSize,
    float            width
) {
    ImVec2 const minimum = ImGui::GetCursorScreenPos();
    ImVec2 const maximum{minimum.x + width, minimum.y + kControlHeight};
    bool const   hovered = ImGui::IsMouseHoveringRect(minimum, maximum);
    ImU32 const  bg      = ImGui::GetColorU32(hovered ? kColorButtonHover : kColorButton);
    ImDrawList*  draw    = ImGui::GetWindowDrawList();
    draw->AddRectFilled(minimum, maximum, bg, ImGui::GetStyle().FrameRounding);

    constexpr float iconAreaWidth = 44.0f;
    ImGui::InvisibleButton("##search-leading", {iconAreaWidth, kControlHeight});
    bool const focusFromIcon = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    draw->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {
            centeredIconX(icon, minimum.x, minimum.x + iconAreaWidth),
            centeredIconY(icon, minimum.y, maximum.y),
        },
        ImGui::GetColorU32(kColorTextDim),
        icon
    );

    ImGui::SetCursorScreenPos({minimum.x + iconAreaWidth, minimum.y + kTextOpticalOffsetY});
    ImGui::SetNextItemWidth(width - iconAreaWidth - 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0f, (kControlHeight - ImGui::GetFontSize()) * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kColorTextDim);
    if (focusFromIcon) ImGui::SetKeyboardFocusHere();
    std::string const hintText{hint};
    bool const        changed = ImGui::InputTextWithHint(id, hintText.c_str(), buffer, bufferSize);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return changed;
}

} // namespace

SelectReplayScreen& SelectReplayScreen::getInstance() {
    static SelectReplayScreen instance;
    return instance;
}

std::vector<playback::editor::ReplayBrowserEntry> const& SelectReplayScreen::replays() const {
    static std::vector<playback::editor::ReplayBrowserEntry> const empty;
    return mState && mState->snapshot ? mState->snapshot->replays : empty;
}

void SelectReplayScreen::submit(playback::editor::EditorAction action) const {
    if (mSubmit) (*mSubmit)(std::move(action));
}

void SelectReplayScreen::updateAnimations() {
    float const delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    if (mViewTransitionActive) {
        mViewTransition = std::min(1.0f, mViewTransition + delta / kAnimationDuration);
        if (mViewTransition >= 1.0f) mViewTransitionActive = false;
    }
}

float SelectReplayScreen::animate(std::string_view key, float target) {
    auto& value = mAnimationValues[std::string(key)];
    float const delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    float const step  = std::min(1.0f, delta / kAnimationDuration);
    value += (target - value) * (step * 1.5f);
    if (std::abs(target - value) < 0.001f) value = target;
    return value;
}

void SelectReplayScreen::syncSnapshot() {
    auto const revision = mState && mState->snapshot ? mState->snapshot->revision : 0;
    if (revision == mSnapshotRevision) return;
    mSnapshotRevision = revision;
    mSelectedIds.clear();
    mSelectionAnchor.reset();
    mShowDeleteDialog = false;
    rebuildVisible();
}

void SelectReplayScreen::rebuildVisible() {
    auto const& items = replays();
    mVisible.clear();
    mVisible.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto const& replay = items[index];
        bool        pass   = replay.matches(mSearch);
        if (pass && mFilter == ReplayFilter::Playable) pass = replay.canOpen;
        if (pass && mFilter == ReplayFilter::Broken) pass = !replay.canOpen;
        if (pass) mVisible.push_back(index);
    }

    auto less = [&](std::size_t leftIndex, std::size_t rightIndex) {
        auto const& left  = items[leftIndex];
        auto const& right = items[rightIndex];
        int         result{};
        switch (mSort) {
        case BrowserSort::ReplayName:
            result = compareText(left.displayName(), right.displayName());
            break;
        case BrowserSort::WorldName:
            result = compareText(left.worldName, right.worldName);
            break;
        case BrowserSort::Duration: {
            auto const leftTicks  = left.totalTicks > 0 ? left.totalTicks : left.durationTicks;
            auto const rightTicks = right.totalTicks > 0 ? right.totalTicks : right.durationTicks;
            result                = leftTicks < rightTicks ? -1 : leftTicks > rightTicks ? 1 : 0;
            break;
        }
        case BrowserSort::FileSize:
            result = left.fileSize < right.fileSize ? -1 : left.fileSize > right.fileSize ? 1 : 0;
            break;
        case BrowserSort::LastModified:
        default:
            result = left.lastModified < right.lastModified ? -1 : left.lastModified > right.lastModified ? 1 : 0;
            break;
        }
        if (result == 0) result = compareText(left.replayId, right.replayId);
        return mDescending ? result > 0 : result < 0;
    };
    std::stable_sort(mVisible.begin(), mVisible.end(), less);
}

void SelectReplayScreen::select(std::string_view replayId, std::size_t visibleIndex, bool toggle, bool range) {
    if (range && mSelectionAnchor && *mSelectionAnchor < mVisible.size()) {
        auto const first = std::min(*mSelectionAnchor, visibleIndex);
        auto const last  = std::max(*mSelectionAnchor, visibleIndex);
        for (auto index = first; index <= last; ++index) mSelectedIds.insert(replays()[mVisible[index]].replayId);
    } else if (toggle) {
        if (!mSelectedIds.erase(std::string(replayId))) mSelectedIds.insert(std::string(replayId));
        mSelectionAnchor = visibleIndex;
    } else {
        mSelectedIds     = {std::string(replayId)};
        mSelectionAnchor = visibleIndex;
    }
}

std::optional<playback::editor::ReplayBrowserEntry const*> SelectReplayScreen::selectedReplay() const {
    if (mSelectedIds.size() != 1) return std::nullopt;
    auto const& items = replays();
    auto        it    = std::find_if(items.begin(), items.end(), [&](auto const& replay) {
        return mSelectedIds.contains(replay.replayId);
    });
    return it == items.end() ? std::nullopt : std::optional<playback::editor::ReplayBrowserEntry const*>{&*it};
}

void SelectReplayScreen::openSelected() {
    auto replay = selectedReplay();
    if (!replay || !(*replay)->canOpen) return;
    playback::editor::EditorAction action{playback::editor::EditorActionType::OpenReplay};
    action.path     = (*replay)->path;
    action.replayId = (*replay)->replayId;
    submit(std::move(action));
}

void SelectReplayScreen::importReplay() {
    std::array<wchar_t, 32768> file{};
    std::wstring               filter =
        ll::string_utils::str2wstr("playback.replayBrowser.openDialog.replayFiles"_tr()) + L" (*.playback;*.zip)";
    filter.push_back(L'\0');
    filter += L"*.playback;*.zip";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile   = file.data();
    dialog.nMaxFile    = static_cast<DWORD>(file.size());
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    playback::editor::EditorAction action{playback::editor::EditorActionType::ImportReplay};
    action.path = file.data();
    submit(std::move(action));
}

void SelectReplayScreen::draw(playback::editor::ReplayBrowserState const& state, SubmitAction const& submitAction) {
    if (!state.visible) return;
    mState  = &state;
    mSubmit = &submitAction;
    syncSnapshot();
    updateAnimations();
    playback::editor::ui::EditorTheme theme;
    theme.apply();
    auto const& io = ImGui::GetIO();

    ImVec2 const panelSize{
        std::max(0.0f, std::min(io.DisplaySize.x * kPanelWidthScale, io.DisplaySize.x - kPanelMinimumMargin * 2.0f)),
        std::max(0.0f, std::min(io.DisplaySize.y * kPanelHeightScale, io.DisplaySize.y - kPanelMinimumMargin * 2.0f)),
    };
    ImVec2 const panelMinimum{
        (io.DisplaySize.x - panelSize.x) * 0.5f,
        (io.DisplaySize.y - panelSize.y) * 0.5f,
    };
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin(
        "##replay-browser",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoBackground
    );
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(kFontScaleBody);

    ImDrawList* const draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, kColorBackdrop);

    ImGui::SetCursorPos(panelMinimum);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorBg);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorCardBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(
        "##replay-browser-panel",
        panelSize,
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::SetWindowFontScale(kFontScaleBody);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        submit({playback::editor::EditorActionType::CloseReplayBrowser});
    }

    ImGui::BeginDisabled(state.busy());
    drawNavigation();
    ImGui::Separator();

    float const actionHeight = (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) ? kActionBarHeight : 0.0f;
    ImGui::BeginChild("##content", {0.0f, -actionHeight}, false);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, mViewTransitionActive ? mViewTransition : 1.0f);
    if (mViewMode == ViewMode::Grid) drawGrid();
    else drawDetails();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    if (mViewMode == ViewMode::Grid && !mSelectedIds.empty()) drawActionBar();
    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::SetWindowFontScale(kFontScaleBody);
    drawDeleteDialog();
    drawRenameDialog();

    ImGui::End();
    mState  = nullptr;
    mSubmit = nullptr;
}

void SelectReplayScreen::drawNavigation() {
    ImGui::BeginChild(
        "##header",
        {0.0f, kNavHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    float const width   = ImGui::GetWindowWidth();
    float const margin  = kNavigationLayout.horizontalMargin;
    float const gap     = kNavigationLayout.controlGap;
    float const iconW   = kControlHeight;
    float const searchW = kNavigationLayout.searchWidth;

    std::string const importLabelText   = "playback.replayBrowser.navigation.import"_tr();
    std::string const filterLabelText   = "playback.replayBrowser.navigation.filter"_tr();
    std::string const sortLabelText     = "playback.replayBrowser.navigation.sort"_tr();
    std::string const overviewLabelText = "playback.replayBrowser.navigation.overview"_tr();
    std::string const detailsLabelText  = "playback.replayBrowser.navigation.details"_tr();
    std::string const title             = "playback.replayBrowser.title"_tr();

    ImGui::SetWindowFontScale(kFontScaleNavTitle);
    ImVec2 const titleSize  = ImGui::CalcTextSize(title.c_str());
    float const  titleX     = margin + iconW + gap;
    float const  titleRight = titleX + titleSize.x;

    ImGui::SetWindowFontScale(kFontScaleNavControl);
    float const importW      = toolbarButtonWidth(ICON_EXPORT, importLabelText);
    float const sortW        = toolbarButtonWidth(ICON_SORT, sortLabelText, ICON_CHEVRON_DOWN);
    float const viewSegmentW = std::max(
        kNavigationLayout.viewSegmentMinWidth,
        std::ceil(
            std::max(textWidth(overviewLabelText), textWidth(detailsLabelText)) + kNavigationLayout.buttonPadding * 2.0f
        )
    );
    float const viewW  = viewSegmentW * 2.0f;
    float const ctrlW  = searchW + importW + sortW + iconW + viewW + gap * 3.0f + kNavigationLayout.viewGroupGap;
    float const startX = std::max(titleRight + margin, width - margin - ctrlW);
    float const y      = (kNavHeight - kControlHeight) * 0.5f;

    ImGui::SetWindowFontScale(kFontScaleNavBackIcon);
    ImGui::SetCursorPos({margin, y});
    if (backButton("##back", ICON_BACK, iconW)) submit({playback::editor::EditorActionType::CloseReplayBrowser});
    ImGui::SetWindowFontScale(kFontScaleNavControl);
    tooltip("playback.replayBrowser.navigation.back"_tr().c_str());

    ImGui::SetWindowFontScale(kFontScaleNavTitle);
    ImGui::SetCursorPos({titleX, (kNavHeight - titleSize.y) * 0.5f + kTextOpticalOffsetY});
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(kFontScaleNavControl);

    float x = startX;

    ImGui::SetCursorPos({x, y});
    std::array<char, 256> search{};
    std::copy_n(mSearch.data(), std::min(mSearch.size(), search.size() - 1), search.data());
    std::string const searchHint = "playback.replayBrowser.navigation.search"_tr();
    if (searchInput("##search", ICON_SEARCH, searchHint, search.data(), search.size(), searchW)) {
        mSearch = search.data();
        rebuildVisible();
    }
    x += searchW + gap;

    ImGui::SetCursorPos({x, y});
    if (toolbarButton("##import", ICON_EXPORT, importLabelText, importW)) importReplay();
    tooltip("playback.replayBrowser.navigation.importTooltip"_tr().c_str());
    x += importW + gap;

    // Keep filtering and sorting in one fixed-label menu so the toolbar stays compact and stable.
    ImGui::SetCursorPos({x, y});
    bool const sortClicked =
        toolbarButton("##sort", ICON_SORT, sortLabelText, sortW, mFilter != ReplayFilter::All, ICON_CHEVRON_DOWN);
    ImVec2 const sortButtonMinimum = ImGui::GetItemRectMin();
    ImVec2 const sortButtonMaximum = ImGui::GetItemRectMax();
    if (sortClicked) ImGui::OpenPopup("##sort-menu");
    std::string const sortTooltip = filterLabelText + " / " + sortLabelText;
    tooltip(sortTooltip.c_str());
    if (beginAnchoredPopup("##sort-menu", sortButtonMinimum, sortButtonMaximum)) {
        ImGui::SetWindowFontScale(kFontScaleNavControl);
        ImGui::TextDisabled("%s", filterLabelText.c_str());
        for (auto filter : {ReplayFilter::All, ReplayFilter::Playable, ReplayFilter::Broken}) {
            if (ImGui::MenuItem(filterLabel(filter).c_str(), nullptr, mFilter == filter)) {
                mFilter = filter;
                rebuildVisible();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", sortLabelText.c_str());
        for (auto sort :
             {BrowserSort::LastModified,
              BrowserSort::ReplayName,
              BrowserSort::WorldName,
              BrowserSort::Duration,
              BrowserSort::FileSize}) {
            if (ImGui::MenuItem(sortLabel(sort).c_str(), nullptr, mSort == sort)) {
                mSort = sort;
                rebuildVisible();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("playback.replayBrowser.sort.ascending"_tr().c_str(), nullptr, !mDescending)) {
            mDescending = false;
            rebuildVisible();
        }
        if (ImGui::MenuItem("playback.replayBrowser.sort.descending"_tr().c_str(), nullptr, mDescending)) {
            mDescending = true;
            rebuildVisible();
        }
        ImGui::EndPopup();
    }
    x += sortW + gap;

    // Keep refresh available under Settings instead of consuming another toolbar slot.
    ImGui::SetCursorPos({x, y});
    bool const   settingsClicked       = toolbarButton("##settings-button", ICON_SETTINGS, {}, iconW);
    ImVec2 const settingsButtonMinimum = ImGui::GetItemRectMin();
    ImVec2 const settingsButtonMaximum = ImGui::GetItemRectMax();
    if (settingsClicked) ImGui::OpenPopup("##settings");
    tooltip("playback.replayBrowser.navigation.settings"_tr().c_str());
    if (beginAnchoredPopup("##settings", settingsButtonMinimum, settingsButtonMaximum)) {
        ImGui::SetWindowFontScale(kFontScaleNavControl);
        std::string const refreshLabel = "playback.replayBrowser.navigation.refresh"_tr();
        if (popupIconMenuItem("##refresh-replay-list", ICON_REFRESH, refreshLabel)) {
            submit({playback::editor::EditorActionType::RefreshReplayBrowser});
        }
        if (ImGui::MenuItem("playback.settings.menu.open"_tr().c_str())) playback::editor::ui::openSettingsPage();
        ImGui::EndPopup();
    }
    x += iconW + kNavigationLayout.viewGroupGap;

    ImGui::SetCursorPos({x, y});
    if (viewToggleButton(
            "##view-mode",
            overviewLabelText,
            detailsLabelText,
            viewSegmentW,
            mViewMode == ViewMode::Grid
        )) {
        mViewMode = mViewMode == ViewMode::Grid ? ViewMode::Details : ViewMode::Grid;
        mViewTransition       = 0.0f;
        mViewTransitionActive = true;
    }
    std::string const viewTooltip = mViewMode == ViewMode::Grid
                                      ? "playback.replayBrowser.navigation.switchToDetails"_tr()
                                      : "playback.replayBrowser.navigation.switchToOverview"_tr();
    tooltip(viewTooltip.c_str());

    ImGui::EndChild();
}

void SelectReplayScreen::drawPreview(
    playback::editor::ReplayBrowserEntry const& replay,
    ImVec2                                         size,
    float                                          rounding
) {
    auto start = ImGui::GetCursorScreenPos();
    auto end   = ImVec2(start.x + size.x, start.y + size.y);
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, end, kColorPreviewBg, rounding);

    auto texture = playback::editor::renderer::gImGuiRenderer.acquireReplayThumbnailTexture(
        replay.path.string(),
        replay.thumbnailPng
    );
    if (texture) {
        // Thumbnail sources are 16:9; center-crop to the target aspect ratio without stretching.
        constexpr float sourceAspect = 16.0f / 9.0f;
        float const     targetAspect = size.x / size.y;
        ImVec2          uv0{0.0f, 0.0f};
        ImVec2          uv1{1.0f, 1.0f};
        if (targetAspect < sourceAspect) {
            float const visibleWidth = targetAspect / sourceAspect;
            uv0.x                    = (1.0f - visibleWidth) * 0.5f;
            uv1.x                    = 1.0f - uv0.x;
        } else if (targetAspect > sourceAspect) {
            float const visibleHeight = sourceAspect / targetAspect;
            uv0.y                     = (1.0f - visibleHeight) * 0.5f;
            uv1.y                     = 1.0f - uv0.y;
        }
        drawList->AddImageRounded(texture, start, end, uv0, uv1, IM_COL32_WHITE, rounding);
        ImGui::Dummy(size);
    } else {
        auto              center = ImVec2(start.x + size.x * 0.5f, start.y + size.y * 0.5f);
        std::string const msg    = "playback.replayBrowser.previewUnavailable"_tr();
        auto              ts     = ImGui::CalcTextSize(msg.c_str());
        drawList->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), kColorTextDim, msg.c_str());
        ImGui::Dummy(size);
    }
}

void SelectReplayScreen::drawCard(
    playback::editor::ReplayBrowserEntry const& replay,
    std::size_t                                 visibleIndex,
    float                                       width
) {
    bool const  selected      = mSelectedIds.contains(replay.replayId);
    float const previewWidth  = width - kCardLayout.previewInset * 2.0f;
    float const previewHeight = cardPreviewHeight(width);
    float const height        = cardHeight(width);
    float const previewBottom = kCardLayout.previewInset + previewHeight;
    float const titleY        = previewBottom + kCardLayout.verticalPadding;
    float const worldY        = titleY + kCardLayout.titleToMetadataAdvance();
    float const modifiedY     = worldY + kCardLayout.metadataRowAdvance();
    float const footerY       = modifiedY + kCardLayout.metadataRowAdvance();

    ImGui::PushID(replay.replayId.c_str());
    float const selectedAmount = animate(std::string("card-selected-") + replay.replayId, selected ? 1.0f : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorCardBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::BeginChild(
        "##card",
        {width, height},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );
    ImVec2 const cardMinimum = ImGui::GetWindowPos();
    ImVec2 const cardMaximum{cardMinimum.x + width, cardMinimum.y + height};
    ImGui::GetWindowDrawList()->AddRectFilled(cardMinimum, cardMaximum, lerpColor(kColorCardBg, kColorCardSelected, selectedAmount), 8.0f);

    ImGui::SetCursorPos({kCardLayout.previewInset, kCardLayout.previewInset});
    drawPreview(replay, {previewWidth, previewHeight}, 8.0f);

    // InvisibleButton activates on release, but ImGui reports a double-click on the second press.
    // Detect it on that press while the card is hovered.
    ImGui::SetCursorPos({0.0f, 0.0f});
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##select-card", {width, height});
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && ImGui::IsItemHovered()) {
        select(replay.replayId, visibleIndex, false, false);
        openSelected();
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        select(replay.replayId, visibleIndex, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
    }
    bool const cardHovered = ImGui::IsItemHovered();
    float const hoverAmount    = animate(std::string("card-hover-") + replay.replayId, cardHovered ? 1.0f : 0.0f);

    // Keep full file details in the info-button tooltip without crowding the card body.
    float const infoSize = 28.0f;
    float const infoX    = width - kCardLayout.horizontalPadding - infoSize;
    float const infoY    = titleY - 4.0f;
    ImGui::SetWindowFontScale(kFontScaleCardMeta);
    ImGui::SetCursorPos({infoX, infoY});
    ImGui::InvisibleButton("##card-info", {infoSize, infoSize});
    bool const infoHovered = ImGui::IsItemHovered();

    // Store fade timing in per-card ImGui state; shared state would be reset by every non-hovered card.
    double const        now         = ImGui::GetTime();
    ImGuiStorage* const fadeStorage = ImGui::GetStateStorage();
    ImGuiID const       fadeKey     = ImGui::GetID("##info-fade");
    if (infoHovered) {
        if (fadeStorage->GetFloat(fadeKey, -1.0f) < 0.0f) fadeStorage->SetFloat(fadeKey, static_cast<float>(now));
    } else {
        fadeStorage->SetFloat(fadeKey, -1.0f);
    }
    float tooltipAlpha = 1.0f;
    if (infoHovered) {
        double const elapsed = now - fadeStorage->GetFloat(fadeKey, -1.0f);
        tooltipAlpha         = elapsed <= 0.5 ? 0.0f : static_cast<float>(std::clamp((elapsed - 0.5) / 0.3, 0.0, 1.0));
    }

    ImVec2 const infoMin = ImGui::GetItemRectMin();
    ImVec2 const infoMax{infoMin.x + infoSize, infoMin.y + infoSize};
    if (infoHovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(infoMin, infoMax, kColorButtonHover, 4.0f);
    }
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {centeredIconX(ICON_INFO, infoMin.x, infoMax.x), centeredIconY(ICON_INFO, infoMin.y, infoMax.y)},
        infoHovered ? kColorText : kColorTextDim,
        ICON_INFO
    );
    if (infoHovered) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tooltipAlpha);
        ImGui::BeginTooltip();
        ImGui::SetWindowFontScale(kFontScaleCardMeta);
        auto detailRow = [](char const* label, std::string const& value) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(value.c_str());
        };
        detailRow("playback.replayBrowser.field.name"_tr().c_str(), replay.displayName());
        detailRow(
            "playback.replayBrowser.field.world"_tr().c_str(),
            replay.worldName.empty() ? "playback.replayBrowser.unknown"_tr() : replay.worldName
        );
        detailRow("playback.replayBrowser.field.duration"_tr().c_str(), formatDuration(replay));
        detailRow("playback.replayBrowser.field.size"_tr().c_str(), formatSize(replay.fileSize));
        detailRow("playback.replayBrowser.field.modified"_tr().c_str(), formatModifiedTime(replay.lastModified));
        detailRow("playback.replayBrowser.field.fileName"_tr().c_str(), replay.replayId);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", "playback.replayBrowser.pathValue"_tr(replay.path.string()).c_str());
        if (!replay.canOpen) {
            ImGui::Spacing();
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(kColorDanger),
                "%s",
                "playback.replayBrowser.problemValue"_tr(replay.problem).c_str()
            );
        }
        ImGui::EndTooltip();
        ImGui::PopStyleVar();
    }

    std::string const displayName = replay.displayName();
    std::string const worldName =
        replay.worldName.empty() ? "playback.replayBrowser.unknownWorld"_tr() : replay.worldName;
    std::string const modified     = formatModifiedTime(replay.lastModified);
    std::string const duration     = formatDuration(replay);
    std::string const size         = formatSize(replay.fileSize);
    float const       contentX     = cardMinimum.x + kCardLayout.horizontalPadding;
    float const       contentRight = cardMaximum.x - kCardLayout.horizontalPadding;
    float const       warningSize  = replay.canOpen ? 0.0f : 26.0f;
    float const       titleRight   = contentRight - infoSize - warningSize - 8.0f;

    ImGui::SetWindowFontScale(kFontScaleCardTitle);
    drawClippedText(displayName, {contentX, cardMinimum.y + titleY}, titleRight, kColorText);

    ImGui::SetWindowFontScale(kFontScaleCardMeta);
    drawIconTextLine(ICON_WORLD, worldName, {contentX, cardMinimum.y + worldY}, contentRight, kColorTextDim);
    drawIconTextLine(ICON_CALENDAR, modified, {contentX, cardMinimum.y + modifiedY}, contentRight, kColorTextDim);

    float const sizeWidth = iconLabelWidth(ICON_FILE, size);
    float const sizeX     = std::max(contentX, contentRight - sizeWidth);
    drawIconTextLine(
        ICON_CLOCK,
        duration,
        {contentX, cardMinimum.y + footerY},
        std::max(contentX, sizeX - 12.0f),
        kColorTextDim
    );
    drawIconTextLine(ICON_FILE, size, {sizeX, cardMinimum.y + footerY}, contentRight, kColorTextDim);

    if (!replay.canOpen) {
        float const warningX = infoX - warningSize - 2.0f;
        ImGui::SetCursorPos({warningX, infoY + 1.0f});
        ImGui::InvisibleButton("##card-warning", {warningSize, warningSize});
        ImVec2 const warningMin = ImGui::GetItemRectMin();
        ImVec2 const warningMax{warningMin.x + warningSize, warningMin.y + warningSize};
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {
                centeredIconX(ICON_WARNING, warningMin.x, warningMax.x),
                centeredIconY(ICON_WARNING, warningMin.y, warningMax.y),
            },
            kColorDanger,
            ICON_WARNING
        );
        tooltip(replay.problem.c_str());
    }

    ImGui::GetWindowDrawList()->AddRect(
        {cardMinimum.x + 1.0f, cardMinimum.y + 1.0f},
        {cardMaximum.x - 1.0f, cardMaximum.y - 1.0f},
        lerpColor(kColorCardBorder, kColorCardHover, std::max(selectedAmount, hoverAmount)),
        8.0f,
        0,
        1.0f
    );

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void SelectReplayScreen::drawGrid() {
    auto clearSelectionOnBlankClick = [&] {
        if (mSelectedIds.empty()) return;
        if (!ImGui::IsWindowHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsAnyItemHovered()) {
            return;
        }
        mSelectedIds.clear();
        mSelectionAnchor.reset();
    };

    float const availableWidth = ImGui::GetContentRegionAvail().x;
    float const contentOriginX = ImGui::GetCursorPosX();
    float const gridWidth      = std::max(0.0f, availableWidth - kContentLayout.horizontalMargin * 2.0f);
    float const gridX = contentOriginX + std::max(kContentLayout.horizontalMargin, (availableWidth - gridWidth) * 0.5f);
    float const locationY   = ImGui::GetCursorPosY() + kContentLayout.locationVerticalGap;
    std::string const path  = playback::utils::PathUtils::getReplaysDir().generic_string();
    std::string const count = "playback.replayBrowser.visibleCount"_tr(mVisible.size());

    ImGui::SetCursorPos({gridX, locationY});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanelBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
    ImGui::BeginChild(
        "##grid-location",
        {gridWidth, kContentLayout.locationHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );
    ImGui::SetWindowFontScale(kFontScaleCardMeta);
    ImVec2 const locationMinimum = ImGui::GetWindowPos();
    float const  textY           = locationMinimum.y + (kContentLayout.locationHeight - ImGui::GetFontSize()) * 0.5f;
    ImVec2 const countSize       = ImGui::CalcTextSize(count.c_str());
    float const  countRight      = locationMinimum.x + gridWidth - 12.0f;
    float const  countX          = std::max(locationMinimum.x + 12.0f, countRight - countSize.x);
    drawIconTextLine(ICON_OPEN, path, {locationMinimum.x + 12.0f, textY}, countX - 18.0f, kColorTextDim);
    drawClippedText(count, {countX, textY + kTextOpticalOffsetY}, countRight, kColorTextDim);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(kFontScaleBody);

    float const gridStartY = locationY + kContentLayout.locationHeight + kContentLayout.locationVerticalGap;

    if (mVisible.empty()) {
        ImGui::SetWindowFontScale(kFontScaleLarge);
        std::string const empty     = "playback.replayBrowser.empty"_tr();
        ImVec2 const      emptySize = ImGui::CalcTextSize(empty.c_str());
        ImGui::SetCursorPos({gridX + std::max(0.0f, (gridWidth - emptySize.x) * 0.5f), gridStartY + 56.0f});
        ImGui::TextDisabled("%s", empty.c_str());
        ImGui::SetWindowFontScale(kFontScaleBody);
        std::string const importFirstLabel = "playback.replayBrowser.importFirst"_tr();
        float const       importWidth      = toolbarButtonWidth(ICON_EXPORT, importFirstLabel);
        ImGui::SetCursorPos({gridX + std::max(0.0f, (gridWidth - importWidth) * 0.5f), gridStartY + 104.0f});
        if (toolbarButton("##import-first", ICON_EXPORT, importFirstLabel, importWidth)) importReplay();
        clearSelectionOnBlankClick();
        return;
    }

    constexpr float minimumCardWidth = 330.0f;
    int const       capacity = std::max(1, static_cast<int>((gridWidth + kCardGap) / (minimumCardWidth + kCardGap)));
    int const       defaultColumns = std::min(4, capacity);
    int const       columns        = std::min(capacity, std::max(defaultColumns, static_cast<int>(mVisible.size())));
    float const     width          = (gridWidth - (columns - 1) * kCardGap) / columns;
    float const     height         = cardHeight(width);
    for (int item = 0; item < static_cast<int>(mVisible.size()); ++item) {
        int const column = item % columns;
        int const row    = item / columns;
        ImGui::SetCursorPos({gridX + column * (width + kCardGap), gridStartY + row * (height + kCardGap)});
        drawCard(replays()[mVisible[static_cast<size_t>(item)]], static_cast<std::size_t>(item), width);
    }

    int const   rows       = (static_cast<int>(mVisible.size()) + columns - 1) / columns;
    float const gridHeight = rows * height + std::max(0, rows - 1) * kCardGap;
    ImGui::SetCursorPos({gridX, gridStartY + gridHeight + kContentLayout.bottomMargin});
    ImGui::Dummy({gridWidth, 1.0f});

    // Only clicks on the true blank canvas reach here; cards, the scrollbar, and header controls are excluded.
    clearSelectionOnBlankClick();
}

void SelectReplayScreen::drawDetailsListItem(
    playback::editor::ReplayBrowserEntry const& replay,
    std::size_t                                 visibleIndex,
    float                                       width
) {
    bool const  selected       = mSelectedIds.contains(replay.replayId);
    float const itemHeight     = kDetailsLayout.listItemHeight;
    float const thumbnailWidth = std::clamp(
        width * kDetailsLayout.thumbnailWidthRatio,
        kDetailsLayout.thumbnailMinWidth,
        kDetailsLayout.thumbnailMaxWidth
    );
    float const thumbnailHeight = thumbnailWidth * 9.0f / 16.0f;
    float const thumbnailY      = (itemHeight - thumbnailHeight) * 0.5f;

    ImGui::PushID(replay.replayId.c_str());
    (void)animate(std::string("details-selected-") + replay.replayId, selected ? 1.0f : 0.0f);
    ImGui::BeginChild(
        "##details-list-item",
        {width, itemHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImVec2 const itemMinimum = ImGui::GetWindowPos();
    ImVec2 const itemMaximum{itemMinimum.x + width, itemMinimum.y + itemHeight};
    ImGui::SetCursorPos({kDetailsLayout.listItemHorizontalPadding, thumbnailY});
    ImVec2 const thumbnailMinimum = ImGui::GetCursorScreenPos();
    drawPreview(replay, {thumbnailWidth, thumbnailHeight}, 3.0f);
    ImVec2 const thumbnailMaximum{thumbnailMinimum.x + thumbnailWidth, thumbnailMinimum.y + thumbnailHeight};
    ImGui::GetWindowDrawList()->AddRect(thumbnailMinimum, thumbnailMaximum, kColorCardBorder, 3.0f);

    ImGui::SetCursorPos({0.0f, 0.0f});
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##select-details-item", {width, itemHeight});
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && ImGui::IsItemHovered()) {
        select(replay.replayId, visibleIndex, false, false);
        openSelected();
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        select(replay.replayId, visibleIndex, false, false);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        select(replay.replayId, visibleIndex, false, false);
    }
    bool const itemHovered = ImGui::IsItemHovered();
    (void)animate(std::string("details-hover-") + replay.replayId, itemHovered ? 1.0f : 0.0f);

    // Keep secondary file operations available without crowding the primary action bar.
    if (ImGui::BeginPopupContextItem("##details-item-menu")) {
        ImGui::SetWindowFontScale(kFontScaleNavControl);
        ImGui::BeginDisabled(!replay.canOpen);
        if (ImGui::MenuItem("playback.replayBrowser.action.open"_tr().c_str())) openSelected();
        if (ImGui::MenuItem("playback.replayBrowser.action.edit"_tr().c_str())) openRenameDialog();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("playback.replayBrowser.action.copyPath"_tr().c_str())) {
            ImGui::SetClipboardText(replay.path.string().c_str());
        }
        if (ImGui::MenuItem("playback.replayBrowser.action.showInFolder"_tr().c_str())) {
            playback::editor::EditorAction action{playback::editor::EditorActionType::ShowReplayInFolder};
            action.replayId = replay.replayId;
            submit(std::move(action));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("playback.replayBrowser.action.delete"_tr().c_str())) mShowDeleteDialog = true;
        ImGui::EndPopup();
    }

    std::string const displayName = replay.displayName();
    std::string const worldName =
        replay.worldName.empty() ? "playback.replayBrowser.unknownWorld"_tr() : replay.worldName;
    std::string const modified = formatModifiedTime(replay.lastModified);
    std::string const duration = formatDuration(replay);
    float const       textX    = thumbnailMaximum.x + kDetailsLayout.thumbnailToTextGap;
    float const textRight = itemMaximum.x - kDetailsLayout.listItemHorizontalPadding - (replay.canOpen ? 0.0f : 28.0f);

    ImGui::SetWindowFontScale(kFontScaleListTitle);
    float const titleY      = itemMinimum.y + kDetailsLayout.listTextVerticalPadding;
    float const titleHeight = ImGui::CalcTextSize(displayName.c_str()).y;
    drawClippedText(displayName, {textX, titleY}, textRight, kColorText);

    ImGui::SetWindowFontScale(kFontScaleListMeta);
    float const metadataHeight = ImGui::CalcTextSize(worldName.c_str()).y;
    float const footerY        = itemMaximum.y - kDetailsLayout.listTextVerticalPadding - metadataHeight;
    float const worldY =
        ((titleY + titleHeight * 0.5f) + (footerY + metadataHeight * 0.5f)) * 0.5f - metadataHeight * 0.5f;
    drawClippedText(worldName, {textX, worldY + kTextOpticalOffsetY}, textRight, kColorTextDim);

    float const durationWidth = iconLabelWidth(ICON_CLOCK, duration);
    float const durationX = std::max(textX, itemMaximum.x - kDetailsLayout.listItemHorizontalPadding - durationWidth);
    drawIconTextLine(
        ICON_CALENDAR,
        modified,
        {textX, footerY},
        durationX - kDetailsLayout.footerGroupGap,
        kColorTextDim
    );
    drawIconTextLine(
        ICON_CLOCK,
        duration,
        {durationX, footerY},
        itemMaximum.x - kDetailsLayout.listItemHorizontalPadding,
        kColorTextDim
    );

    if (!replay.canOpen) {
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {itemMaximum.x - kDetailsLayout.listItemHorizontalPadding - 20.0f, titleY},
            kColorDanger,
            ICON_WARNING
        );
    }

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::EndChild();
    ImGui::PopID();
}

void SelectReplayScreen::drawDetails() {
    ImVec2 const available      = ImGui::GetContentRegionAvail();
    float const  contentOriginX = ImGui::GetCursorPosX();
    float const  contentOriginY = ImGui::GetCursorPosY();
    float const  contentWidth   = std::max(0.0f, available.x - kContentLayout.horizontalMargin * 2.0f);
    float const  contentX =
        contentOriginX + std::max(kContentLayout.horizontalMargin, (available.x - contentWidth) * 0.5f);

    float const locationY = contentOriginY + kContentLayout.locationVerticalGap;
    ImGui::SetCursorPos({contentX, locationY});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanelBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kDetailsLayout.panelRounding);
    ImGui::BeginChild(
        "##details-location",
        {contentWidth, kContentLayout.locationHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );
    ImGui::SetWindowFontScale(kFontScaleCardMeta);
    ImVec2 const      locationMinimum = ImGui::GetWindowPos();
    float const       locationTextY = locationMinimum.y + (kContentLayout.locationHeight - ImGui::GetFontSize()) * 0.5f;
    std::string const replayPath    = playback::utils::PathUtils::getReplaysDir().generic_string();
    drawIconTextLine(
        ICON_OPEN,
        replayPath,
        {locationMinimum.x + 12.0f, locationTextY},
        locationMinimum.x + contentWidth - 12.0f,
        kColorTextDim
    );
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(kFontScaleBody);

    float const panelsY      = locationY + kContentLayout.locationHeight + kContentLayout.locationVerticalGap;
    float const panelsHeight = std::max(
        0.0f,
        available.y - kContentLayout.locationHeight - kContentLayout.locationVerticalGap * 2.0f
            - kContentLayout.bottomMargin
    );
    bool const stacked = contentWidth < kDetailsLayout.stackedBreakpoint;

    float listWidth   = contentWidth;
    float listHeight  = panelsHeight;
    float panelWidth  = contentWidth;
    float panelHeight = panelsHeight;
    float panelX      = contentX;
    float panelY      = panelsY;
    if (stacked) {
        float const preferredListHeight = std::clamp(
            panelsHeight * kDetailsLayout.stackedListHeightRatio,
            kDetailsLayout.stackedListMinHeight,
            kDetailsLayout.stackedListMaxHeight
        );
        float const maxListHeight = std::max(160.0f, panelsHeight - kDetailsLayout.panelGap - 320.0f);
        listHeight                = std::min(preferredListHeight, maxListHeight);
        panelHeight               = std::max(0.0f, panelsHeight - listHeight - kDetailsLayout.panelGap);
        panelY                    = panelsY + listHeight + kDetailsLayout.panelGap;
    } else {
        listWidth = std::clamp(
            contentWidth * kDetailsLayout.listWidthRatio,
            kDetailsLayout.listMinWidth,
            kDetailsLayout.listMaxWidth
        );
        panelX     = contentX + listWidth + kDetailsLayout.panelGap;
        panelWidth = std::max(0.0f, contentWidth - listWidth - kDetailsLayout.panelGap);
    }

    ImGui::SetCursorPos({contentX, panelsY});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorCardBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kDetailsLayout.panelRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(
        "##details-list",
        {listWidth, listHeight},
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::BeginChild("##details-list-scroll", {0.0f, -kDetailsLayout.listFooterHeight}, false);
    if (mVisible.empty()) {
        ImGui::SetWindowFontScale(kFontScaleSmall);
        ImGui::TextDisabled("%s", "playback.replayBrowser.empty"_tr().c_str());
    } else {
        for (std::size_t visibleIndex = 0; visibleIndex < mVisible.size(); ++visibleIndex) {
            float const itemWidth = ImGui::GetContentRegionAvail().x;
            drawDetailsListItem(replays()[mVisible[visibleIndex]], visibleIndex, itemWidth);
            if (visibleIndex + 1 < mVisible.size()) ImGui::Dummy({0.0f, kDetailsLayout.listItemGap});
        }
    }
    if (!mSelectedIds.empty() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsAnyItemHovered()) {
        mSelectedIds.clear();
        mSelectionAnchor.reset();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::SetWindowFontScale(kFontScaleListMeta);
    float const footerY = ImGui::GetCursorPosY()
                        + std::max(0.0f, (ImGui::GetContentRegionAvail().y - ImGui::GetFontSize()) * 0.5f)
                        + kTextOpticalOffsetY;
    ImGui::SetCursorPos({kDetailsLayout.listFooterPaddingX, footerY});
    ImGui::TextDisabled("%s", "playback.replayBrowser.fileCount"_tr(mVisible.size()).c_str());
    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::SetCursorPos({panelX, panelY});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorCardBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kDetailsLayout.panelRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(
        "##details-panel",
        {panelWidth, panelHeight},
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    auto replay = selectedReplay();
    if (!replay) {
        float const panelW = ImGui::GetWindowWidth();
        float const panelH = ImGui::GetWindowHeight();
        ImGui::SetWindowFontScale(kFontScaleLarge);
        ImVec2 const iconSize = ImGui::CalcTextSize(ICON_FILE_VIDEO);
        float const  emptyY   = std::max(24.0f, panelH * 0.40f - iconSize.y);
        ImGui::SetCursorPos({std::max(0.0f, (panelW - iconSize.x) * 0.5f), emptyY});
        ImGui::TextDisabled("%s", ICON_FILE_VIDEO);

        ImGui::SetWindowFontScale(kFontScaleSmall);
        std::string const selectHint = "playback.replayBrowser.selectForDetails"_tr();
        ImVec2 const      hintSize   = ImGui::CalcTextSize(selectHint.c_str());
        ImGui::SetCursorPos({std::max(0.0f, (panelW - hintSize.x) * 0.5f), emptyY + iconSize.y + 14.0f});
        ImGui::TextDisabled("%s", selectHint.c_str());
    } else {
        auto const& selectedReplayEntry = **replay;
        float const panelW              = ImGui::GetWindowWidth();
        float const panelH              = ImGui::GetWindowHeight();
        float const dividerY            = panelH - kDetailsLayout.actionAreaHeight();
        float const scrollWidth         = std::max(0.0f, panelW - kDetailsLayout.panelPadding * 2.0f);
        float const scrollHeight        = std::max(0.0f, dividerY - kDetailsLayout.panelPadding);

        ImGui::SetCursorPos({kDetailsLayout.panelPadding, kDetailsLayout.panelPadding});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::BeginChild("##details-scroll", {scrollWidth, scrollHeight}, false);
        ImGui::PopStyleVar();

        float const detailWidth = ImGui::GetContentRegionAvail().x;
        float const estimatedMetadataHeight =
            kDetailsLayout.metadataRowCount
                * (kDetailsLayout.detailMetadataFontSize + kDetailsLayout.metadataCellPaddingY * 2.0f)
            + kDetailsLayout.metadataWrapReserve;
        float const contentLimitedPreviewHeight = ImGui::GetWindowHeight() - kDetailsLayout.previewToMetadataGap
                                                - estimatedMetadataHeight - kDetailsLayout.detailContentBottomGap;
        float const ratioLimitedPreviewHeight = ImGui::GetWindowHeight() * kDetailsLayout.detailPreviewMaxHeightRatio;
        float const maxPreviewHeight          = std::max(
            kDetailsLayout.detailPreviewMinHeight,
            std::min(contentLimitedPreviewHeight, ratioLimitedPreviewHeight)
        );
        float const previewWidth  = std::min(detailWidth, maxPreviewHeight * kDetailsLayout.detailPreviewAspectRatio);
        float const previewHeight = previewWidth / kDetailsLayout.detailPreviewAspectRatio;
        float const previewX      = std::max(0.0f, (detailWidth - previewWidth) * 0.5f);
        ImGui::SetCursorPosX(previewX);
        ImVec2 const previewMinimum = ImGui::GetCursorScreenPos();
        drawPreview(selectedReplayEntry, {previewWidth, previewHeight}, 3.0f);
        ImVec2 const previewMaximum{previewMinimum.x + previewWidth, previewMinimum.y + previewHeight};
        ImGui::GetWindowDrawList()->AddRect(previewMinimum, previewMaximum, kColorCardBorder, 3.0f);
        ImGui::SetCursorPosX(0.0f);
        ImGui::Dummy({0.0f, kDetailsLayout.previewToMetadataGap});

        ImGui::SetWindowFontScale(kFontScaleSmall);
        ImGui::PushStyleVar(
            ImGuiStyleVar_CellPadding,
            {kDetailsLayout.metadataCellPaddingX, kDetailsLayout.metadataCellPaddingY}
        );
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColorCardBorder);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColorCardBorder);
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, kColorCardBg);
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, kColorPanelBg);
        if (ImGui::BeginTable(
                "##metadata",
                2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
            )) {
            float const labelWidth =
                std::clamp(kDetailsLayout.metadataLabelWidth, 100.0f, std::max(100.0f, detailWidth * 0.32f));
            ImGui::TableSetupColumn("##metadata-key", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##metadata-value", ImGuiTableColumnFlags_WidthStretch);
            auto row = [](char const* label, std::string const& value, bool wrap = false) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", label);
                ImGui::TableSetColumnIndex(1);
                if (wrap) ImGui::TextWrapped("%s", value.c_str());
                else ImGui::TextUnformatted(value.c_str());
            };

            std::string fileFormat = selectedReplayEntry.path.extension().string();
            if (fileFormat.empty()) fileFormat = "playback.replayBrowser.unknown"_tr();
            row("playback.replayBrowser.field.name"_tr().c_str(), selectedReplayEntry.displayName());
            row("playback.replayBrowser.field.world"_tr().c_str(),
                selectedReplayEntry.worldName.empty() ? "playback.replayBrowser.unknown"_tr()
                                                      : selectedReplayEntry.worldName);
            row("playback.replayBrowser.field.duration"_tr().c_str(), formatDuration(selectedReplayEntry));
            row("playback.replayBrowser.field.modified"_tr().c_str(),
                formatModifiedTime(selectedReplayEntry.lastModified));
            row("playback.replayBrowser.field.fileSize"_tr().c_str(), formatSize(selectedReplayEntry.fileSize));
            row("playback.replayBrowser.field.fileFormat"_tr().c_str(), fileFormat);
            row("playback.replayBrowser.field.filePath"_tr().c_str(), selectedReplayEntry.path.string(), true);
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        if (!selectedReplayEntry.canOpen) {
            ImGui::Dummy({0.0f, 8.0f});
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(kColorDanger),
                "%s",
                "playback.replayBrowser.problemValue"_tr(selectedReplayEntry.problem).c_str()
            );
        }
        ImGui::SetWindowFontScale(kFontScaleBody);
        ImGui::EndChild();

        ImVec2 const panelMinimum = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(
            {panelMinimum.x + kDetailsLayout.panelPadding, panelMinimum.y + dividerY},
            {panelMinimum.x + panelW - kDetailsLayout.panelPadding, panelMinimum.y + dividerY},
            kColorCardBorder
        );

        float const buttonWidth = std::max(
            0.0f,
            (panelW - kDetailsLayout.panelPadding * 2.0f - kDetailsLayout.actionButtonGap * 2.0f) / 3.0f
        );
        float const buttonY = dividerY + kDetailsLayout.panelPadding;
        ImGui::SetWindowFontScale(kFontScaleSmall);
        ImGui::SetCursorPos({kDetailsLayout.panelPadding, buttonY});
        ImGui::BeginDisabled(!selectedReplayEntry.canOpen);
        styleButton(true);
        if (ImGui::Button("playback.replayBrowser.action.open"_tr().c_str(), {buttonWidth, kControlHeight})) {
            openSelected();
        }
        popButtonStyle();
        ImGui::SameLine(0.0f, kDetailsLayout.actionButtonGap);
        styleButton();
        if (ImGui::Button("playback.replayBrowser.action.edit"_tr().c_str(), {buttonWidth, kControlHeight})) {
            openRenameDialog();
        }
        popButtonStyle();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, kDetailsLayout.actionButtonGap);
        ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorDanger);
        if (ImGui::Button("playback.replayBrowser.action.delete"_tr().c_str(), {buttonWidth, kControlHeight})) {
            mShowDeleteDialog = true;
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void SelectReplayScreen::drawActionBar() {
    auto replay = selectedReplay();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanelBg);
    ImGui::BeginChild(
        "##actions",
        {0.0f, kActionBarHeight},
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    float const  contentW   = ImGui::GetWindowWidth();
    ImVec2 const barMinimum = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(barMinimum, {barMinimum.x + contentW, barMinimum.y}, kColorCardBorder);

    std::uintmax_t selectedBytes{};
    for (auto const& item : replays()) {
        if (mSelectedIds.contains(item.replayId)) selectedBytes += item.fileSize;
    }
    std::string const summary =
        "playback.replayBrowser.selectedCount"_tr(mSelectedIds.size()) + "  ·  " + formatSize(selectedBytes);

    float const buttonWidth    = 140.0f;
    float const buttonGap      = 10.0f;
    float const groupWidth     = buttonWidth * 3.0f + buttonGap * 2.0f;
    float const buttonY        = (kActionBarHeight - kControlHeight) * 0.5f;
    float const availableWidth = ImGui::GetContentRegionAvail().x;
    float const contentOriginX = ImGui::GetCursorPosX();
    float const innerWidth     = std::max(0.0f, availableWidth - kScreenMargin * 2.0f);
    float const innerX         = contentOriginX + std::max(kScreenMargin, (availableWidth - innerWidth) * 0.5f);
    float const buttonX        = innerX + innerWidth - groupWidth;

    ImGui::SetWindowFontScale(kFontScaleSmall);
    drawClippedText(
        summary,
        {barMinimum.x + innerX, barMinimum.y + (kActionBarHeight - ImGui::GetFontSize()) * 0.5f},
        barMinimum.x + buttonX - 24.0f,
        kColorText
    );
    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::SetCursorPos({buttonX, buttonY});

    ImGui::BeginDisabled(!replay || !(*replay)->canOpen);
    ImGui::PushStyleColor(ImGuiCol_Button, kColorAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorAccent);
    if (ImGui::Button("playback.replayBrowser.action.open"_tr().c_str(), {buttonWidth, kControlHeight})) {
        openSelected();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0.0f, buttonGap);
    styleButton();
    if (ImGui::Button("playback.replayBrowser.action.edit"_tr().c_str(), {buttonWidth, kControlHeight})) {
        openRenameDialog();
    }
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, buttonGap);
    ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorDanger);
    if (ImGui::Button("playback.replayBrowser.action.delete"_tr().c_str(), {buttonWidth, kControlHeight})) {
        mShowDeleteDialog = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SelectReplayScreen::drawDeleteDialog() {
    std::string const deleteTitle = "playback.replayBrowser.dialog.delete.title"_tr() + "###delete-replay";
    if (mShowDeleteDialog) ImGui::OpenPopup(deleteTitle.c_str());
    if (ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("playback.replayBrowser.dialog.delete.confirm"_tr().c_str());
        ImGui::TextDisabled("%s", "playback.replayBrowser.dialog.delete.irreversible"_tr().c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, kColorDanger);
        std::string const confirmDelete =
            std::string(ICON_DELETE) + "  " + "playback.replayBrowser.dialog.delete.confirmButton"_tr();
        if (ImGui::Button(confirmDelete.c_str(), {156.0f, kControlHeight})) {
            playback::editor::EditorAction action{playback::editor::EditorActionType::DeleteReplays};
            action.replayIds.assign(mSelectedIds.begin(), mSelectedIds.end());
            submit(std::move(action));
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string const cancel = std::string(ICON_CLOSE) + "  " + "playback.replayBrowser.dialog.cancel"_tr();
        if (ImGui::Button(cancel.c_str(), {120.0f, kControlHeight})) {
            mShowDeleteDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (mState && !mState->error.empty()) {
        std::string const errorTitle =
            "playback.replayBrowser.dialog.operationFailed"_tr() + "###replay-operation-failed";
        ImGui::OpenPopup(errorTitle.c_str());
        if (ImGui::BeginPopupModal(errorTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", mState->error.c_str());
            std::string const ok = std::string(ICON_CHECK) + "  " + "playback.replayBrowser.dialog.ok"_tr();
            if (ImGui::Button(ok.c_str(), {120.0f, kControlHeight})) {
                submit({playback::editor::EditorActionType::ClearReplayBrowserError});
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void SelectReplayScreen::openRenameDialog() {
    auto replay = selectedReplay();
    if (!replay || !(*replay)->canOpen) return;
    mRenameBuffer     = (*replay)->displayName();
    mRenameDialogOpen = true;
}

void SelectReplayScreen::drawRenameDialog() {
    auto              replay      = selectedReplay();
    std::string const renameTitle = "playback.replayBrowser.dialog.rename.title"_tr() + "###rename-replay";
    if (mRenameDialogOpen) {
        ImGui::OpenPopup(renameTitle.c_str());
        mRenameDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal(renameTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::SetWindowFontScale(kFontScaleBody);
    ImGui::TextDisabled("%s", "playback.replayBrowser.dialog.rename.description"_tr().c_str());
    ImGui::Spacing();

    std::array<char, 256> buffer{};
    std::copy_n(mRenameBuffer.data(), std::min(mRenameBuffer.size(), buffer.size() - 1), buffer.data());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kColorButton);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kColorButtonHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kColorButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, 10.0f});
    ImGui::SetNextItemWidth(460.0f);
    bool const edited =
        ImGui::InputText("##rename-input", buffer.data(), buffer.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (edited) mRenameBuffer = buffer.data();

    ImGui::Spacing();
    bool const empty = mRenameBuffer.empty();
    ImGui::BeginDisabled(empty);
    styleButton();
    std::string const save  = std::string(ICON_CHECK) + "  " + "playback.replayBrowser.dialog.rename.save"_tr();
    bool const        saved = ImGui::Button(save.c_str(), {140.0f, kControlHeight});
    popButtonStyle();
    ImGui::EndDisabled();
    ImGui::SameLine();
    styleButton();
    std::string const cancel    = std::string(ICON_CLOSE) + "  " + "playback.replayBrowser.dialog.cancel"_tr();
    bool const        cancelled = ImGui::Button(cancel.c_str(), {120.0f, kControlHeight});
    popButtonStyle();

    if (saved && replay) {
        playback::editor::EditorAction action{playback::editor::EditorActionType::RenameReplay};
        action.replayId = (*replay)->replayId;
        action.name     = mRenameBuffer;
        submit(std::move(action));
        ImGui::CloseCurrentPopup();
    } else if (cancelled) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace playback::screen::select_replay

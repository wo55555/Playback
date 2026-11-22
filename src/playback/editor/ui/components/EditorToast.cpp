#include "EditorToast.h"

#include "playback/editor/ui/EditorTheme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback::editor::ui {

namespace {

constexpr float kFadeInSeconds  = 0.12f;
constexpr float kFadeOutSeconds = 0.35f;

[[nodiscard]] ImU32 scaleAlpha(ImU32 color, float alphaScale) {
    int const baseAlpha = static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xffu);
    int const scaled =
        static_cast<int>(std::lround(static_cast<float>(baseAlpha) * std::clamp(alphaScale, 0.0f, 1.0f)));
    return theme::withAlpha(color, std::clamp(scaled, 0, 255));
}

} // namespace

void EditorToast::show(std::string message, float durationSeconds) {
    if (message.empty() || durationSeconds <= 0.0f) {
        clear();
        return;
    }
    mMessage  = std::move(message);
    mElapsed  = 0.0f;
    mDuration = durationSeconds;
    mVisible  = true;
}

void EditorToast::clear() {
    mMessage.clear();
    mElapsed  = 0.0f;
    mDuration = 0.0f;
    mVisible  = false;
}

void EditorToast::draw(ImVec2 viewportPos, ImVec2 viewportSize, float dt) {
    if (!mVisible || mMessage.empty() || mDuration <= 0.0f) return;

    float const stepDt  = dt < 0.0f ? std::max(0.0f, ImGui::GetIO().DeltaTime) : std::max(0.0f, dt);
    mElapsed           += stepDt;
    if (mElapsed >= mDuration) {
        clear();
        return;
    }

    float const fadeInAlpha  = mElapsed < kFadeInSeconds ? (mElapsed / kFadeInSeconds) : 1.0f;
    float const remaining    = mDuration - mElapsed;
    float const fadeOutAlpha = remaining < kFadeOutSeconds ? (remaining / kFadeOutSeconds) : 1.0f;
    float const alpha        = std::clamp(std::min(fadeInAlpha, fadeOutAlpha), 0.0f, 1.0f);
    if (alpha <= 0.001f) return;

    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) {
        if (ImGuiViewport const* vp = ImGui::GetMainViewport()) {
            viewportPos  = vp->Pos;
            viewportSize = vp->Size;
        }
    }
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) return;

    float const  scale        = metrics::scale();
    float const  fontSize     = metrics::font();
    float const  padX         = 14.0f * scale;
    float const  padY         = 7.0f * scale;
    float const  bottomMargin = 28.0f * scale;
    float const  maxTextWidth = std::max(60.0f * scale, viewportSize.x - 32.0f * scale - padX * 2.0f);
    ImVec2 const textSize =
        ImGui::CalcTextSize(mMessage.c_str(), mMessage.c_str() + mMessage.size(), false, maxTextWidth);
    ImVec2 const pillSize{textSize.x + padX * 2.0f, textSize.y + padY * 2.0f};
    ImVec2 const pillMin{
        std::round(viewportPos.x + (viewportSize.x - pillSize.x) * 0.5f),
        std::round(viewportPos.y + viewportSize.y - bottomMargin - pillSize.y),
    };
    ImVec2 const pillMax{pillMin.x + pillSize.x, pillMin.y + pillSize.y};
    float const  rounding = pillSize.y * 0.5f;

    auto* const drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(
        {pillMin.x, pillMin.y + 2.0f * scale},
        {pillMax.x, pillMax.y + 2.0f * scale},
        scaleAlpha(theme::kTooltipShadow, alpha),
        rounding
    );
    drawList->AddRectFilled(pillMin, pillMax, scaleAlpha(theme::kOverlayBg, alpha), rounding);
    drawList->AddRect(pillMin, pillMax, scaleAlpha(theme::kTooltipBorder, alpha), rounding, 0, 1.0f);
    drawList->AddText(
        ImGui::GetFont(),
        fontSize,
        {pillMin.x + padX, pillMin.y + padY},
        scaleAlpha(theme::kText, alpha),
        mMessage.c_str(),
        mMessage.c_str() + mMessage.size(),
        maxTextWidth
    );
}

} // namespace playback::editor::ui

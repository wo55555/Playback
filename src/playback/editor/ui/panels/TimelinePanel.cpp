#include "TimelinePanel.h"

#include "playback/audio/ExternalAudioCache.h"
#include "playback/audio/ExternalAudioMixer.h"
#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/HistoryLabel.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/keyframe/FreezeTimelineEvaluator.h"
#include "playback/keyframe/SpeedTimelineEvaluator.h"
#include "playback/keyframe/TimeOfDayTimelineEvaluator.h"
#include "playback/keyframe/TimelapseTimelineEvaluator.h"
#include "playback/keyframe/WeatherTimelineEvaluator.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/TimelineNavigation.h"
#include "playback/state/editing/commands/MarkerCommands.h"
#include "playback/utils/ShellUtils.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <ranges>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

constexpr float kMinZoomScale   = 1.0f;
constexpr float kMaxZoomScale   = 20.0f;
constexpr float kZoomStep       = 1.15f;
constexpr int   kTicksPerSecond = widgets::kTicksPerSecond;
// Shift-snap reach in screen pixels, so it feels the same at every zoom level.
constexpr float kSnapPixelRadius = 10.0f;
constexpr float kMarkerHalfWidth = 5.0f;
constexpr float kBoxDragThreshold = 4.0f;
// Queued actions run on the controller's next tick; give up on a pending selection after this many frames.
constexpr int   kPendingSelectionFrames = 30;
constexpr ImU32 kFreezeAccent           = IM_COL32(96, 200, 255, 255);
constexpr ImU32 kFreezeDelayTint        = IM_COL32(150, 232, 255, 255);
constexpr ImU32 kFreezeKeyFill          = IM_COL32(82, 205, 255, 255);
constexpr ImU32 kFreezeUnfreezeFill     = IM_COL32(118, 214, 152, 255);
constexpr ImU32 kFreezeUnfreezeOutline  = IM_COL32(236, 242, 248, 255);
constexpr ImU32 kTimelapseAccent         = IM_COL32(180, 122, 255, 255);
constexpr ImU32 kTimelapseKeyFill        = IM_COL32(190, 136, 255, 255);
constexpr ImU32 kTimelapseInvalidFill    = IM_COL32(220, 68, 68, 72);
constexpr ImU32 kTimelapseInvalidEdge    = IM_COL32(235, 92, 82, 210);
constexpr ImU32 kTimelapseInvalidText    = IM_COL32(255, 118, 104, 255);
constexpr ImU32 kTimelapseInvalidKeyFill = IM_COL32(255, 95, 72, 255);
constexpr ImU32 kTimeOfDayAccent         = IM_COL32(246, 184, 76, 255);
constexpr ImU32 kTimeOfDayKeyFill        = IM_COL32(250, 198, 92, 255);
constexpr ImU32 kWeatherAccent           = IM_COL32(0x52, 0xa8, 0xe8, 0xff);
constexpr ImU32 kAudioAccent             = IM_COL32(52, 211, 153, 255);
constexpr ImU32 kAudioErrorEdge          = IM_COL32(235, 110, 82, 220);
constexpr ImU32 kAudioErrorText          = IM_COL32(255, 140, 118, 255);

using widgets::formatTick;
using widgets::formatTickCompact;
using widgets::iconButton;
using widgets::iconButtonSize;
using widgets::iconToggle;

struct PlayheadBadgeMetrics {
    float       playheadX{};
    float       badgeX{};
    float       badgeY{};
    float       badgeW{};
    float       badgeH{};
    float       fontSize{};
    ImVec2      textSize{};
    std::string text;
};

PlayheadBadgeMetrics computePlayheadBadge(
    float                canvasLeft,
    float                canvasRight,
    float                rulerTop,
    float                headerHeight,
    TimelineScale const& scale,
    int                  displayTick
) {
    float const          uiScale = metrics::scale();
    PlayheadBadgeMetrics out;
    out.playheadX = std::clamp(scale.xAt(static_cast<float>(displayTick)), canvasLeft, canvasRight);
    out.text      = formatTick(displayTick);
    out.fontSize  = std::max(10.5f, std::round(metrics::font() * 0.76f));
    out.textSize  = ImGui::GetFont()->CalcTextSizeA(out.fontSize, FLT_MAX, 0.0f, out.text.c_str());
    out.badgeW    = out.textSize.x + 9.0f * uiScale;
    out.badgeH    = std::max(14.5f, headerHeight - 6.0f * uiScale);
    out.badgeX    = std::clamp(out.playheadX - out.badgeW * 0.5f, canvasLeft, canvasRight - out.badgeW);
    out.badgeY    = rulerTop + 1.5f * uiScale;
    return out;
}

int majorTickStep(float pixelsPerTick, float minimumSpacing) {
    constexpr int steps[] = {
        kTicksPerSecond,
        kTicksPerSecond * 2,
        kTicksPerSecond * 5,
        kTicksPerSecond * 10,
        kTicksPerSecond * 20,
        kTicksPerSecond * 30,
        kTicksPerSecond * 60,
        kTicksPerSecond * 120,
        kTicksPerSecond * 300,
        kTicksPerSecond * 600,
    };
    for (int step : steps)
        if (static_cast<float>(step) * pixelsPerTick >= minimumSpacing) return step;
    return steps[std::size(steps) - 1];
}

int computeMajorTickStep(float pixelsPerTick, float labelFontSize, int totalTicks) {
    ImFont* const     font         = ImGui::GetFont();
    std::string const longest      = formatTickCompact(totalTicks);
    float const       minimumMajor = std::max(
        metrics::font() * 6.0f,
        font->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, longest.c_str()).x + 30.0f * metrics::scale()
    );
    return majorTickStep(pixelsPerTick, minimumMajor);
}

bool contains(ImVec2 const& minimum, ImVec2 const& maximum, ImVec2 const& point) {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y && point.y <= maximum.y;
}

float maxScrollFor(float canvasWidth, float zoom, int totalTicks) {
    float const fit = canvasWidth / static_cast<float>(std::max(1, totalTicks));
    return std::max(0.0f, static_cast<float>(totalTicks) * fit * zoom - canvasWidth);
}

void drawKeyframeDiamond(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    ImVec2 const top{x, centerY - radius};
    ImVec2 const right{x + radius, centerY};
    ImVec2 const bottom{x, centerY + radius};
    ImVec2 const left{x - radius, centerY};
    drawList->AddQuadFilled(top, right, bottom, left, fill);
    drawList->AddQuad(top, right, bottom, left, outline, 1.5f);
}

void drawKeyframeSquare(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    float const h = radius * 0.8f;
    drawList->AddRectFilled({x - h, centerY - h}, {x + h, centerY + h}, fill);
    drawList->AddRect({x - h, centerY - h}, {x + h, centerY + h}, outline, 0.0f, 0, 1.5f);
}

void drawKeyframeCircle(
    ImDrawList* drawList,
    float       x,
    float       centerY,
    float       radius,
    ImU32       fill,
    ImU32       outline,
    bool        thirdPerson,
    bool        thirdPersonCustom = false,
    bool        hold              = false
) {
    float const r     = radius * 0.92f;
    int const   alpha = static_cast<int>((fill >> IM_COL32_A_SHIFT) & 0xff);
    drawList->AddCircleFilled({x, centerY}, r, fill);
    drawList->AddCircle({x, centerY}, r, outline, 0, 1.5f);
    if (thirdPerson) {
        drawList->AddCircleFilled({x, centerY}, r * 0.58f, IM_COL32(0x16, 0x18, 0x20, alpha));
        if (thirdPersonCustom) {
            ImU32 const core = IM_COL32(0xf4, 0xf8, 0xff, alpha);
            if (hold) {
                float const h = std::max(1.25f, r * 0.34f);
                drawList->AddRectFilled({x - h, centerY - h}, {x + h, centerY + h}, core);
            } else {
                float const d = std::max(1.5f, r * 0.46f);
                drawList->AddQuadFilled({x, centerY - d}, {x + d, centerY}, {x, centerY + d}, {x - d, centerY}, core);
            }
        } else {
            drawList->AddCircleFilled({x, centerY}, std::max(1.1f, r * 0.24f), fill);
        }
    } else {
        drawList->AddCircleFilled({x, centerY}, std::max(1.25f, r * 0.30f), IM_COL32(0xf4, 0xf8, 0xff, alpha));
    }
}

std::string trimWhitespace(std::string value) {
    auto const first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

std::optional<std::string> findSplittableClip(
    state::editing::model::EditorStateExt const& project,
    state::editing::model::SelectionModel const& selection,
    int                                          tick
) {
    auto const isSplittable = [&](state::editing::model::CameraEntity const& camera,
                                  state::editing::model::Clip const&         clip) {
        return !camera.locked && !clip.locked && clip.inTick < tick && tick < clip.outTick;
    };

    if (auto const* selectedClip = selection.getAs<state::editing::model::SelectedClip>()) {
        if (auto const hit = state::editing::ClipOps::findClipWithCamera(project, selectedClip->clipId);
            hit && isSplittable(*hit.camera, *hit.clip)) {
            return hit.clip->id;
        }
        return std::nullopt;
    }

    std::string selectedCameraId;
    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        selectedCameraId = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = selection.getAs<state::editing::model::SelectedKeyframe>()) {
        selectedCameraId = selectedKeyframe->trackId;
    }
    if (!selectedCameraId.empty()) {
        auto const camIt =
            std::ranges::find(project.cameras, selectedCameraId, &state::editing::model::CameraEntity::id);
        if (camIt != project.cameras.end() && !camIt->locked) {
            for (auto const& clip : camIt->clips) {
                if (!clip.locked && clip.inTick < tick && tick < clip.outTick) {
                    return clip.id;
                }
            }
        }
        return std::nullopt;
    }

    for (auto const& camera : project.cameras) {
        if (!camera.enabled || camera.locked) continue;
        for (auto const& clip : camera.clips) {
            if (!clip.locked && clip.inTick < tick && tick < clip.outTick) {
                return clip.id;
            }
        }
    }
    return std::nullopt;
}

struct ClipTrimTarget {
    std::string clipId;
    int         newInTick{};
    int         newOutTick{};
};

std::pair<int, int> clipBoundsOnCamera(
    state::editing::model::CameraEntity const& camera,
    state::editing::model::Clip const&         clip,
    int                                        totalTicks
) {
    int minIn  = 0;
    int maxOut = totalTicks;
    for (auto const& other : camera.clips) {
        if (other.id == clip.id) continue;
        if (other.outTick <= clip.inTick) minIn = std::max(minIn, other.outTick);
        if (other.inTick >= clip.outTick) maxOut = std::min(maxOut, other.inTick);
    }
    minIn  = std::clamp(minIn, 0, totalTicks);
    maxOut = std::clamp(maxOut, minIn, totalTicks);
    return {minIn, maxOut};
}

std::optional<ClipTrimTarget> tryMakeTrimTarget(
    state::editing::model::CameraEntity const& camera,
    state::editing::model::Clip const&         clip,
    int                                        playhead,
    int                                        totalTicks,
    bool                                       trimIn
) {
    if (camera.locked || clip.locked) return std::nullopt;
    auto const [minIn, maxOut] = clipBoundsOnCamera(camera, clip, totalTicks);
    if (trimIn) {
        if (playhead >= minIn && playhead < clip.outTick && playhead != clip.inTick) {
            return ClipTrimTarget{clip.id, playhead, clip.outTick};
        }
    } else {
        if (playhead > clip.inTick && playhead <= maxOut && playhead != clip.outTick) {
            return ClipTrimTarget{clip.id, clip.inTick, playhead};
        }
    }
    return std::nullopt;
}

std::optional<ClipTrimTarget> findTrimmableClip(
    state::editing::model::EditorStateExt const& project,
    state::editing::model::SelectionModel const& selection,
    int                                          playhead,
    int                                          totalTicks,
    bool                                         trimIn
) {
    if (totalTicks <= 0 || playhead < 0 || playhead > totalTicks) return std::nullopt;

    if (auto const* selectedClip = selection.getAs<state::editing::model::SelectedClip>()) {
        auto const hit = state::editing::ClipOps::findClipWithCamera(project, selectedClip->clipId);
        if (!hit) return std::nullopt;
        return tryMakeTrimTarget(*hit.camera, *hit.clip, playhead, totalTicks, trimIn);
    }

    std::string selectedCameraId;
    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        selectedCameraId = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = selection.getAs<state::editing::model::SelectedKeyframe>()) {
        selectedCameraId = selectedKeyframe->trackId;
    }
    if (!selectedCameraId.empty()) {
        auto const camIt =
            std::ranges::find(project.cameras, selectedCameraId, &state::editing::model::CameraEntity::id);
        if (camIt == project.cameras.end() || camIt->locked) return std::nullopt;
        for (auto const& clip : camIt->clips) {
            if (!clip.locked && clip.inTick < playhead && playhead < clip.outTick) {
                return tryMakeTrimTarget(*camIt, clip, playhead, totalTicks, trimIn);
            }
        }
        // In a gap on the selected camera: trimIn extends the next clip left, trimOut extends the previous clip right.
        for (auto const& clip : camIt->clips) {
            if (clip.locked) continue;
            auto const [minIn, maxOut] = clipBoundsOnCamera(*camIt, clip, totalTicks);
            if (trimIn && minIn <= playhead && playhead < clip.inTick) {
                return ClipTrimTarget{clip.id, playhead, clip.outTick};
            }
            if (!trimIn && clip.outTick < playhead && playhead <= maxOut) {
                return ClipTrimTarget{clip.id, clip.inTick, playhead};
            }
        }
        return std::nullopt;
    }

    for (auto const& camera : project.cameras) {
        if (!camera.enabled || camera.locked) continue;
        for (auto const& clip : camera.clips) {
            if (!clip.locked && clip.inTick < playhead && playhead < clip.outTick) {
                return tryMakeTrimTarget(camera, clip, playhead, totalTicks, trimIn);
            }
        }
    }
    return std::nullopt;
}

int utf8CharLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

std::string elideTextToWidth(ImFont* font, float fontSize, std::string_view text, float maxWidth) {
    if (text.empty() || maxWidth <= 0.0f) return {};
    float const fullWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
    if (fullWidth <= maxWidth) return std::string{text};

    constexpr char const* kEllipsis = "...";
    float const           dotsWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, kEllipsis).x;
    if (dotsWidth > maxWidth) return {};

    size_t bestEnd = 0;
    size_t pos     = 0;
    while (pos < text.size()) {
        int const    step = utf8CharLength(static_cast<unsigned char>(text[pos]));
        size_t const next = std::min(text.size(), pos + static_cast<size_t>(step));
        float const width = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + next).x + dotsWidth;
        if (width > maxWidth) break;
        bestEnd = next;
        pos     = next;
    }
    std::string result{text.substr(0, bestEnd)};
    result += kEllipsis;
    return result;
}

std::optional<unsigned int> decodeUtf8First(char const* text) {
    if (!text || *text == '\0') return std::nullopt;
    auto const* bytes = reinterpret_cast<unsigned char const*>(text);
    if (bytes[0] < 0x80) return bytes[0];
    if ((bytes[0] & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        return static_cast<unsigned int>((bytes[0] & 0x1F) << 6 | (bytes[1] & 0x3F));
    }
    if ((bytes[0] & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
        return static_cast<unsigned int>((bytes[0] & 0x0F) << 12 | (bytes[1] & 0x3F) << 6 | (bytes[2] & 0x3F));
    }
    if ((bytes[0] & 0xF8) == 0xF0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80
        && (bytes[3] & 0xC0) == 0x80) {
        return static_cast<unsigned int>(
            (bytes[0] & 0x07) << 18 | (bytes[1] & 0x3F) << 12 | (bytes[2] & 0x3F) << 6 | (bytes[3] & 0x3F)
        );
    }
    return std::nullopt;
}

void drawCompactIconAtCentre(
    ImDrawList*   drawList,
    char const*   icon,
    ImVec2 const& centre,
    float         glyphSize,
    ImU32         color
) {
    ImFont*            font  = ImGui::GetFont();
    ImFontGlyph const* glyph = nullptr;
    if (auto const codepoint = decodeUtf8First(icon)) {
        if (ImFontBaked* baked = font->GetFontBaked(glyphSize)) {
            glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        }
    }
    if (!glyph) {
        ImVec2 const fallback = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, icon);
        drawList->AddText(
            font,
            glyphSize,
            {std::floor(centre.x - fallback.x * 0.5f + 0.5f), std::floor(centre.y - fallback.y * 0.5f + 0.5f)},
            color,
            icon
        );
        return;
    }
    ImVec2 const pos{
        std::floor(centre.x - (glyph->X0 + glyph->X1) * 0.5f + 0.5f),
        std::floor(centre.y - (glyph->Y0 + glyph->Y1) * 0.5f + 0.5f)
    };
    drawList->AddText(font, glyphSize, pos, color, icon);
}

void drawDiagonalHatch(
    ImDrawList*   drawList,
    ImVec2 const& min,
    ImVec2 const& max,
    ImU32         color,
    float         spacing,
    float         thickness = 1.0f
) {
    if (max.x <= min.x || max.y <= min.y || spacing <= 1.0f) return;
    float const height = max.y - min.y;
    drawList->PushClipRect(min, max, true);
    for (float x = min.x - height; x < max.x; x += spacing) {
        drawList->AddLine({x, max.y}, {x + height, min.y}, color, thickness);
    }
    drawList->PopClipRect();
}

// Symmetric log2 mapping centered at 1.0x: MinSpeedKeyframeValue at bottom, 1.0x at center, MaxSpeedKeyframeValue at
// top.
float speedToCurveY(double speed, float plotTop, float plotBottom) {
    double const minSpd = static_cast<double>(state::editing::model::MinSpeedKeyframeValue);
    double const maxSpd = static_cast<double>(state::editing::model::MaxSpeedKeyframeValue);
    double const safe   = std::clamp(std::isfinite(speed) ? speed : 1.0, minSpd, maxSpd);
    double       t      = 0.5;
    if (safe <= 1.0) {
        double const span = -std::log2(minSpd);
        t                 = span > 0.0 ? 0.5 * (1.0 + std::clamp(std::log2(safe) / span, -1.0, 0.0)) : 0.5;
    } else {
        double const span = std::log2(maxSpd);
        t                 = span > 0.0 ? 0.5 + 0.5 * std::clamp(std::log2(safe) / span, 0.0, 1.0) : 0.5;
    }
    return static_cast<float>(static_cast<double>(plotBottom) - t * static_cast<double>(plotBottom - plotTop));
}

float timeOfDayToCurveY(double timeOfDay, double minVal, double maxVal, float plotTop, float plotBottom) {
    double const span = std::max(1.0, maxVal - minVal);
    double const t    = std::clamp((timeOfDay - minVal) / span, 0.0, 1.0);
    return static_cast<float>(static_cast<double>(plotBottom) - t * static_cast<double>(plotBottom - plotTop));
}

std::string formatMinecraftClock(int ticks) {
    int const clamped      = std::max(0, ticks);
    int const day          = clamped / state::editing::model::DayCycleTicks;
    int const tickInDay    = clamped % state::editing::model::DayCycleTicks;
    int const totalMinutes = ((tickInDay * 1440) / state::editing::model::DayCycleTicks + 360) % 1440;
    int const hour         = totalMinutes / 60;
    int const minute       = totalMinutes % 60;
    char      buf[32]{};
    if (day > 0) {
        std::snprintf(buf, sizeof(buf), "+%dd %02d:%02d", day, hour, minute);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    }
    return std::string(buf);
}

std::string interpolationTypeLabel(state::editing::model::CameraInterpolationType type) {
    using Type = state::editing::model::CameraInterpolationType;
    switch (type) {
    case Type::Smooth:
        return "playback.refactorEditor.details.interpolationType.smooth"_tr();
    case Type::Linear:
        return "playback.refactorEditor.details.interpolationType.linear"_tr();
    case Type::EaseIn:
        return "playback.refactorEditor.details.interpolationType.easeIn"_tr();
    case Type::EaseOut:
        return "playback.refactorEditor.details.interpolationType.easeOut"_tr();
    case Type::EaseInOut:
        return "playback.refactorEditor.details.interpolationType.easeInOut"_tr();
    case Type::Hold:
        return "playback.refactorEditor.details.interpolationType.hold"_tr();
    case Type::Hermite:
        return "playback.refactorEditor.details.interpolationType.hermite"_tr();
    case Type::CubicBezier:
        return "playback.refactorEditor.details.interpolationType.cubicBezier"_tr();
    }
    return "playback.refactorEditor.details.interpolationType.smooth"_tr();
}

bool speedTrackEquals(state::editing::model::SpeedTrack const& a, state::editing::model::SpeedTrack const& b) {
    if (a.enabled != b.enabled || a.keysByTick.size() != b.keysByTick.size()) return false;
    auto itA = a.keysByTick.begin();
    auto itB = b.keysByTick.begin();
    for (; itA != a.keysByTick.end(); ++itA, ++itB) {
        if (itA->first != itB->first) return false;
        auto const& ka = itA->second;
        auto const& kb = itB->second;
        if (ka.speed != kb.speed || ka.interpolationType != kb.interpolationType || ka.bezierCtrl1.x != kb.bezierCtrl1.x
            || ka.bezierCtrl1.y != kb.bezierCtrl1.y || ka.bezierCtrl2.x != kb.bezierCtrl2.x
            || ka.bezierCtrl2.y != kb.bezierCtrl2.y) {
            return false;
        }
    }
    return true;
}

bool defaultFreezeStateAt(state::editing::model::FreezeTrack const& track, int tick) {
    auto const it = track.keysByTick.lower_bound(tick);
    if (it == track.keysByTick.begin()) return true;
    return !std::prev(it)->second.frozen;
}

void drawDashedHorizontalLine(
    ImDrawList* drawList,
    float       x0,
    float       x1,
    float       y,
    ImU32       color,
    float       dashLen,
    float       gapLen,
    float       thickness = 1.0f
) {
    if (x1 <= x0 || dashLen <= 0.0f) return;
    float const step = dashLen + std::max(0.0f, gapLen);
    for (float x = x0; x < x1; x += step) {
        float const segEnd = std::min(x1, x + dashLen);
        drawList->AddLine({x, y}, {segEnd, y}, color, thickness);
    }
}

ImU32 weatherModeColor(state::editing::model::WeatherOverride mode) {
    using Weather = state::editing::model::WeatherOverride;
    switch (mode) {
    case Weather::Recorded:
        return IM_COL32(0x7c, 0x86, 0x94, 0xff);
    case Weather::Clear:
        return IM_COL32(0xee, 0xb6, 0x44, 0xff);
    case Weather::Overcast:
        return IM_COL32(0x8a, 0x9c, 0xb0, 0xff);
    case Weather::Rain:
        return IM_COL32(0x4a, 0x98, 0xe8, 0xff);
    case Weather::Snow:
        return IM_COL32(0x98, 0xdc, 0xf4, 0xff);
    case Weather::Thunder:
        return IM_COL32(0x9c, 0x76, 0xe8, 0xff);
    }
    return kWeatherAccent;
}

char const* weatherModeIcon(state::editing::model::WeatherOverride mode) {
    using Weather = state::editing::model::WeatherOverride;
    switch (mode) {
    case Weather::Recorded:
        return ICON_WORLD;
    case Weather::Clear:
        return ICON_WEATHER_CLEAR;
    case Weather::Overcast:
        return ICON_WEATHER_OVERCAST;
    case Weather::Rain:
        return ICON_WEATHER_RAIN;
    case Weather::Snow:
        return ICON_WEATHER_SNOW;
    case Weather::Thunder:
        return ICON_WEATHER_THUNDER;
    }
    return ICON_WEATHER;
}

std::string weatherModeLabel(state::editing::model::WeatherOverride mode) {
    using Weather = state::editing::model::WeatherOverride;
    switch (mode) {
    case Weather::Recorded:
        return "playback.refactorEditor.worldEnv.weatherRecorded"_tr();
    case Weather::Clear:
        return "playback.refactorEditor.worldEnv.weatherClear"_tr();
    case Weather::Overcast:
        return "playback.refactorEditor.worldEnv.weatherOvercast"_tr();
    case Weather::Rain:
        return "playback.refactorEditor.worldEnv.weatherRain"_tr();
    case Weather::Snow:
        return "playback.refactorEditor.worldEnv.weatherSnow"_tr();
    case Weather::Thunder:
        return "playback.refactorEditor.worldEnv.weatherThunder"_tr();
    }
    return "playback.refactorEditor.worldEnv.weatherRecorded"_tr();
}

} // namespace

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.15f, 0.50f);
    mZoomScale           = std::clamp(zoomScale, kMinZoomScale, kMaxZoomScale);
    mScrollX             = std::max(0.0f, horizontalScroll);
    mScrollY             = 0.0f;
    mPendingSeekTick     = -1;
    mRulerDragTick       = -1;
    mAnimator.clear();
    mDraggingKeyframeCameraId.clear();
    mDraggingKeyframeStartTick = -1;
    mDraggingKeyframeMoved     = false;
    mDraggingSpeedKeyframeOriginTick = -1;
    mDraggingSpeedKeyframeTick       = -1;
    mDraggingSpeedKeyframeMoved      = false;
    mDraggingFreezeKeyframeOriginTick = -1;
    mDraggingFreezeKeyframeTick       = -1;
    mDraggingFreezeKeyframeMoved      = false;
    mDraggingTimelapseKeyframeOriginTick = -1;
    mDraggingTimelapseKeyframeTick       = -1;
    mDraggingTimelapseKeyframeMoved      = false;
    mDraggingTimeOfDayKeyframeOriginTick = -1;
    mDraggingTimeOfDayKeyframeTick       = -1;
    mDraggingTimeOfDayKeyframeMoved      = false;
    mTimeOfDayGroupDrag                  = false;
    mTimeOfDayGroupDragDelta             = 0;
    mDraggingWeatherKeyframeOriginTick   = -1;
    mDraggingWeatherKeyframeTick         = -1;
    mDraggingWeatherKeyframeMoved        = false;
    mWeatherGroupDrag                    = false;
    mWeatherGroupDragDelta               = 0;
    mDraggingAudioTrackId.clear();
    mDraggingAudioClipId.clear();
    mDraggingAudioOriginTick = -1;
    mDraggingAudioDraftTick  = -1;
    mDraggingAudioMoved      = false;
    mTrackDragCameraId.clear();
    mTrackDragCancelled        = false;
    mClipDragMode              = 0;
    mClipDragCameraId.clear();
    mClipDragId.clear();
    mClipDragMoved             = false;
    mRangeDragMode             = 0;
    mExportMarkerDrag          = 0;
    mExportDragInTick          = -1;
    mExportDragOutTick         = -1;
}

void TimelinePanel::submitSeek(PanelContext const& ctx, int tick) {
    mPendingSeekTick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ctx.submitAction(std::move(action));
}

void TimelinePanel::seekTo(PanelContext const& ctx, int tick) { submitSeek(ctx, tick); }

void TimelinePanel::seekRelative(PanelContext const& ctx, int tickDelta) {
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    submitSeek(ctx, baseTick + tickDelta);
}

void TimelinePanel::seekSmartEdge(PanelContext const& ctx, bool toEnd) {
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    seekTo(
        ctx,
        state::editing::TimelineNavigation::resolveSmartEdgeTick(
            baseTick,
            ctx.state.totalTicks,
            ctx.commands.exportStartTick(),
            ctx.commands.exportEndTick(),
            toEnd
        )
    );
}

void TimelinePanel::seekAdjacentEditPoint(PanelContext const& ctx, bool forward) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) return;

    int const  baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    int        target   = forward ? state.totalTicks : 0;
    bool       found    = false;
    auto const consider = [&](int tick) {
        if ((forward && tick <= baseTick) || (!forward && tick >= baseTick)) return;
        if (!found || (forward ? tick < target : tick > target)) {
            target = tick;
            found  = true;
        }
    };

    consider(0);
    consider(state.totalTicks);
    int const exportIn  = ctx.commands.exportStartTick();
    int const exportOut = ctx.commands.exportEndTick();
    if (exportIn > 0 && exportIn < state.totalTicks) consider(exportIn);
    if (exportOut > 0 && exportOut < state.totalTicks) consider(exportOut);
    for (auto const& camera : project->cameras) {
        for (auto const& [tick, _] : camera.keysByTick) consider(tick);
        for (auto const& clip : camera.clips) {
            consider(clip.inTick);
            consider(clip.outTick);
        }
    }
    for (auto const& marker : project->markers) consider(marker.tick);
    for (auto const& [tick, _] : project->speedTrack.keysByTick) consider(tick);
    for (auto const& [tick, _] : project->freezeTrack.keysByTick) consider(tick);
    for (auto const& [tick, _] : project->timelapseTrack.keysByTick) consider(tick);
    for (auto const& [tick, _] : project->timeOfDayTrack.keysByTick) consider(tick);
    for (auto const& [tick, _] : project->weatherTrack.keysByTick) consider(tick);
    for (auto const& audioTrack : project->audioTracks) {
        for (auto const& audioClip : audioTrack.clips) consider(audioClip.startTick);
    }
    submitSeek(ctx, target);
}

bool TimelinePanel::addKeyframeAtPlayhead(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (project && (ctx.selection.isSpeedTrackSelected() || ctx.selection.selectedSpeedKeyframe().has_value())) {
        if (project->speedTrack.locked) return false;
        int const tick = std::clamp(
            mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
            0,
            std::max(0, ctx.state.totalTicks)
        );
        if (project->speedTrack.keysByTick.contains(tick)) return false;
        float const sampled =
            static_cast<float>(keyframe::sampleSpeedTrackAt(project->speedTrack, static_cast<long double>(tick)));
        EditorAction action{EditorActionType::AddSpeedKeyframe};
        action.tick       = tick;
        action.speedValue = sampled;
        ctx.submitAction(std::move(action));
        queueSpeedKeyframeSelection(tick);
        ctx.commands.showSelectionInspector();
        return true;
    }
    if (project && (ctx.selection.isFreezeTrackSelected() || ctx.selection.selectedFreezeKeyframe().has_value())) {
        int const tick = std::clamp(
            mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
            0,
            std::max(0, ctx.state.totalTicks)
        );
        return addFreezeKeyframeAt(ctx, tick);
    }
    if (project
        && (ctx.selection.isTimelapseTrackSelected() || ctx.selection.selectedTimelapseKeyframe().has_value())) {
        int const tick = std::clamp(
            mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
            0,
            std::max(0, ctx.state.totalTicks)
        );
        return addTimelapseKeyframeAt(ctx, tick);
    }
    if (project && (ctx.selection.isTimeOfDayTrackSelected() || !ctx.selection.selectedTimeOfDayKeyframes().empty())) {
        int const tick = std::clamp(
            mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
            0,
            std::max(0, ctx.state.totalTicks)
        );
        return addTimeOfDayKeyframeAt(ctx, tick);
    }
    if (project && (ctx.selection.isWeatherTrackSelected() || !ctx.selection.selectedWeatherKeyframes().empty())) {
        int const tick = std::clamp(
            mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
            0,
            std::max(0, ctx.state.totalTicks)
        );
        return addWeatherKeyframeAt(ctx, tick);
    }

    std::string cameraId = resolveKeyframeTargetCamera(ctx);
    if (cameraId.empty()) return false;

    int const tick = std::clamp(
        mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick,
        0,
        std::max(0, ctx.state.totalTicks)
    );
    EditorAction action{EditorActionType::AddCameraKeyframe};
    action.id   = cameraId;
    action.tick = tick;
    if (ctx.commands.shortcutKeyframeLensMode() == ShortcutKeyframeLensMode::FreeCamera) {
        action.value = true;
        action.kind  = static_cast<int>(state::editing::model::CameraKeyframeMode::Interpolated);
    } else if (ctx.state.followingRecordedPlayer) {
        action.value = true;
        action.kind  = static_cast<int>(
            ctx.state.followThirdPerson ? state::editing::model::CameraKeyframeMode::ThirdPerson
                                        : state::editing::model::CameraKeyframeMode::FirstPerson
        );
        action.secondaryTick = 1;
    } else {
        action.value = true;
        action.kind  = static_cast<int>(state::editing::model::CameraKeyframeMode::Interpolated);
    }
    ctx.submitAction(std::move(action));
    queueCameraKeyframeSelection(std::move(cameraId), tick);
    ctx.commands.showSelectionInspector();
    return true;
}

// Resolves a target track so the shortcut works without a prior selection.
std::string TimelinePanel::resolveKeyframeTargetCamera(PanelContext const& ctx) const {
    auto const project = ctx.state.project;
    if (!project) return {};

    auto const isUsable = [&](std::string const& id) {
        return std::ranges::any_of(project->cameras, [&](auto const& camera) {
            return camera.id == id && !camera.locked;
        });
    };

    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selected && isUsable(selected->cameraId)) {
        return selected->cameraId;
    }
    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
        selected && isUsable(selected->trackId)) {
        return selected->trackId;
    }
    for (auto const& camera : project->cameras) {
        if (camera.enabled && !camera.locked) return camera.id;
    }
    for (auto const& camera : project->cameras) {
        if (!camera.locked) return camera.id;
    }
    return {};
}

namespace {

std::vector<state::editing::model::KeyframeRef> selectedKeyframeRefs(PanelContext const& ctx) {
    std::vector<state::editing::model::KeyframeRef> refs;
    refs.reserve(ctx.selection.keyframeCount());
    for (auto const& key : ctx.selection.keyframes()) refs.push_back({key.trackId, key.tick, std::nullopt});
    return refs;
}

} // namespace

bool TimelinePanel::deleteSelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;

    EditorAction action;
    if (ctx.selection.clipCutCount() > 0) {
        action.type     = EditorActionType::HealClipCuts;
        action.clipCuts = ctx.selection.clipCuts();
        if (ctx.selection.keyframeCount() > 0) {
            action.keyframeRefs = selectedKeyframeRefs(ctx);
        }
    } else if (ctx.selection.keyframeCount() > 1) {
        action.type         = EditorActionType::DeleteCameraKeyframes;
        action.keyframeRefs = selectedKeyframeRefs(ctx);
    } else if (auto const* selectedMarker = ctx.selection.getAs<state::editing::model::SelectedMarker>()) {
        action.type = EditorActionType::DeleteMarker;
        action.id   = selectedMarker->markerId;
    } else if (auto const* selectedClip = ctx.selection.getAs<state::editing::model::SelectedClip>()) {
        auto const hit = state::editing::ClipOps::findClipWithCamera(*project, selectedClip->clipId);
        if (!hit || hit.camera->locked || hit.clip->locked) return false;
        action.type = EditorActionType::DeleteClip;
        action.id   = selectedClip->clipId;
    } else if (auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
               selectedCamera && project->cameras.size() > 1) {
        action.type = EditorActionType::DeleteCamera;
        action.id   = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>()) {
        action.type = EditorActionType::DeleteCameraKeyframe;
        action.id   = selectedKeyframe->trackId;
        action.tick = selectedKeyframe->tick;
    } else if (ctx.selection.isSpeedTrackSelected()) {
        if (!project->speedTrack.isVisible() || project->speedTrack.locked) return false;
        action.type = EditorActionType::RemoveSpeedTrack;
    } else if (auto const speedTick = ctx.selection.selectedSpeedKeyframe()) {
        if (project->speedTrack.locked) return false;
        action.type = EditorActionType::DeleteSpeedKeyframe;
        action.tick = *speedTick;
    } else if (ctx.selection.isFreezeTrackSelected()) {
        if (!project->freezeTrack.isVisible() || project->freezeTrack.locked) return false;
        action.type = EditorActionType::RemoveFreezeTrack;
    } else if (auto const freezeTick = ctx.selection.selectedFreezeKeyframe()) {
        if (project->freezeTrack.locked) return false;
        action.type = EditorActionType::DeleteFreezeKeyframe;
        action.tick = *freezeTick;
    } else if (ctx.selection.isTimelapseTrackSelected()) {
        if (!project->timelapseTrack.isVisible() || project->timelapseTrack.locked) return false;
        action.type = EditorActionType::RemoveTimelapseTrack;
    } else if (auto const timelapseTick = ctx.selection.selectedTimelapseKeyframe()) {
        if (project->timelapseTrack.locked) return false;
        action.type = EditorActionType::DeleteTimelapseKeyframe;
        action.tick = *timelapseTick;
    } else if (ctx.selection.isTimeOfDayTrackSelected()) {
        if (!project->timeOfDayTrack.isVisible() || project->timeOfDayTrack.locked) return false;
        action.type = EditorActionType::RemoveTimeOfDayTrack;
    } else if (auto const todTicks = ctx.selection.selectedTimeOfDayKeyframes(); !todTicks.empty()) {
        if (project->timeOfDayTrack.locked) return false;
        if (todTicks.size() > 1) {
            action.type  = EditorActionType::RemoveTimeOfDayKeyframes;
            action.ticks = todTicks;
        } else {
            action.type = EditorActionType::RemoveTimeOfDayKeyframe;
            action.tick = todTicks.front();
        }
    } else if (ctx.selection.isWeatherTrackSelected()) {
        if (!project->weatherTrack.isVisible() || project->weatherTrack.locked) return false;
        action.type = EditorActionType::RemoveWeatherTrack;
    } else if (auto const weatherTicks = ctx.selection.selectedWeatherKeyframes(); !weatherTicks.empty()) {
        if (project->weatherTrack.locked) return false;
        if (weatherTicks.size() > 1) {
            action.type  = EditorActionType::RemoveWeatherKeyframes;
            action.ticks = weatherTicks;
        } else {
            action.type = EditorActionType::RemoveWeatherKeyframe;
            action.tick = weatherTicks.front();
        }
    } else if (auto const audioClip = ctx.selection.selectedAudioClip()) {
        auto const* track = state::editing::ClipOps::findAudioTrack(*project, audioClip->trackId);
        if (!track || track->locked || !state::editing::ClipOps::findAudioClip(*track, audioClip->clipId)) {
            return false;
        }
        action.type        = EditorActionType::RemoveAudioClip;
        action.id          = audioClip->trackId;
        action.secondaryId = audioClip->clipId;
    } else if (auto const audioTrackId = ctx.selection.selectedAudioTrackId()) {
        auto const* track = state::editing::ClipOps::findAudioTrack(*project, *audioTrackId);
        if (!track || track->locked) return false;
        action.type = EditorActionType::RemoveAudioTrack;
        action.id   = *audioTrackId;
    } else {
        return false;
    }

    ctx.submitAction(std::move(action));
    ctx.selection.clear();
    return true;
}

bool TimelinePanel::selectAllKeyframes(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;
    if ((ctx.selection.isTimeOfDayTrackSelected() || !ctx.selection.selectedTimeOfDayKeyframes().empty())
        && !project->timeOfDayTrack.locked && !project->timeOfDayTrack.keysByTick.empty()) {
        std::vector<int> ticks;
        ticks.reserve(project->timeOfDayTrack.keysByTick.size());
        for (auto const& [tick, _] : project->timeOfDayTrack.keysByTick) ticks.push_back(tick);
        if (ticks.size() == 1) ctx.selection.selectTimeOfDayKeyframe(ticks.front());
        else ctx.selection.selectTimeOfDayKeyframes(std::move(ticks));
        return true;
    }
    if ((ctx.selection.isWeatherTrackSelected() || !ctx.selection.selectedWeatherKeyframes().empty())
        && !project->weatherTrack.locked && !project->weatherTrack.keysByTick.empty()) {
        std::vector<int> ticks;
        ticks.reserve(project->weatherTrack.keysByTick.size());
        for (auto const& [tick, _] : project->weatherTrack.keysByTick) ticks.push_back(tick);
        if (ticks.size() == 1) ctx.selection.selectWeatherKeyframe(ticks.front());
        else ctx.selection.selectWeatherKeyframes(std::move(ticks));
        return true;
    }
    // Locked tracks stay out so a group move is never rejected because of them.
    std::vector<state::editing::model::SelectedKeyframe> keys;
    for (auto const& camera : project->cameras) {
        if (camera.locked) continue;
        for (auto const& [tick, _] : camera.keysByTick) keys.push_back({camera.id, tick});
    }
    if (keys.empty()) return false;
    ctx.selection.setKeyframes(std::move(keys));
    return true;
}

bool TimelinePanel::copySelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project || ctx.selection.keyframeCount() == 0) return false;

    std::vector<ClipboardKey> clip;
    int                       firstTick = std::numeric_limits<int>::max();
    for (auto const& selected : ctx.selection.keyframes()) {
        auto const camera = std::ranges::find(project->cameras, selected.trackId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) continue;
        auto const key = camera->keysByTick.find(selected.tick);
        if (key == camera->keysByTick.end()) continue;
        clip.push_back({camera->id, key->first, key->second});
        firstTick = std::min(firstTick, key->first);
    }
    if (clip.empty()) return false;
    for (auto& entry : clip) entry.offset -= firstTick;
    mKeyframeClipboard = std::move(clip);
    return true;
}

bool TimelinePanel::pasteAtPlayhead(PanelContext const& ctx) {
    int const tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    return pasteKeyframes(ctx, tick, resolveKeyframeTargetCamera(ctx));
}

bool TimelinePanel::pasteKeyframes(PanelContext const& ctx, int anchorTick, std::string const& targetCameraId) {
    auto const project = ctx.state.project;
    if (!project || mKeyframeClipboard.empty()) return false;

    bool const singleCamera = std::ranges::all_of(mKeyframeClipboard, [&](ClipboardKey const& entry) {
        return entry.cameraId == mKeyframeClipboard.front().cameraId;
    });
    std::vector<state::editing::model::KeyframeRef>      refs;
    std::vector<state::editing::model::SelectedKeyframe> landed;
    for (auto const& entry : mKeyframeClipboard) {
        std::string const& cameraId = singleCamera && !targetCameraId.empty() ? targetCameraId : entry.cameraId;
        int const          tick     = anchorTick + entry.offset;
        if (tick < 0 || tick > ctx.state.totalTicks) continue;
        auto const camera = std::ranges::find(project->cameras, cameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end() || camera->locked || camera->keysByTick.contains(tick)) continue;
        refs.push_back({cameraId, tick, entry.pose});
        landed.push_back({cameraId, tick});
    }
    if (refs.empty()) return false;

    EditorAction action{EditorActionType::AddCameraKeyframes};
    action.keyframeRefs = std::move(refs);
    ctx.submitAction(std::move(action));
    queueKeyframeSelection(std::move(landed));
    ctx.commands.showSelectionInspector();
    return true;
}

bool TimelinePanel::addMarkerAtPlayhead(PanelContext const& ctx) {
    return addMarkerAt(ctx, mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick);
}

bool TimelinePanel::addMarkerAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    // One marker per tick: pressing M on an existing one just selects it.
    if (auto const existing = std::ranges::find(project->markers, tick, &state::editing::model::Marker::tick);
        existing != project->markers.end()) {
        ctx.selection.select(state::editing::model::SelectedMarker{existing->id});
        return false;
    }
    std::string id = state::editing::command::nextMarkerId(*project);
    EditorAction action{EditorActionType::AddMarker};
    action.id   = id;
    action.tick = tick;
    action.name = "playback.refactorEditor.defaults.marker"_tr(project->markers.size() + 1);
    ctx.submitAction(std::move(action));
    queueMarkerSelection(std::move(id));
    return true;
}

bool TimelinePanel::addFreezeKeyframeAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project || project->freezeTrack.locked) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    if (project->freezeTrack.keysByTick.contains(tick)) return false;
    EditorAction action{EditorActionType::AddFreezeKeyframe};
    action.tick  = tick;
    action.value = defaultFreezeStateAt(project->freezeTrack, tick);
    action.kind  = 0;
    ctx.submitAction(std::move(action));
    queueFreezeKeyframeSelection(tick);
    ctx.commands.showSelectionInspector();
    return true;
}

bool TimelinePanel::addTimelapseKeyframeAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project || project->timelapseTrack.locked) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    if (project->timelapseTrack.keysByTick.contains(tick)) return false;
    int const    suggested = keyframe::suggestTimelapseOutputTicks(project->timelapseTrack, tick);
    EditorAction action{EditorActionType::AddTimelapseKeyframe};
    action.tick          = tick;
    action.secondaryTick = suggested;
    ctx.submitAction(std::move(action));
    queueTimelapseKeyframeSelection(tick);
    ctx.commands.showSelectionInspector();
    return true;
}

bool TimelinePanel::addTimeOfDayKeyframeAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project || project->timeOfDayTrack.locked) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    if (project->timeOfDayTrack.keysByTick.contains(tick)) return false;
    std::optional<double> sampled;
    if (!project->timeOfDayTrack.keysByTick.empty()) {
        if (project->timeOfDayTrack.enabled) {
            sampled = keyframe::evaluateTimeOfDayAt(project->timeOfDayTrack, static_cast<double>(tick));
        } else {
            auto enabledTrack    = project->timeOfDayTrack;
            enabledTrack.enabled = true;
            sampled              = keyframe::evaluateTimeOfDayAt(enabledTrack, static_cast<double>(tick));
        }
    } else {
        sampled = keyframe::resolveEffectiveTimeOfDay(*project, static_cast<double>(tick));
    }
    int const timeOfDay =
        sampled ? state::editing::model::clampTimeOfDayTicks(static_cast<int>(std::llround(*sampled))) : 6000;
    EditorAction action{EditorActionType::AddTimeOfDayKeyframe};
    action.tick          = tick;
    action.secondaryTick = timeOfDay;
    action.kind          = static_cast<int>(state::editing::model::CameraInterpolationType::Linear);
    ctx.submitAction(std::move(action));
    queueTimeOfDayKeyframeSelection(tick);
    ctx.commands.showSelectionInspector();
    return true;
}

bool TimelinePanel::addWeatherKeyframeAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project || project->weatherTrack.locked) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    if (project->weatherTrack.keysByTick.contains(tick)) return false;

    state::editing::model::WeatherOverride weather = state::editing::model::WeatherOverride::Rain;
    if (!project->weatherTrack.keysByTick.empty()) {
        std::optional<state::editing::model::WeatherOverride> sampled;
        if (project->weatherTrack.enabled) {
            sampled = keyframe::evaluateWeatherAt(project->weatherTrack, static_cast<double>(tick));
        } else {
            auto enabledTrack    = project->weatherTrack;
            enabledTrack.enabled = true;
            sampled              = keyframe::evaluateWeatherAt(enabledTrack, static_cast<double>(tick));
        }
        weather = sampled ? *sampled : project->weatherTrack.keysByTick.begin()->second.weather;
    } else if (project->weatherOverride != state::editing::model::WeatherOverride::Recorded) {
        weather = project->weatherOverride;
    }

    EditorAction action{EditorActionType::AddWeatherKeyframe};
    action.tick = tick;
    action.kind = static_cast<int>(weather);
    ctx.submitAction(std::move(action));
    queueWeatherKeyframeSelection(tick);
    ctx.commands.showSelectionInspector();
    return true;
}

bool TimelinePanel::promptAndAddAudioClip(PanelContext const& ctx, std::string const& trackId, int tick) {
    auto const project = ctx.state.project;
    if (!project) return false;
    auto const* track = state::editing::ClipOps::findAudioTrack(*project, trackId);
    if (!track || track->locked) return false;
    auto const picked = playback::utils::pickAudioFile();
    if (!picked) return false;
    std::string const clipId = state::editing::ClipOps::nextAudioClipId(*project);
    EditorAction      action{EditorActionType::AddAudioClip};
    action.id   = trackId;
    action.path = *picked;
    action.tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    ctx.submitAction(std::move(action));
    queueAudioClipSelection(trackId, clipId);
    return true;
}

bool TimelinePanel::canSplitClipAtPlayhead(PanelContext const& ctx) const {
    auto const project = ctx.state.project;
    if (!project) return false;
    int const tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    return findSplittableClip(*project, ctx.selection, tick).has_value();
}

bool TimelinePanel::splitClipAtPlayhead(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;
    int const  tick  = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    auto const match = findSplittableClip(*project, ctx.selection, tick);
    if (!match) return false;

    EditorAction action{EditorActionType::SplitClip};
    action.id   = *match;
    action.tick = tick;
    ctx.submitAction(std::move(action));
    return true;
}

bool TimelinePanel::canTrimClipAtPlayhead(PanelContext const& ctx, bool trimIn) const {
    auto const project = ctx.state.project;
    if (!project) return false;
    int const tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    return findTrimmableClip(*project, ctx.selection, tick, ctx.state.totalTicks, trimIn).has_value();
}

bool TimelinePanel::trimClipAtPlayhead(PanelContext const& ctx, bool trimIn) {
    auto const project = ctx.state.project;
    if (!project) return false;
    int const  tick  = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    auto const match = findTrimmableClip(*project, ctx.selection, tick, ctx.state.totalTicks, trimIn);
    if (!match) return false;

    EditorAction action{EditorActionType::MoveAndTrimClip};
    action.id            = match->clipId;
    action.tick          = match->newInTick;
    action.secondaryTick = match->newOutTick;
    ctx.submitAction(std::move(action));
    return true;
}

void TimelinePanel::seekAdjacentMarker(PanelContext const& ctx, bool forward) {
    auto const project = ctx.state.project;
    if (!project) return;
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    int       target   = -1;
    for (auto const& marker : project->markers) {
        if (forward ? marker.tick <= baseTick : marker.tick >= baseTick) continue;
        if (target < 0 || (forward ? marker.tick < target : marker.tick > target)) target = marker.tick;
    }
    if (target >= 0) submitSeek(ctx, target);
}

int TimelinePanel::snapToEditPoints(
    PanelContext const&                                 ctx,
    int                                                 tick,
    float                                               pixelsPerTick,
    bool                                                includePlayhead,
    bool                                                includeExportRange,
    std::function<bool(std::string const&, int)> const& skipKey,
    std::string_view                                    skipMarkerId,
    std::string_view                                    skipClipId,
    std::optional<int>                                  skipSpeedKeyTick,
    std::optional<int>                                  skipFreezeKeyTick,
    std::optional<int>                                  skipTimelapseKeyTick,
    std::string_view                                    skipAudioClipId,
    std::function<bool(int)> const&                     skipTimeOfDayKey,
    std::function<bool(int)> const&                     skipWeatherKey
) {
    auto const project = ctx.state.project;
    if (!project || pixelsPerTick <= 0.0f) return tick;

    float const radius   = kSnapPixelRadius * metrics::scale();
    float       bestDist = std::numeric_limits<float>::max();
    int         best     = -1;
    auto const  consider = [&](int candidate) {
        if (candidate < 0) return;
        float const distance = static_cast<float>(std::abs(candidate - tick)) * pixelsPerTick;
        if (distance <= radius && distance < bestDist) {
            bestDist = distance;
            best     = candidate;
        }
    };
    for (auto const& camera : project->cameras) {
        for (auto const& [keyTick, _] : camera.keysByTick) {
            if (skipKey && skipKey(camera.id, keyTick)) continue;
            consider(keyTick);
        }
        for (auto const& clip : camera.clips) {
            if (clip.id == skipClipId) continue;
            consider(clip.inTick);
            consider(clip.outTick);
        }
    }
    for (auto const& [speedTick, _] : project->speedTrack.keysByTick) {
        if (skipSpeedKeyTick && *skipSpeedKeyTick == speedTick) continue;
        consider(speedTick);
    }
    for (auto const& [freezeTick, _] : project->freezeTrack.keysByTick) {
        if (skipFreezeKeyTick && *skipFreezeKeyTick == freezeTick) continue;
        consider(freezeTick);
    }
    for (auto const& [timelapseTick, _] : project->timelapseTrack.keysByTick) {
        if (skipTimelapseKeyTick && *skipTimelapseKeyTick == timelapseTick) continue;
        consider(timelapseTick);
    }
    for (auto const& [todTick, _] : project->timeOfDayTrack.keysByTick) {
        if (skipTimeOfDayKey && skipTimeOfDayKey(todTick)) continue;
        consider(todTick);
    }
    for (auto const& [weatherTick, _] : project->weatherTrack.keysByTick) {
        if (skipWeatherKey && skipWeatherKey(weatherTick)) continue;
        consider(weatherTick);
    }
    for (auto const& audioTrack : project->audioTracks) {
        for (auto const& audioClip : audioTrack.clips) {
            if (!skipAudioClipId.empty() && audioClip.id == skipAudioClipId) continue;
            consider(audioClip.startTick);
        }
    }
    for (auto const& marker : project->markers) {
        if (marker.id != skipMarkerId) consider(marker.tick);
    }
    if (includePlayhead) consider(mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick);
    if (includeExportRange) {
        consider(ctx.commands.exportStartTick());
        consider(ctx.commands.exportEndTick());
    }
    if (best < 0) return tick;
    mSnapGuideTick = best;
    return best;
}

void TimelinePanel::clearPendingSelection() {
    mPendingKeySelection.clear();
    mPendingMarkerSelection.clear();
    mPendingSpeedTrackSelection = false;
    mPendingSpeedKeyframeSelection.reset();
    mPendingFreezeTrackSelection = false;
    mPendingFreezeKeyframeSelection.reset();
    mPendingTimelapseTrackSelection = false;
    mPendingTimelapseKeyframeSelection.reset();
    mPendingTimeOfDayTrackSelection = false;
    mPendingTimeOfDayKeyframeSelection.reset();
    mPendingTimeOfDayKeyframesSelection.clear();
    mPendingWeatherTrackSelection = false;
    mPendingWeatherKeyframeSelection.reset();
    mPendingWeatherKeyframesSelection.clear();
    mPendingAudioTrackSelection.clear();
    mPendingAudioClipTrackId.clear();
    mPendingAudioClipId.clear();
    mPendingSelectionFrames = 0;
}

void TimelinePanel::queueKeyframeSelection(std::vector<state::editing::model::SelectedKeyframe> keys) {
    clearPendingSelection();
    mPendingKeySelection    = std::move(keys);
    mPendingSelectionFrames = kPendingSelectionFrames;
}

void TimelinePanel::queueCameraKeyframeSelection(std::string cameraId, int tick) {
    queueKeyframeSelection({
        {std::move(cameraId), tick}
    });
}

void TimelinePanel::queueMarkerSelection(std::string markerId) {
    clearPendingSelection();
    mPendingMarkerSelection = std::move(markerId);
    mPendingSelectionFrames = kPendingSelectionFrames;
}

void TimelinePanel::queueSpeedTrackSelection() {
    clearPendingSelection();
    mPendingSpeedTrackSelection = true;
    mPendingSelectionFrames     = kPendingSelectionFrames;
}

void TimelinePanel::queueSpeedKeyframeSelection(int tick) {
    clearPendingSelection();
    mPendingSpeedKeyframeSelection = tick;
    mPendingSelectionFrames        = kPendingSelectionFrames;
}

void TimelinePanel::queueFreezeTrackSelection() {
    clearPendingSelection();
    mPendingFreezeTrackSelection = true;
    mPendingSelectionFrames      = kPendingSelectionFrames;
}

void TimelinePanel::queueFreezeKeyframeSelection(int tick) {
    clearPendingSelection();
    mPendingFreezeKeyframeSelection = tick;
    mPendingSelectionFrames         = kPendingSelectionFrames;
}

void TimelinePanel::queueTimelapseTrackSelection() {
    clearPendingSelection();
    mPendingTimelapseTrackSelection = true;
    mPendingSelectionFrames         = kPendingSelectionFrames;
}

void TimelinePanel::queueTimelapseKeyframeSelection(int tick) {
    clearPendingSelection();
    mPendingTimelapseKeyframeSelection = tick;
    mPendingSelectionFrames            = kPendingSelectionFrames;
}

void TimelinePanel::queueTimeOfDayTrackSelection() {
    clearPendingSelection();
    mPendingTimeOfDayTrackSelection = true;
    mPendingSelectionFrames         = kPendingSelectionFrames;
}

void TimelinePanel::queueTimeOfDayKeyframeSelection(int tick) {
    clearPendingSelection();
    mPendingTimeOfDayKeyframeSelection = tick;
    mPendingSelectionFrames            = kPendingSelectionFrames;
}

void TimelinePanel::queueTimeOfDayKeyframesSelection(std::vector<int> ticks) {
    clearPendingSelection();
    mPendingTimeOfDayKeyframesSelection = std::move(ticks);
    mPendingSelectionFrames             = kPendingSelectionFrames;
}

void TimelinePanel::queueWeatherTrackSelection() {
    clearPendingSelection();
    mPendingWeatherTrackSelection = true;
    mPendingSelectionFrames       = kPendingSelectionFrames;
}

void TimelinePanel::queueWeatherKeyframeSelection(int tick) {
    clearPendingSelection();
    mPendingWeatherKeyframeSelection = tick;
    mPendingSelectionFrames          = kPendingSelectionFrames;
}

void TimelinePanel::queueWeatherKeyframesSelection(std::vector<int> ticks) {
    clearPendingSelection();
    mPendingWeatherKeyframesSelection = std::move(ticks);
    mPendingSelectionFrames           = kPendingSelectionFrames;
}

void TimelinePanel::queueAudioTrackSelection(std::string trackId) {
    clearPendingSelection();
    mPendingAudioTrackSelection = std::move(trackId);
    mPendingSelectionFrames     = kPendingSelectionFrames;
}

void TimelinePanel::queueAudioClipSelection(std::string trackId, std::string clipId) {
    clearPendingSelection();
    mPendingAudioClipTrackId = std::move(trackId);
    mPendingAudioClipId      = std::move(clipId);
    mPendingSelectionFrames  = kPendingSelectionFrames;
}

void TimelinePanel::applyPendingSelection(PanelContext const& ctx) {
    if (mPendingSelectionFrames <= 0) return;
    auto const project = ctx.state.project;
    if (!project) return;
    bool const lastChance = --mPendingSelectionFrames == 0;

    bool done = false;
    if (!mPendingMarkerSelection.empty()) {
        if (std::ranges::any_of(project->markers, [&](auto const& marker) { return marker.id == mPendingMarkerSelection; })) {
            ctx.selection.select(state::editing::model::SelectedMarker{mPendingMarkerSelection});
            done = true;
        }
    } else if (mPendingSpeedTrackSelection) {
        if (project->speedTrack.isVisible()) {
            ctx.selection.selectSpeedTrack();
            done = true;
        }
    } else if (mPendingSpeedKeyframeSelection.has_value()) {
        if (project->speedTrack.keysByTick.contains(*mPendingSpeedKeyframeSelection)) {
            ctx.selection.selectSpeedKeyframe(*mPendingSpeedKeyframeSelection);
            done = true;
        }
    } else if (mPendingFreezeTrackSelection) {
        if (project->freezeTrack.isVisible()) {
            ctx.selection.selectFreezeTrack();
            done = true;
        }
    } else if (mPendingFreezeKeyframeSelection.has_value()) {
        if (project->freezeTrack.keysByTick.contains(*mPendingFreezeKeyframeSelection)) {
            ctx.selection.selectFreezeKeyframe(*mPendingFreezeKeyframeSelection);
            done = true;
        }
    } else if (mPendingTimelapseTrackSelection) {
        if (project->timelapseTrack.isVisible()) {
            ctx.selection.selectTimelapseTrack();
            done = true;
        }
    } else if (mPendingTimelapseKeyframeSelection.has_value()) {
        if (project->timelapseTrack.keysByTick.contains(*mPendingTimelapseKeyframeSelection)) {
            ctx.selection.selectTimelapseKeyframe(*mPendingTimelapseKeyframeSelection);
            done = true;
        }
    } else if (mPendingTimeOfDayTrackSelection) {
        if (project->timeOfDayTrack.isVisible()) {
            ctx.selection.selectTimeOfDayTrack();
            done = true;
        }
    } else if (mPendingTimeOfDayKeyframeSelection.has_value()) {
        if (project->timeOfDayTrack.keysByTick.contains(*mPendingTimeOfDayKeyframeSelection)) {
            ctx.selection.selectTimeOfDayKeyframe(*mPendingTimeOfDayKeyframeSelection);
            done = true;
        }
    } else if (!mPendingTimeOfDayKeyframesSelection.empty()) {
        std::vector<int> present;
        for (int const tick : mPendingTimeOfDayKeyframesSelection) {
            if (project->timeOfDayTrack.keysByTick.contains(tick)) present.push_back(tick);
        }
        if (!present.empty() && (present.size() == mPendingTimeOfDayKeyframesSelection.size() || lastChance)) {
            if (present.size() == 1) ctx.selection.selectTimeOfDayKeyframe(present.front());
            else ctx.selection.selectTimeOfDayKeyframes(std::move(present));
            done = true;
        }
    } else if (mPendingWeatherTrackSelection) {
        if (project->weatherTrack.isVisible()) {
            ctx.selection.selectWeatherTrack();
            done = true;
        }
    } else if (mPendingWeatherKeyframeSelection.has_value()) {
        if (project->weatherTrack.keysByTick.contains(*mPendingWeatherKeyframeSelection)) {
            ctx.selection.selectWeatherKeyframe(*mPendingWeatherKeyframeSelection);
            done = true;
        }
    } else if (!mPendingWeatherKeyframesSelection.empty()) {
        std::vector<int> present;
        for (int const tick : mPendingWeatherKeyframesSelection) {
            if (project->weatherTrack.keysByTick.contains(tick)) present.push_back(tick);
        }
        if (!present.empty() && (present.size() == mPendingWeatherKeyframesSelection.size() || lastChance)) {
            if (present.size() == 1) ctx.selection.selectWeatherKeyframe(present.front());
            else ctx.selection.selectWeatherKeyframes(std::move(present));
            done = true;
        }
    } else if (!mPendingAudioTrackSelection.empty()) {
        if (state::editing::ClipOps::findAudioTrack(*project, mPendingAudioTrackSelection) != nullptr) {
            ctx.selection.selectAudioTrack(mPendingAudioTrackSelection);
            done = true;
        }
    } else if (!mPendingAudioClipTrackId.empty() && !mPendingAudioClipId.empty()) {
        if (auto const* track = state::editing::ClipOps::findAudioTrack(*project, mPendingAudioClipTrackId);
            track && state::editing::ClipOps::findAudioClip(*track, mPendingAudioClipId) != nullptr) {
            ctx.selection.selectAudioClip(mPendingAudioClipTrackId, mPendingAudioClipId);
            done = true;
        }
    } else {
        std::vector<state::editing::model::SelectedKeyframe> present;
        for (auto const& key : mPendingKeySelection) {
            auto const camera = std::ranges::find(project->cameras, key.trackId, &state::editing::model::CameraEntity::id);
            if (camera != project->cameras.end() && camera->keysByTick.contains(key.tick)) present.push_back(key);
        }
        // Wait until every key exists; on the last frame settle for whatever made it.
        if (!present.empty() && (present.size() == mPendingKeySelection.size() || lastChance)) {
            ctx.selection.setKeyframes(std::move(present));
            done = true;
        }
    }
    if (done || lastChance) {
        clearPendingSelection();
    }
}

std::string TimelinePanel::markerAt(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    ImVec2               mouse
) const {
    auto const project = ctx.state.project;
    if (!project || mouse.y < layout.rulerTop || mouse.y > layout.bodyTop || mouse.x < layout.canvasLeft
        || mouse.x > layout.fullMax.x) {
        return {};
    }
    float const reach    = kMarkerHalfWidth * metrics::scale() + 2.0f;
    float       bestDist = reach;
    std::string best;
    for (auto const& marker : project->markers) {
        float const distance = std::abs(scale.xAt(static_cast<float>(marker.tick)) - mouse.x);
        if (distance <= bestDist) {
            bestDist = distance;
            best     = marker.id;
        }
    }
    return best;
}

void TimelinePanel::drawMarkers(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const project = ctx.state.project;
    if (!project || project->markers.empty()) return;
    auto*       drawList = ImGui::GetWindowDrawList();
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedMarker>();
    float const half     = kMarkerHalfWidth * metrics::scale();
    float const bottom   = layout.bodyTop - 1.0f;
    float const top      = layout.rulerTop + (layout.bodyTop - layout.rulerTop) * 0.35f;
    ImVec2 const mouse   = ImGui::GetMousePos();
    for (auto const& marker : project->markers) {
        bool const  dragging = mMarkerDragId == marker.id && mMarkerDragMoved;
        int const   tick     = dragging ? mMarkerDragTick : marker.tick;
        float const x        = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft - half || x > layout.fullMax.x + half) continue;
        bool const  active = dragging || (selected && selected->markerId == marker.id);
        ImU32 const color  = active ? theme::kIconHighlight : theme::kWarning;
        // A tag shape pointing down at its tick, plus a faint line through the lanes.
        drawList->AddLine({x, layout.bodyTop}, {x, layout.bodyBottom}, theme::withAlpha(color, active ? 150 : 70), 1.0f);
        float const shoulder = bottom - half;
        drawList->AddRectFilled({x - half, top}, {x + half, shoulder}, color, 1.5f);
        drawList->AddTriangleFilled({x - half, shoulder}, {x + half, shoulder}, {x, bottom}, color);
        if (std::abs(mouse.x - x) <= half + 2.0f && mouse.y >= layout.rulerTop && mouse.y <= layout.bodyTop
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mMarkerDragId.empty()) {
            ImGui::SetTooltip("%s  %s", marker.label.c_str(), formatTick(marker.tick).c_str());
        }
    }
}

void TimelinePanel::zoomIn() { mZoomScale = std::min(kMaxZoomScale, mZoomScale * kZoomStep); }

void TimelinePanel::zoomOut() { mZoomScale = std::max(kMinZoomScale, mZoomScale / kZoomStep); }

void TimelinePanel::resetZoom() {
    mZoomScale = kMinZoomScale;
    mScrollX   = 0.0f;
}

float TimelinePanel::drawTimecodeReadout(PanelContext const& ctx, float height, int displayTick) {
    float const       scale    = metrics::scale();
    float const       x        = metrics::gutter() + 2.0f * scale;
    std::string const curStr   = formatTick(displayTick);
    std::string const totalStr = "  /  " + formatTick(ctx.state.totalTicks);
    float const       curW     = ImGui::CalcTextSize(curStr.c_str()).x;
    float const       totalW   = ImGui::CalcTextSize(totalStr.c_str()).x;
    ImVec2 const      childPos = ImGui::GetWindowPos();
    float const       textY    = widgets::textYForCentre(childPos.y + height * 0.5f, curStr.c_str());
    auto*             drawList = ImGui::GetWindowDrawList();
    drawList->AddText({childPos.x + x, textY}, IM_COL32(0xf6, 0xb8, 0x4c, 0xff), curStr.c_str());
    drawList->AddText({childPos.x + x + curW, textY}, theme::kTextDim, totalStr.c_str());
    return x + curW + totalW;
}

float TimelinePanel::drawTransportGroup(
    PanelContext const& ctx,
    float               height,
    float               width,
    float               minStartX,
    float               maxRightX
) {
    auto const& state   = ctx.state;
    float const scale   = metrics::scale();
    float const unit    = metrics::iconButton();
    float const rowY    = std::max(0.0f, (height - unit) * 0.5f);
    float const divGap  = metrics::gutter();
    float const chipGap = metrics::gutter();
    char        speedLabel[24]{};
    std::snprintf(speedLabel, sizeof(speedLabel), "%.2fx", state.playbackSpeed);

    int const   total      = std::max(0, state.totalTicks);
    int const   exportIn   = ctx.commands.exportStartTick();
    int const   exportOut  = ctx.commands.exportEndTick();
    bool const  hasIn      = total > 0 && exportIn > 0 && (exportOut < 0 || exportIn < exportOut);
    bool const  hasOut     = total > 0 && exportOut >= 0 && exportOut < total && (exportIn < 0 || exportOut > exportIn);
    bool const  ranged     = hasIn || hasOut;
    float const speedWidth = widgets::dropdownChipWidth(speedLabel);
    float const ioBtnW     = std::round(unit + 4.0f * scale);
    float const ioPillW    = ioBtnW * 2.0f + 1.0f;
    float const groupWidth =
        unit * 5.0f + 4.0f + divGap * 2.0f + 1.0f + speedWidth + chipGap + unit + divGap * 2.0f + 1.0f + ioPillW;
    float const idealStartX = (width - groupWidth) * 0.5f;
    float const maxStartX   = maxRightX - groupWidth;
    float const startX =
        maxStartX >= minStartX ? std::clamp(idealStartX, minStartX, maxStartX) : std::max(minStartX, idealStartX);

    ImGui::SetCursorPos({startX, rowY});
    using widgets::VectorIcon;
    if (widgets::vectorIconButton(
            "transport-start",
            VectorIcon::SkipStart,
            "playback.refactorEditor.timeline.skipToStart"_tr().c_str()
        ))
        seekSmartEdge(ctx, false);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-prev",
            VectorIcon::StepBack,
            "playback.refactorEditor.timeline.previousTick"_tr().c_str()
        ))
        seekRelative(ctx, -1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-play",
            state.paused ? VectorIcon::Play : VectorIcon::Pause,
            (state.paused ? "playback.refactorEditor.timeline.play"_tr()
                          : "playback.refactorEditor.timeline.pause"_tr())
                .c_str()
        )) {
        ctx.submitAction({EditorActionType::TogglePause});
    }
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-next",
            VectorIcon::StepForward,
            "playback.refactorEditor.timeline.nextTick"_tr().c_str()
        ))
        seekRelative(ctx, 1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-end",
            VectorIcon::SkipEnd,
            "playback.refactorEditor.timeline.skipToEnd"_tr().c_str()
        ))
        seekSmartEdge(ctx, true);

    ImVec2 const childPos = ImGui::GetWindowPos();
    float const  divX     = childPos.x + startX + unit * 5.0f + 4.0f + divGap;
    float const  divPad   = 6.0f * scale;
    ImGui::GetWindowDrawList()
        ->AddLine({divX, childPos.y + divPad}, {divX, childPos.y + height - divPad}, theme::kLine, 1.0f);

    ImGui::SameLine(0.0f, divGap * 2.0f + 1.0f);
    if (widgets::dropdownChip(
            "transport-speed",
            speedLabel,
            "playback.refactorEditor.timeline.speedTooltip"_tr().c_str()
        )) {
        widgets::openAnchoredPopup("##transport-speed-menu");
    }
    // The menu lists the speeds themselves; stepping up and down stays on the keyboard shortcuts.
    if (widgets::beginAnchoredPopup("##transport-speed-menu")) {
        for (float const speed : replay::ReplaySession::playbackSpeeds()) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(speed));
            if (ImGui::MenuItem(label, nullptr, std::abs(state.playbackSpeed - speed) < 0.0001f)) {
                EditorAction action{EditorActionType::SetPlaybackSpeed};
                action.speed = speed;
                ctx.submitAction(std::move(action));
            }
        }
        widgets::endAnchoredPopup();
    }

    bool const maximized = ctx.commands.isViewportMaximized();
    ImGui::SameLine(0.0f, chipGap);
    if (widgets::vectorIconButton(
            "transport-maximize",
            maximized ? VectorIcon::Restore : VectorIcon::Maximize,
            (maximized ? "playback.refactorEditor.timeline.restore"_tr()
                       : "playback.refactorEditor.timeline.maximize"_tr())
                .c_str(),
            maximized
        )) {
        ctx.commands.toggleViewportMaximized();
    }

    float const div2X =
        childPos.x + startX + unit * 5.0f + 4.0f + divGap * 2.0f + 1.0f + speedWidth + chipGap + unit + divGap;
    ImGui::GetWindowDrawList()
        ->AddLine({div2X, childPos.y + divPad}, {div2X, childPos.y + height - divPad}, theme::kLine, 1.0f);

    std::string inTooltip = "playback.refactorEditor.timeline.setInPoint"_tr() + " ("
                          + input::KeyMap::displayString(input::EditorKeybind::MarkExportIn) + ")";
    if (hasIn) {
        inTooltip += "\n" + "playback.refactorEditor.timeline.currentInPoint"_tr(widgets::formatTick(exportIn));
    }
    inTooltip += "\n" + "playback.refactorEditor.timeline.ioButtonHint"_tr();

    std::string outTooltip = "playback.refactorEditor.timeline.setOutPoint"_tr() + " ("
                           + input::KeyMap::displayString(input::EditorKeybind::MarkExportOut) + ")";
    if (hasOut) {
        outTooltip += "\n" + "playback.refactorEditor.timeline.currentOutPoint"_tr(widgets::formatTick(exportOut));
    }
    outTooltip += "\n" + "playback.refactorEditor.timeline.ioButtonHint"_tr();

    ImGui::SameLine(0.0f, divGap * 2.0f + 1.0f);
    {
        auto*        ioDrawList = ImGui::GetWindowDrawList();
        ImVec2 const pillOrigin = ImGui::GetCursorScreenPos();
        ImVec2 const pillMax{pillOrigin.x + ioPillW, pillOrigin.y + unit};
        float const  rounding = theme::kButtonRounding * scale;
        float const  midX     = pillOrigin.x + ioBtnW;

        // Shared recessed capsule matching the speed dropdown chip beside it.
        ioDrawList->AddRectFilled(pillOrigin, pillMax, theme::kInputBg, rounding);

        auto const drawIoHalf = [&](char const*  id,
                                    ImVec2 const halfMin,
                                    ImVec2 const halfMax,
                                    ImDrawFlags  cornerFlags,
                                    VectorIcon   icon,
                                    bool         active,
                                    char const*  tooltip) {
            ImGui::SetCursorScreenPos(halfMin);
            ImGui::BeginDisabled(total <= 0);
            bool const clicked = ImGui::InvisibleButton(id, {halfMax.x - halfMin.x, halfMax.y - halfMin.y});
            bool const hovered = total > 0 && ImGui::IsItemHovered();
            bool const held    = total > 0 && ImGui::IsItemActive();
            ImGui::EndDisabled();

            if (active) {
                ImU32 const fill = theme::withAlpha(theme::kWarning, held ? 0x62 : hovered ? 0x48 : 0x2c);
                ioDrawList->AddRectFilled(halfMin, halfMax, fill, rounding, cornerFlags);
            } else if (hovered || held) {
                ioDrawList->AddRectFilled(
                    halfMin,
                    halfMax,
                    held ? theme::kInputBgActive : theme::kInputBgHover,
                    rounding,
                    cornerFlags
                );
            }

            ImU32 const iconColor = total <= 0 ? theme::kTextDim
                                  : active     ? (hovered ? theme::kIconHighlight : theme::kWarning)
                                               : (hovered ? theme::kIconHighlight : theme::kIconInactive);
            widgets::drawVectorIcon(
                ioDrawList,
                {(halfMin.x + halfMax.x) * 0.5f, (halfMin.y + halfMax.y) * 0.5f},
                unit * 0.92f,
                iconColor,
                icon
            );
            widgets::itemTooltip(tooltip);
            return std::pair{clicked, hovered};
        };

        auto const [inClicked, inHovered] = drawIoHalf(
            "transport-mark-in",
            pillOrigin,
            {midX, pillMax.y},
            ImDrawFlags_RoundCornersLeft,
            VectorIcon::MarkIn,
            hasIn,
            inTooltip.c_str()
        );
        if (inClicked) {
            if (ImGui::GetIO().KeyShift) {
                seekTo(
                    ctx,
                    state::editing::TimelineNavigation::resolveExportPointTick(total, exportIn, exportOut, false)
                );
            } else {
                ctx.commands.markExportPoint(true);
            }
        }
        if (total > 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            widgets::openAnchoredPopup("##transport-io-menu");

        auto const [outClicked, outHovered] = drawIoHalf(
            "transport-mark-out",
            {midX + 1.0f, pillOrigin.y},
            pillMax,
            ImDrawFlags_RoundCornersRight,
            VectorIcon::MarkOut,
            hasOut,
            outTooltip.c_str()
        );
        if (outClicked) {
            if (ImGui::GetIO().KeyShift) {
                seekTo(
                    ctx,
                    state::editing::TimelineNavigation::resolveExportPointTick(total, exportIn, exportOut, true)
                );
            } else {
                ctx.commands.markExportPoint(false);
            }
        }
        if (total > 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            widgets::openAnchoredPopup("##transport-io-menu");

        // Outer pill border and crisp center separator.
        ImU32 const outerBorder = (inHovered || outHovered)
                                    ? theme::kButtonBorder
                                    : (ranged ? theme::withAlpha(theme::kWarning, 0x70) : theme::kFineDivider);
        ioDrawList->AddRect(pillOrigin, pillMax, outerBorder, rounding, 0, 1.0f);
        ioDrawList->AddLine(
            {midX + 0.5f, pillOrigin.y + 3.0f * scale},
            {midX + 0.5f, pillMax.y - 3.0f * scale},
            theme::kFineDivider,
            1.0f
        );
    }

    if (widgets::beginAnchoredPopup("##transport-io-menu")) {
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.setInPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::MarkExportIn).c_str(),
                false,
                total > 0
            )) {
            ctx.commands.markExportPoint(true);
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.setOutPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::MarkExportOut).c_str(),
                false,
                total > 0
            )) {
            ctx.commands.markExportPoint(false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.jumpToInPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::JumpExportIn).c_str(),
                false,
                total > 0
            )) {
            seekTo(ctx, state::editing::TimelineNavigation::resolveExportPointTick(total, exportIn, exportOut, false));
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.jumpToOutPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::JumpExportOut).c_str(),
                false,
                total > 0
            )) {
            seekTo(ctx, state::editing::TimelineNavigation::resolveExportPointTick(total, exportIn, exportOut, true));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.clearExportRange"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::ClearExportRange).c_str(),
                false,
                ranged
            )) {
            ctx.commands.clearExportRange();
        }
        widgets::endAnchoredPopup();
    }

    return startX;
}

void TimelinePanel::drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick) {
    float const scale    = metrics::scale();
    float const height   = layout.titleBottom - layout.fullMin.y;
    float const width    = layout.fullMax.x - layout.fullMin.x;
    auto*       drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(layout.fullMin, {layout.fullMax.x, layout.titleBottom}, theme::kBgHeader);
    drawList->AddLine({layout.fullMin.x, layout.titleBottom}, {layout.fullMax.x, layout.titleBottom}, theme::kBorder);

    ImGui::SetCursorScreenPos(layout.fullMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTitle", {width, height}, false, ImGuiWindowFlags_NoScrollbar);
    bool const  exportActive = exporting::isExportActive(ctx.state.exportStatus.state);
    float const timeRight    = drawTimecodeReadout(ctx, height, displayTick);
    float const unit         = metrics::iconButton();
    float const rowY         = std::max(0.0f, (height - unit) * 0.5f);
    float const divGap       = metrics::gutter();
    float const zoomResetX   = std::max(0.0f, width - unit - metrics::gutter());
    float const rightDivX    = zoomResetX - divGap - 1.0f;
    float const clusterGap   = 2.0f;
    float const clusterW     = unit * 4.0f + clusterGap * 3.0f;
    float const clusterX     = rightDivX - divGap - clusterW;
    float const midDivX      = clusterX - divGap - 1.0f;
    float const historyW     = unit * 2.0f + clusterGap;
    float const historyX     = midDivX - divGap - historyW;
    float const leftDivX     = historyX - divGap - 1.0f;

    drawTransportGroup(ctx, height, width, timeRight + metrics::gutter() * 2.0f, leftDivX - metrics::gutter());
    float const transportRight = ImGui::GetItemRectMax().x - layout.fullMin.x;
    auto*       childDrawList  = ImGui::GetWindowDrawList();
    float const divPad         = 6.0f * scale;

    if (leftDivX >= transportRight + metrics::gutter()) {
        childDrawList->AddLine(
            {layout.fullMin.x + leftDivX, layout.fullMin.y + divPad},
            {layout.fullMin.x + leftDivX, layout.titleBottom - divPad},
            theme::kLine,
            1.0f
        );

        std::string const undoTooltip =
            ((ctx.state.canUndo && !ctx.state.undoLabel.empty())
                 ? "playback.refactorEditor.menu.undoAction"_tr(translateHistoryLabel(ctx.state.undoLabel))
                 : "playback.refactorEditor.menu.undo"_tr())
            + " (" + input::KeyMap::displayString(input::EditorKeybind::Undo) + ")";
        std::string const redoTooltip =
            ((ctx.state.canRedo && !ctx.state.redoLabel.empty())
                 ? "playback.refactorEditor.menu.redoAction"_tr(translateHistoryLabel(ctx.state.redoLabel))
                 : "playback.refactorEditor.menu.redo"_tr())
            + " (" + input::KeyMap::displayString(input::EditorKeybind::Redo) + ")";

        ImGui::SetCursorPos({historyX, rowY});
        if (iconButton("timeline-undo", ICON_UNDO, undoTooltip.c_str(), ctx.state.canUndo && !exportActive)) {
            ctx.commands.undo();
        }
        ImGui::SameLine(0.0f, clusterGap);
        if (iconButton("timeline-redo", ICON_REDO, redoTooltip.c_str(), ctx.state.canRedo && !exportActive)) {
            ctx.commands.redo();
        }

        childDrawList->AddLine(
            {layout.fullMin.x + midDivX, layout.fullMin.y + divPad},
            {layout.fullMin.x + midDivX, layout.titleBottom - divPad},
            theme::kLine,
            1.0f
        );

        ImGui::SetCursorPos({clusterX, rowY});
        if (iconButton(
                "timeline-split-clip",
                ICON_SPLIT,
                "playback.refactorEditor.timeline.splitClip"_tr().c_str(),
                canSplitClipAtPlayhead(ctx)
            )) {
            (void)splitClipAtPlayhead(ctx);
        }
        ImGui::SameLine(0.0f, clusterGap);
        if (iconButton(
                "timeline-add-keyframe",
                ICON_ADD_KEYFRAME,
                "playback.refactorEditor.timeline.addKeyframe"_tr().c_str()
            )) {
            (void)addKeyframeAtPlayhead(ctx);
        }
        ImGui::SameLine(0.0f, clusterGap);
        if (iconButton(
                "timeline-add-marker",
                ICON_ADD_MARKER,
                "playback.refactorEditor.timeline.addMarker"_tr().c_str()
            )) {
            (void)addMarkerAtPlayhead(ctx);
        }
        ImGui::SameLine(0.0f, clusterGap);
        if (iconButton(
                "timeline-delete",
                ICON_TRASH,
                "playback.refactorEditor.timeline.deleteSelection"_tr().c_str(),
                ctx.selection.hasSelection()
            )) {
            (void)deleteSelection(ctx);
        }
        childDrawList->AddLine(
            {layout.fullMin.x + rightDivX, layout.fullMin.y + divPad},
            {layout.fullMin.x + rightDivX, layout.titleBottom - divPad},
            theme::kLine,
            1.0f
        );
    }

    // Zooming lives on the range bar handles now; only the reset-to-fit affordance stays here.
    ImGui::SetCursorPos({zoomResetX, rowY});
    if (iconButton("timeline-zoom-reset", ICON_RESET, "playback.refactorEditor.timeline.zoomReset"_tr().c_str()))
        resetZoom();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void TimelinePanel::drawSceneTabs(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const project = ctx.state.project;
    if (!project) return;

    auto*       drawList = ImGui::GetWindowDrawList();
    float const top      = layout.titleBottom;
    float const bottom   = layout.sceneTabBottom;
    float const height   = bottom - top;
    float const width    = layout.fullMax.x - layout.fullMin.x;
    ImU32 const barBg    = IM_COL32(0x16, 0x16, 0x18, 0xff);
    drawList->AddRectFilled({layout.fullMin.x, top}, {layout.fullMax.x, bottom}, barBg);
    drawList->AddLine({layout.fullMin.x, bottom}, {layout.fullMax.x, bottom}, theme::kBorder);

    float const uiScale    = metrics::scale();
    float const padX       = 10.0f * uiScale;
    float const closeW     = 12.0f * uiScale;
    float const closeGap   = 4.0f * uiScale;
    bool const  canClose   = project->scenes.size() > 1;
    float       totalTabsW = 0.0f;
    for (auto const& scene : project->scenes) {
        ImVec2 const textSize = ImGui::CalcTextSize(scene.name.c_str());
        totalTabsW += std::max(64.0f * uiScale, textSize.x + padX * 2.0f + (canClose ? (closeGap + closeW) : 0.0f));
    }

    float const addSize  = std::max(18.0f * uiScale, height - 4.0f * uiScale);
    float const addPad   = metrics::gutter() * 0.5f;
    float const helpSize = addSize;
    float const helpPad  = metrics::gutter() * 0.75f;
    float const maxTabsW = std::max(40.0f, width - addSize - addPad * 2.0f - helpSize - helpPad * 2.0f);
    float const tabsW    = std::min(totalTabsW, maxTabsW);

    ImGui::SetCursorScreenPos({layout.fullMin.x, top});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineSceneTabs", {tabsW, height}, false, ImGuiWindowFlags_NoScrollbar);
    auto*  tabDrawList = ImGui::GetWindowDrawList();
    bool   openMenu    = false;
    ImVec2 menuMin{};
    ImVec2 menuMax{};

    float cursorX = 0.0f;
    for (size_t i = 0; i < project->scenes.size(); ++i) {
        auto const&  scene    = project->scenes[i];
        bool const   active   = scene.id == project->activeSceneId;
        ImVec2 const textSize = ImGui::CalcTextSize(scene.name.c_str());
        float const  tabW =
            std::max(64.0f * uiScale, textSize.x + padX * 2.0f + (canClose ? (closeGap + closeW) : 0.0f));

        ImVec2 const tabMin{layout.fullMin.x + cursorX - ImGui::GetScrollX(), top};
        ImVec2 const tabMax{tabMin.x + tabW, bottom};
        ImGui::SetCursorPos({cursorX, 0.0f});
        ImGui::PushID(scene.id.c_str());
        ImGui::InvisibleButton("##scene-tab", {tabW, height});
        bool const   tabHovered = allowInput && ImGui::IsItemHovered();
        ImVec2 const mouse      = ImGui::GetMousePos();
        ImVec2 const closeMin{tabMax.x - padX * 0.6f - closeW, top + (height - closeW) * 0.5f};
        ImVec2 const closeMax{closeMin.x + closeW, closeMin.y + closeW};
        bool const   overClose = canClose && tabHovered && contains(closeMin, closeMax, mouse);

        if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (overClose) {
                EditorAction action{EditorActionType::DeleteScene};
                action.id = scene.id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            } else if (!active) {
                EditorAction action{EditorActionType::SwitchScene};
                action.id = scene.id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        if (allowInput && tabHovered && !overClose && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            mSceneMenuId = scene.id;
            mSceneRenameId.clear();
            mFocusSceneRename = true;
            menuMin           = tabMin;
            menuMax           = tabMax;
            openMenu          = true;
        }
        if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            mSceneMenuId = scene.id;
            mSceneRenameId.clear();
            mFocusSceneRename = false;
            menuMin           = tabMin;
            menuMax           = tabMax;
            openMenu          = true;
        }
        ImGui::PopID();

        ImU32 const bg = active ? IM_COL32(0x21, 0x22, 0x26, 0xff) : (tabHovered ? theme::kButtonHover : barBg);
        tabDrawList->AddRectFilled(tabMin, tabMax, bg);
        tabDrawList->AddLine(
            {tabMax.x, active ? top : top + 4.0f * uiScale},
            {tabMax.x, active ? bottom : bottom - 4.0f * uiScale},
            theme::kBorder
        );
        if (active) {
            tabDrawList->AddRectFilled({tabMin.x, top}, {tabMax.x, top + 2.0f * uiScale}, theme::kAccent);
        }

        float const  textY = widgets::textYForCentre((top + bottom) * 0.5f, scene.name.c_str());
        ImVec4 const clip{tabMin.x + 2.0f, top, (canClose ? closeMin.x - 2.0f : tabMax.x - 2.0f), bottom};
        tabDrawList->AddText(
            nullptr,
            0.0f,
            {tabMin.x + padX, textY},
            active ? theme::kText : (tabHovered ? theme::kText : theme::kTextDim),
            scene.name.c_str(),
            nullptr,
            0.0f,
            &clip
        );

        if (canClose && (active || tabHovered)) {
            drawCompactIconAtCentre(
                tabDrawList,
                ICON_CLOSE,
                {(closeMin.x + closeMax.x) * 0.5f, (closeMin.y + closeMax.y) * 0.5f},
                11.0f * uiScale,
                overClose ? theme::kIconHighlight : theme::kTextDim
            );
        }

        cursorX += tabW;
    }
    if (allowInput && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        ImGui::SetScrollX(
            std::clamp(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 40.0f, 0.0f, ImGui::GetScrollMaxX())
        );
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (openMenu) {
        widgets::openAnchoredPopup("##timeline-scene-menu", menuMin, menuMax);
    }

    ImVec2 const addPos{layout.fullMin.x + tabsW + addPad, top + std::max(0.0f, (height - addSize) * 0.5f)};
    ImGui::SetCursorScreenPos(addPos);
    if (ImGui::InvisibleButton("timeline-add-scene", {addSize, addSize})) {
        ctx.submitAction({EditorActionType::AddScene});
    }
    bool const addHovered = allowInput && ImGui::IsItemHovered();
    bool const addHeld    = allowInput && ImGui::IsItemActive();
    if (addHovered || addHeld) {
        drawList->AddRectFilled(
            addPos,
            {addPos.x + addSize, addPos.y + addSize},
            addHeld ? theme::kButtonActive : theme::withAlpha(theme::kButtonHover, 0xa0),
            3.0f * uiScale
        );
    }
    drawCompactIconAtCentre(
        drawList,
        ICON_ADD,
        {addPos.x + addSize * 0.5f, addPos.y + addSize * 0.5f},
        std::round(metrics::font() * 0.92f),
        addHovered ? theme::kIconHighlight : theme::kIconInactive
    );
    widgets::itemTooltip("playback.refactorEditor.timeline.addScene"_tr().c_str());

    float const addEndX      = layout.fullMin.x + tabsW + addPad + addSize + addPad;
    float const helpX        = layout.fullMax.x - helpPad - helpSize;
    float const helpY        = top + (height - helpSize) * 0.5f;
    float const summaryRight = helpX - 6.0f * uiScale;
    float const remainW      = summaryRight - addEndX;
    if (remainW >= 140.0f * uiScale) {
        static std::vector<int> const emptyTransitions;
        auto const&                   transitions =
            ctx.state.cameraTimeline ? ctx.state.cameraTimeline->dimensionTransitionTicks() : emptyTransitions;
        auto const enabledSlices = state::editing::ClipOps::collectCompositedSlices(*project, true, transitions);
        auto const allSlices     = state::editing::ClipOps::collectCompositedSlices(*project, false, transitions);
        std::vector<std::pair<int, int>> sliceKeys;
        sliceKeys.reserve(enabledSlices.size());
        for (auto const& slice : enabledSlices) {
            sliceKeys.emplace_back(slice.inTick, slice.outTick);
        }
        bool const cacheHit = mOutputDurationCache.valid && mOutputDurationCache.totalTicks == project->totalTicks
                           && mOutputDurationCache.camerasEmpty == project->cameras.empty()
                           && mOutputDurationCache.slices == sliceKeys
                           && speedTrackEquals(mOutputDurationCache.speedTrack, project->speedTrack)
                           && mOutputDurationCache.timelapseTrack.enabled == project->timelapseTrack.enabled
                           && mOutputDurationCache.timelapseTrack.keysByTick == project->timelapseTrack.keysByTick;
        double outputSeconds = mOutputDurationCache.outputSeconds;
        if (!cacheHit) {
            keyframe::SpeedTimeMapping const mapping(*project, project->cameras.empty() ? project->totalTicks : 0);
            outputSeconds                       = mapping.totalOutputSecondsForEnabledSlices(*project);
            mOutputDurationCache.valid          = true;
            mOutputDurationCache.totalTicks     = project->totalTicks;
            mOutputDurationCache.camerasEmpty   = project->cameras.empty();
            mOutputDurationCache.speedTrack     = project->speedTrack;
            mOutputDurationCache.timelapseTrack = project->timelapseTrack;
            mOutputDurationCache.slices         = std::move(sliceKeys);
            mOutputDurationCache.outputSeconds  = outputSeconds;
        }
        char durBuf[32]{};
        if (outputSeconds >= 60.0) {
            int const    mins = static_cast<int>(outputSeconds) / 60;
            double const rem  = outputSeconds - static_cast<double>(mins * 60);
            std::snprintf(durBuf, sizeof(durBuf), "%d:%04.1f", mins, rem);
        } else {
            std::snprintf(durBuf, sizeof(durBuf), "%.1fs", outputSeconds);
        }
        std::string const summaryText = "playback.refactorEditor.timeline.sceneSummaryWithTracks"_tr(
            project->cameras.size(),
            enabledSlices.size(),
            allSlices.size(),
            durBuf
        );
        float const   summaryFont = std::max(11.0f, std::round(metrics::font() * 0.86f));
        ImFont* const font        = ImGui::GetFont();
        ImVec2 const  summarySize = font->CalcTextSizeA(summaryFont, FLT_MAX, 0.0f, summaryText.c_str());
        float const   textX       = std::max(addEndX, summaryRight - summarySize.x);
        float const   textY       = std::floor((top + bottom - summarySize.y) * 0.5f + 0.5f);
        ImVec4 const  clipRect{addEndX, top, summaryRight, bottom};
        drawList->AddText(
            font,
            summaryFont,
            {textX, textY},
            theme::kTextDim,
            summaryText.c_str(),
            nullptr,
            0.0f,
            &clipRect
        );
    }

    if (helpX >= addEndX) {
        ImVec2 const helpPos{helpX, helpY};
        ImGui::SetCursorScreenPos(helpPos);
        ImGui::InvisibleButton("##timeline-help", {helpSize, helpSize});
        bool const helpHovered = allowInput && ImGui::IsItemHovered();
        if (helpHovered) {
            drawList->AddRectFilled(
                helpPos,
                {helpPos.x + helpSize, helpPos.y + helpSize},
                theme::withAlpha(theme::kButtonHover, 0xa0),
                3.0f * uiScale
            );
        }
        drawCompactIconAtCentre(
            drawList,
            ICON_HELP,
            {helpPos.x + helpSize * 0.5f, helpPos.y + helpSize * 0.5f},
            std::round(metrics::font() * 0.90f),
            helpHovered ? theme::kIconHighlight : theme::kTextDim
        );
        widgets::itemTooltip("playback.refactorEditor.timeline.transportHelp"_tr().c_str());
    }
}

void TimelinePanel::drawToolStrip(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    (void)ctx;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const scale    = metrics::scale();
    float const stripW   = layout.toolStripRight - layout.fullMin.x;
    drawList->AddRectFilled(
        {layout.fullMin.x, layout.rulerTop},
        {layout.toolStripRight, layout.bodyBottom},
        theme::kBgHeader
    );
    drawList
        ->AddLine({layout.toolStripRight, layout.rulerTop}, {layout.toolStripRight, layout.bodyBottom}, theme::kLine);
    drawList->AddLine({layout.fullMin.x, layout.bodyTop}, {layout.toolStripRight, layout.bodyTop}, theme::kLine);

    // Top cell aligned with ruler/track-controls: compact active tool shortcut badge pill.
    {
        input::EditorKeybind const activeBind =
            mTool == TimelineTool::BoxSelect
                ? input::EditorKeybind::BoxSelectTool
                : (mTool == TimelineTool::Razor ? input::EditorKeybind::RazorTool
                                                : (mTool == TimelineTool::Pen ? input::EditorKeybind::PenTool
                                                                              : input::EditorKeybind::SelectTool));
        std::string keyText = input::KeyMap::displayString(activeBind);
        if (keyText.empty()) {
            keyText = mTool == TimelineTool::BoxSelect
                        ? "B"
                        : (mTool == TimelineTool::Razor ? "C" : (mTool == TimelineTool::Pen ? "P" : "V"));
        } else if (keyText.size() > 2) {
            keyText.resize(2);
        }
        float const   badgeFont = std::max(10.0f, std::round(metrics::font() * 0.72f));
        ImFont* const font      = ImGui::GetFont();
        ImVec2 const  keySize   = font->CalcTextSizeA(badgeFont, FLT_MAX, 0.0f, keyText.c_str());
        float const   chipW     = std::max(16.0f * scale, keySize.x + 7.0f * scale);
        float const   chipH     = std::max(14.0f * scale, layout.headerHeight - 7.0f * scale);
        float const   chipX     = layout.fullMin.x + (stripW - chipW) * 0.5f;
        float const   chipY     = layout.rulerTop + (layout.headerHeight - chipH) * 0.5f;
        drawList->AddRectFilled({chipX, chipY}, {chipX + chipW, chipY + chipH}, theme::kInputBg, 3.0f * scale);
        drawList->AddRect({chipX, chipY}, {chipX + chipW, chipY + chipH}, theme::kBorder, 3.0f * scale, 0, 1.0f);
        drawList->AddText(
            font,
            badgeFont,
            {chipX + (chipW - keySize.x) * 0.5f, std::floor(chipY + (chipH - keySize.y) * 0.5f + 0.5f)},
            theme::kAccent,
            keyText.c_str()
        );
    }

    ImGui::SetCursorScreenPos({layout.fullMin.x, layout.rulerTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineToolStrip",
        {stripW, layout.bodyBottom - layout.rulerTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    auto*       stripDrawList = ImGui::GetWindowDrawList();
    float const availBodyH    = std::max(54.0f, layout.bodyBottom - layout.bodyTop - 8.0f * scale);
    float const maxBtnForH    = std::max(18.0f, std::floor((availBodyH - 9.0f * scale) / 4.0f));
    float const btnSize =
        std::min({std::round(metrics::iconButton() * 0.92f), std::max(18.0f, stripW - 5.0f * scale), maxBtnForH});
    float const padX          = std::max(0.0f, (stripW - btnSize) * 0.5f);
    float const padTop        = layout.headerHeight + 4.0f * scale;
    float const stepY         = btnSize + 3.0f * scale;

    auto const toolStripButton =
        [&](char const* id, ImVec2 const& pos, bool active, char const* tooltip, auto&& drawGlyph) {
            ImGui::SetCursorPos(pos);
            ImVec2 const origin  = ImGui::GetCursorScreenPos();
            bool const   clicked = ImGui::InvisibleButton(id, {btnSize, btnSize});
            bool const   hovered = allowInput && ImGui::IsItemHovered();
            if (active) {
                stripDrawList->AddRectFilled(
                    origin,
                    {origin.x + btnSize, origin.y + btnSize},
                    theme::withAlpha(theme::kAccent, 0x38),
                    4.0f * scale
                );
                float const barW      = 2.5f * scale;
                float const barInsetY = 4.0f * scale;
                stripDrawList->AddRectFilled(
                    {origin.x, origin.y + barInsetY},
                    {origin.x + barW, origin.y + btnSize - barInsetY},
                    theme::kAccent,
                    1.5f * scale
                );
            } else if (hovered) {
                stripDrawList->AddRectFilled(
                    origin,
                    {origin.x + btnSize, origin.y + btnSize},
                    theme::kButtonHover,
                    4.0f * scale
                );
            }
            ImU32 const iconCol =
                active ? theme::kIconHighlight : (hovered ? theme::kIconActive : theme::kIconInactive);
            drawGlyph(ImVec2{origin.x + btnSize * 0.5f, origin.y + btnSize * 0.5f}, iconCol);
            widgets::itemTooltip(tooltip);
            return clicked && allowInput;
        };

    std::string const selectTip = "playback.refactorEditor.timeline.toolSelect"_tr() + " ("
                                + input::KeyMap::displayString(input::EditorKeybind::SelectTool) + ")";
    std::string const boxSelectTip = "playback.refactorEditor.timeline.toolBoxSelect"_tr() + " ("
                                   + input::KeyMap::displayString(input::EditorKeybind::BoxSelectTool) + ")";
    std::string const razorTip = "playback.refactorEditor.timeline.toolRazor"_tr() + " ("
                               + input::KeyMap::displayString(input::EditorKeybind::RazorTool) + ")";
    std::string const penTip = "playback.refactorEditor.timeline.toolPen"_tr() + " ("
                             + input::KeyMap::displayString(input::EditorKeybind::PenTool) + ")";

    if (toolStripButton(
            "timeline-tool-select",
            {padX, padTop},
            mTool == TimelineTool::Select,
            selectTip.c_str(),
            [&](ImVec2 const& centre, ImU32 col) {
                widgets::drawVectorIcon(stripDrawList, centre, btnSize * 0.92f, col, widgets::VectorIcon::SelectCursor);
            }
        )) {
        mTool = TimelineTool::Select;
    }
    if (toolStripButton(
            "timeline-tool-box-select",
            {padX, padTop + stepY},
            mTool == TimelineTool::BoxSelect,
            boxSelectTip.c_str(),
            [&](ImVec2 const& centre, ImU32 col) {
                widgets::drawVectorIcon(stripDrawList, centre, btnSize * 0.92f, col, widgets::VectorIcon::BoxSelect);
            }
        )) {
        mTool = TimelineTool::BoxSelect;
    }
    if (toolStripButton(
            "timeline-tool-razor",
            {padX, padTop + stepY * 2.0f},
            mTool == TimelineTool::Razor,
            razorTip.c_str(),
            [&](ImVec2 const& centre, ImU32 col) {
                drawCompactIconAtCentre(stripDrawList, ICON_SPLIT, centre, std::round(metrics::font() * 1.05f), col);
            }
        )) {
        mTool = TimelineTool::Razor;
    }
    if (toolStripButton(
            "timeline-tool-pen",
            {padX, padTop + stepY * 3.0f},
            mTool == TimelineTool::Pen,
            penTip.c_str(),
            [&](ImVec2 const& centre, ImU32 col) {
                widgets::drawVectorIcon(stripDrawList, centre, btnSize * 0.92f, col, widgets::VectorIcon::PenTool);
            }
        )) {
        mTool = TimelineTool::Pen;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void TimelinePanel::drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const  project  = ctx.state.project;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const scale    = metrics::scale();
    float const listLeft = layout.listLeft;
    float const listW    = layout.listWidth;

    drawList
        ->AddRectFilled({listLeft, layout.rulerTop}, {listLeft + listW, layout.bodyBottom}, theme::kTimelineSidebar);

    ImGui::SetCursorScreenPos({listLeft, layout.rulerTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackControls",
        {listW, layout.bodyTop - layout.rulerTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    auto*       ctrlDrawList = ImGui::GetWindowDrawList();
    float const btnSize          = std::min(layout.headerHeight - 5.0f * scale, std::round(20.0f * scale));
    float const btnY             = std::max(0.0f, (layout.headerHeight - btnSize) * 0.5f);
    bool        openAddTrackMenu = false;
    ImVec2      addMenuMin{};
    ImVec2      addMenuMax{};
    ImGui::SetCursorPos({metrics::gutter() * 0.5f, btnY});
    {
        ImVec2 const addOrigin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("timeline-add", {btnSize, btnSize}) && allowInput) {
            openAddTrackMenu = true;
            addMenuMin       = ImGui::GetItemRectMin();
            addMenuMax       = ImGui::GetItemRectMax();
        }
        bool const hovered = allowInput && ImGui::IsItemHovered();
        bool const held    = allowInput && ImGui::IsItemActive();
        if (hovered || held) {
            ctrlDrawList->AddRectFilled(
                addOrigin,
                {addOrigin.x + btnSize, addOrigin.y + btnSize},
                held ? theme::kButtonActive : theme::kButtonHover,
                3.0f * scale
            );
        }
        drawCompactIconAtCentre(
            ctrlDrawList,
            ICON_ADD,
            {addOrigin.x + btnSize * 0.5f, addOrigin.y + btnSize * 0.5f},
            std::round(metrics::font() * 0.92f),
            hovered ? theme::kIconHighlight : theme::kIconInactive
        );
        widgets::itemTooltip("playback.refactorEditor.timeline.addTrack"_tr().c_str());
    }
    ImGui::SameLine(0.0f, 3.0f * scale);
    {
        ImVec2 const snapOrigin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("timeline-snap", {btnSize, btnSize}) && allowInput) {
            ctx.commands.setSnapEnabled(!mSnapEnabled);
        }
        bool const hovered = allowInput && ImGui::IsItemHovered();
        bool const held    = allowInput && ImGui::IsItemActive();
        if (mSnapEnabled) {
            ctrlDrawList->AddRectFilled(
                snapOrigin,
                {snapOrigin.x + btnSize, snapOrigin.y + btnSize},
                theme::withAlpha(theme::kAccent, held ? 0x58 : 0x38),
                3.5f * scale
            );
            ctrlDrawList->AddRect(
                snapOrigin,
                {snapOrigin.x + btnSize, snapOrigin.y + btnSize},
                theme::withAlpha(theme::kAccent, 0xb8),
                3.5f * scale,
                0,
                1.0f
            );
        } else if (hovered || held) {
            ctrlDrawList->AddRectFilled(
                snapOrigin,
                {snapOrigin.x + btnSize, snapOrigin.y + btnSize},
                held ? theme::kButtonActive : theme::kButtonHover,
                3.5f * scale
            );
        }
        ImU32 const snapIconCol = mSnapEnabled ? (hovered ? theme::kIconHighlight : theme::kAccent)
                                               : (hovered ? theme::kIconActive : theme::kIconInactive);
        widgets::drawVectorIcon(
            ctrlDrawList,
            {snapOrigin.x + btnSize * 0.5f, snapOrigin.y + btnSize * 0.5f},
            btnSize * 0.96f,
            snapIconCol,
            widgets::VectorIcon::SnapMagnet
        );
        widgets::itemTooltip("playback.refactorEditor.timeline.snapTooltip"_tr().c_str());
    }
    ImGui::SameLine(0.0f, metrics::gutter() * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * scale, 1.5f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    // The input is frame-height, not button-height; centre it on the same line as the buttons.
    ImGui::SetCursorPosY(std::max(0.0f, (layout.headerHeight - ImGui::GetFrameHeight()) * 0.5f));
    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::SetNextItemWidth(std::max(32.0f, ImGui::GetContentRegionAvail().x - metrics::gutter() * 0.5f));
    if (ImGui::InputTextWithHint(
            "##timeline-search",
            "playback.refactorEditor.timeline.searchTracks"_tr().c_str(),
            search,
            sizeof(search)
        )) {
        mTrackSearch = search;
    }
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (openAddTrackMenu) {
        widgets::openAnchoredPopup("##timeline-add-track-menu", addMenuMin, addMenuMax);
    }
    drawList->AddLine({listLeft, layout.bodyTop}, {listLeft + listW, layout.bodyTop}, theme::kLine);

    ImGui::SetCursorScreenPos({listLeft, layout.camerasTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackList",
        {listW, std::max(1.0f, layout.bodyBottom - layout.camerasTop)},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    drawList->PushClipRect({listLeft, layout.camerasTop}, {listLeft + listW, layout.bodyBottom}, true);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) mTrackDragCancelled = false;

    auto const* selectedCamera       = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    auto const* selectedKeyframe     = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    auto const  selectedAudioTrackId = ctx.selection.selectedAudioTrackId();
    auto const  selectedAudioClip    = ctx.selection.selectedAudioClip();
    std::string selectedClipCameraId;
    if (project) {
        if (auto const* selectedClip = ctx.selection.getAs<state::editing::model::SelectedClip>()) {
            if (auto const hit = state::editing::ClipOps::findClipWithCamera(*project, selectedClip->clipId)) {
                selectedClipCameraId = hit.camera->id;
            }
        }
    }
    bool   openTrackMenu = false;
    ImVec2 trackMenuMin{};
    ImVec2 trackMenuMax{};
    bool   openSpeedMenu = false;
    ImVec2 speedMenuMin{};
    ImVec2 speedMenuMax{};
    bool   openFreezeMenu = false;
    ImVec2 freezeMenuMin{};
    ImVec2 freezeMenuMax{};
    bool   openTimelapseMenu = false;
    ImVec2 timelapseMenuMin{};
    ImVec2 timelapseMenuMax{};
    bool   openTimeOfDayMenu = false;
    ImVec2 timeOfDayMenuMin{};
    ImVec2 timeOfDayMenuMax{};
    bool   openWeatherMenu = false;
    ImVec2 weatherMenuMin{};
    ImVec2 weatherMenuMax{};
    bool   openAudioMenu = false;
    ImVec2 audioMenuMin{};
    ImVec2 audioMenuMax{};
    float  rowY = layout.camerasTop - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = rowY + row.height;
        if (rowBottom < layout.camerasTop || rowY > layout.bodyBottom) {
            rowY = rowBottom;
            continue;
        }
        bool const isSpeedRow     = row.kind == TrackRowKind::Speed;
        bool const isFreezeRow    = row.kind == TrackRowKind::Freeze;
        bool const isTimelapseRow = row.kind == TrackRowKind::Timelapse;
        bool const isTimeOfDayRow = row.kind == TrackRowKind::TimeOfDay;
        bool const isWeatherRow   = row.kind == TrackRowKind::Weather;
        bool const isAudioRow     = row.kind == TrackRowKind::Audio;
        bool const isSpecialRow   = isSpeedRow || isFreezeRow || isTimelapseRow || isTimeOfDayRow || isWeatherRow;
        bool const selected =
            isSpeedRow ? (ctx.selection.isSpeedTrackSelected() || ctx.selection.selectedSpeedKeyframe().has_value())
            : isFreezeRow
                ? (ctx.selection.isFreezeTrackSelected() || ctx.selection.selectedFreezeKeyframe().has_value())
            : isTimelapseRow
                ? (ctx.selection.isTimelapseTrackSelected() || ctx.selection.selectedTimelapseKeyframe().has_value())
            : isTimeOfDayRow
                ? (ctx.selection.isTimeOfDayTrackSelected() || !ctx.selection.selectedTimeOfDayKeyframes().empty())
            : isWeatherRow
                ? (ctx.selection.isWeatherTrackSelected() || !ctx.selection.selectedWeatherKeyframes().empty())
            : isAudioRow ? ((selectedAudioTrackId && *selectedAudioTrackId == row.id)
                            || (selectedAudioClip && selectedAudioClip->trackId == row.id))
                         : (!row.cameraId.empty()
                            && ((selectedCamera && selectedCamera->cameraId == row.cameraId)
                                || (selectedKeyframe && selectedKeyframe->trackId == row.cameraId)
                                || (!selectedClipCameraId.empty() && selectedClipCameraId == row.cameraId)));

        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listW, row.height});
        bool const  hovered         = allowInput && ImGui::IsItemHovered();
        bool const  clicked         = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool const  rowRightClicked = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        bool const  rowActive       = ImGui::IsItemActive();
        bool const  rowStationary   = hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary);
        float const selectedAmount  = mAnimator.animate("row-selected", row.id, selected ? 1.0f : 0.0f);
        float const hoverAmount     = mAnimator.animate("row-hover", row.id, hovered ? 1.0f : 0.0f);
        float const highlight       = std::max(selectedAmount, hoverAmount);
        if (highlight > 0.0f) {
            drawList->AddRectFilled(
                {listLeft, rowY},
                {listLeft + listW, rowBottom},
                lerpColor(theme::kTimelineSidebar, theme::kRowHighlight, highlight)
            );
        }
        drawList->AddLine({listLeft, rowBottom}, {listLeft + listW, rowBottom}, IM_COL32(0x25, 0x25, 0x28, 0xff));

        ImU32 const baseSwatch = isSpeedRow     ? theme::kSpeedFast
                               : isFreezeRow    ? kFreezeAccent
                               : isTimelapseRow ? kTimelapseAccent
                               : isTimeOfDayRow ? kTimeOfDayAccent
                               : isWeatherRow   ? kWeatherAccent
                               : isAudioRow     ? kAudioAccent
                                                : theme::trackColor(row.cameraIndex);
        ImU32 const swatch     = row.enabled ? baseSwatch : theme::kTrackDisabled;
        float const swatchPadX = 2.0f * scale;
        float const swatchPadY = 4.0f * scale;
        drawList->AddRectFilled(
            {listLeft + swatchPadX, rowY + swatchPadY},
            {listLeft + swatchPadX + metrics::trackSwatch(), rowBottom - swatchPadY},
            lerpColor(theme::withAlpha(swatch, 0x99), swatch, selectedAmount),
            1.5f * scale
        );

        float const indent = swatchPadX + metrics::trackSwatch() + metrics::gutter();
        float       labelX = listLeft + indent;

        ImU32 const textColor = row.enabled ? theme::kText : theme::kTextDim;
        ImU32 const iconColor =
            isSpeedRow
                ? (row.enabled ? theme::kSpeedFast : theme::kTextDim)
                : (isFreezeRow
                       ? (row.enabled ? kFreezeAccent : theme::kTextDim)
                       : (isTimelapseRow
                              ? (row.enabled ? kTimelapseAccent : theme::kTextDim)
                              : (isTimeOfDayRow
                                     ? (row.enabled ? kTimeOfDayAccent : theme::kTextDim)
                                     : (isWeatherRow ? (row.enabled ? kWeatherAccent : theme::kTextDim)
                                                     : (isAudioRow ? (row.enabled ? kAudioAccent : theme::kTextDim)
                                                                   : textColor)))));
        std::string const& label     = row.name;
        // Text centres on the fractional row, then the icon centres on the text's optical middle.
        float const rowCentre = (rowY + rowBottom) * 0.5f;
        float const textY     = widgets::textYForCentre(rowCentre, label.c_str());
        float const iconBox   = metrics::iconGlyph();
        widgets::drawIconAtCentre(
            drawList,
            isSpeedRow
                ? ICON_CHEVRONS_RIGHT
                : (isFreezeRow ? ICON_FREEZE
                               : (isTimelapseRow
                                      ? ICON_TIMELAPSE
                                      : (isTimeOfDayRow ? ICON_CLOCK
                                                        : (isWeatherRow ? ICON_WEATHER
                                                                        : (isAudioRow ? ICON_AUDIO : ICON_CAMERA))))),
            {labelX + iconBox * 0.5f, widgets::textInkCentre(textY, label.c_str())},
            iconColor
        );
        labelX += iconBox + metrics::gutter() * 0.5f;

        float const controlWidth  = std::round(metrics::font() * 1.15f);
        float const eyeX          = listLeft + listW - controlWidth - metrics::gutter();
        float const muteX         = isAudioRow ? (eyeX - controlWidth - 2.0f * scale) : eyeX;
        float const lockX         = (isAudioRow ? muteX : eyeX) - controlWidth - 2.0f * scale;
        float const addX          = lockX - controlWidth - 2.0f * scale;
        bool const  showLock      = row.locked || hovered;
        bool const  hasAddControl = isSpecialRow || isAudioRow;
        float const controlsLeft  = hasAddControl ? addX : (showLock ? lockX : eyeX);
        float const rightLimit    = controlsLeft - 4.0f * scale;
        if (rightLimit > labelX) {
            std::string const elided =
                elideTextToWidth(ImGui::GetFont(), ImGui::GetFontSize(), label, rightLimit - labelX);
            ImVec4 const nameClip{labelX, rowY, rightLimit, rowBottom};
            drawList->AddText(nullptr, 0.0f, {labelX, textY}, textColor, elided.c_str(), nullptr, 0.0f, &nameClip);
        }

        ImVec2 const mouse        = ImGui::GetMousePos();
        int const    playheadTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
        bool const   canAddSpeedKey =
            isSpeedRow && project && !row.locked && !project->speedTrack.keysByTick.contains(playheadTick);
        bool const canAddFreezeKey =
            isFreezeRow && project && !row.locked && !project->freezeTrack.keysByTick.contains(playheadTick);
        bool const canAddTimelapseKey =
            isTimelapseRow && project && !row.locked && !project->timelapseTrack.keysByTick.contains(playheadTick);
        bool const canAddTimeOfDayKey =
            isTimeOfDayRow && project && !row.locked && !project->timeOfDayTrack.keysByTick.contains(playheadTick);
        bool const canAddWeatherKey =
            isWeatherRow && project && !row.locked && !project->weatherTrack.keysByTick.contains(playheadTick);
        bool const canAddAudioClip = isAudioRow && project && !row.locked;
        bool const canAddControl   = canAddSpeedKey || canAddFreezeKey || canAddTimelapseKey || canAddTimeOfDayKey
                                || canAddWeatherKey || canAddAudioClip;
        bool const addHovered =
            hasAddControl && allowInput && contains({addX, rowY}, {addX + controlWidth, rowBottom}, mouse);
        bool const muteHovered =
            isAudioRow && allowInput && contains({muteX, rowY}, {muteX + controlWidth, rowBottom}, mouse);
        bool const eyeHovered = allowInput && contains({eyeX, rowY}, {eyeX + controlWidth, rowBottom}, mouse);
        bool const lockHovered =
            allowInput && showLock && contains({lockX, rowY}, {lockX + controlWidth, rowBottom}, mouse);
        if (addHovered) {
            widgets::hoverTooltip((isFreezeRow      ? "playback.refactorEditor.freeze.addKeyframeAtPlayhead"_tr()
                                   : isTimelapseRow ? "playback.refactorEditor.timelapse.addKeyframeAtPlayhead"_tr()
                                   : isTimeOfDayRow ? "playback.refactorEditor.timeOfDay.addKeyframe"_tr()
                                   : isWeatherRow   ? "playback.refactorEditor.weather.addKeyframeAtPlayhead"_tr()
                                   : isAudioRow     ? "playback.refactorEditor.audio.addAudioAtPlayhead"_tr()
                                                    : "playback.refactorEditor.speed.addKeyframeAtPlayhead"_tr())
                                      .c_str());
        } else if (muteHovered) {
            widgets::hoverTooltip((row.muted ? "playback.refactorEditor.audio.unmuteTrack"_tr()
                                             : "playback.refactorEditor.audio.muteTrack"_tr())
                                      .c_str());
        } else if (eyeHovered) {
            widgets::hoverTooltip((row.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                               : "playback.refactorEditor.timeline.enableTrack"_tr())
                                      .c_str());
        } else if (lockHovered) {
            widgets::hoverTooltip((row.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                              : "playback.refactorEditor.timeline.lockTrack"_tr())
                                      .c_str());
        }
        if (allowInput && addHovered && canAddControl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (isSpeedRow) {
                float const sampled = static_cast<float>(
                    keyframe::sampleSpeedTrackAt(project->speedTrack, static_cast<long double>(playheadTick))
                );
                EditorAction action{EditorActionType::AddSpeedKeyframe};
                action.tick       = playheadTick;
                action.speedValue = sampled;
                ctx.submitAction(std::move(action));
                queueSpeedKeyframeSelection(playheadTick);
                ctx.commands.showSelectionInspector();
            } else if (isFreezeRow) {
                addFreezeKeyframeAt(ctx, playheadTick);
            } else if (isTimelapseRow) {
                addTimelapseKeyframeAt(ctx, playheadTick);
            } else if (isTimeOfDayRow) {
                addTimeOfDayKeyframeAt(ctx, playheadTick);
            } else if (isWeatherRow) {
                addWeatherKeyframeAt(ctx, playheadTick);
            } else if (isAudioRow) {
                promptAndAddAudioClip(ctx, row.id, playheadTick);
            }
        } else if (allowInput && muteHovered && !row.locked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            EditorAction action{EditorActionType::SetAudioTrackMuted};
            action.id    = row.id;
            action.value = !row.muted;
            ctx.submitAction(std::move(action));
        } else if (allowInput && eyeHovered && !(isAudioRow && row.locked)
                   && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (isSpeedRow) {
                EditorAction action{EditorActionType::SetSpeedTrackEnabled};
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else if (isFreezeRow) {
                EditorAction action{EditorActionType::SetFreezeTrackEnabled};
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else if (isTimelapseRow) {
                EditorAction action{EditorActionType::SetTimelapseTrackEnabled};
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else if (isTimeOfDayRow) {
                EditorAction action{EditorActionType::SetTimeOfDayTrackEnabled};
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else if (isWeatherRow) {
                EditorAction action{EditorActionType::SetWeatherTrackEnabled};
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else if (isAudioRow) {
                EditorAction action{EditorActionType::SetAudioTrackEnabled};
                action.id    = row.id;
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            } else {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = row.cameraId;
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            }
        } else if (allowInput && lockHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (isSpeedRow) {
                EditorAction action{EditorActionType::SetSpeedTrackLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else if (isFreezeRow) {
                EditorAction action{EditorActionType::SetFreezeTrackLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else if (isTimelapseRow) {
                EditorAction action{EditorActionType::SetTimelapseTrackLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else if (isTimeOfDayRow) {
                EditorAction action{EditorActionType::SetTimeOfDayTrackLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else if (isWeatherRow) {
                EditorAction action{EditorActionType::SetWeatherTrackLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else if (isAudioRow) {
                EditorAction action{EditorActionType::SetAudioTrackLocked};
                action.id    = row.id;
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            } else {
                EditorAction action{EditorActionType::SetCameraLocked};
                action.id    = row.cameraId;
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            }
        }

        float const controlGlyphSize = std::round(metrics::font() * 1.05f);
        if (hasAddControl) {
            ImU32 const addCol = canAddControl ? (addHovered ? theme::kIconHighlight : theme::kIconInactive)
                                               : theme::withAlpha(theme::kTextDim, 0x70);
            drawCompactIconAtCentre(
                drawList,
                ICON_ADD,
                {addX + controlWidth * 0.5f, rowCentre},
                controlGlyphSize,
                addCol
            );
        }
        if (isAudioRow) {
            ImU32 const muteCol =
                row.locked
                    ? (row.muted ? theme::withAlpha(theme::kWarning, 0x90) : theme::withAlpha(theme::kTextDim, 0x55))
                    : (row.muted
                           ? (muteHovered ? theme::kIconHighlight : theme::kWarning)
                           : (muteHovered ? theme::kIconHighlight : theme::withAlpha(theme::kIconInactive, 0x70)));
            drawCompactIconAtCentre(
                drawList,
                ICON_MUTE,
                {muteX + controlWidth * 0.5f, rowCentre},
                controlGlyphSize,
                muteCol
            );
        }
        char const* eyeIcon = row.enabled ? ICON_EYE : ICON_HIDE;
        ImU32 const eyeCol  = (isAudioRow && row.locked) ? theme::withAlpha(theme::kTextDim, 0x70)
                                                         : (eyeHovered ? theme::kIconHighlight : theme::kIconInactive);
        drawCompactIconAtCentre(drawList, eyeIcon, {eyeX + controlWidth * 0.5f, rowCentre}, controlGlyphSize, eyeCol);
        if (showLock) {
            ImU32 const lockCol =
                row.locked ? theme::kWarning
                           : (lockHovered ? theme::kIconHighlight : theme::withAlpha(theme::kIconInactive, 0x99));
            drawCompactIconAtCentre(
                drawList,
                ICON_LOCK,
                {lockX + controlWidth * 0.5f, rowCentre},
                controlGlyphSize,
                lockCol
            );
        }
        if (isSpeedRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectSpeedTrack();
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectSpeedTrack();
                mSpeedLaneMenuTick = -1;
                speedMenuMin       = ImGui::GetItemRectMin();
                speedMenuMax       = ImGui::GetItemRectMax();
                openSpeedMenu      = true;
            }
        } else if (isFreezeRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectFreezeTrack();
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectFreezeTrack();
                mFreezeLaneMenuTick = -1;
                freezeMenuMin       = ImGui::GetItemRectMin();
                freezeMenuMax       = ImGui::GetItemRectMax();
                openFreezeMenu      = true;
            }
        } else if (isTimelapseRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectTimelapseTrack();
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectTimelapseTrack();
                mTimelapseLaneMenuTick = -1;
                timelapseMenuMin       = ImGui::GetItemRectMin();
                timelapseMenuMax       = ImGui::GetItemRectMax();
                openTimelapseMenu      = true;
            }
        } else if (isTimeOfDayRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectTimeOfDayTrack();
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectTimeOfDayTrack();
                mTimeOfDayLaneMenuTick = -1;
                timeOfDayMenuMin       = ImGui::GetItemRectMin();
                timeOfDayMenuMax       = ImGui::GetItemRectMax();
                openTimeOfDayMenu      = true;
            }
        } else if (isWeatherRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectWeatherTrack();
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectWeatherTrack();
                mWeatherLaneMenuTick = -1;
                weatherMenuMin       = ImGui::GetItemRectMin();
                weatherMenuMax       = ImGui::GetItemRectMax();
                openWeatherMenu      = true;
            }
        } else if (isAudioRow) {
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.selectAudioTrack(row.id);
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.selectAudioTrack(row.id);
                mAudioTrackMenuId  = row.id;
                mAudioLaneMenuTick = -1;
                audioMenuMin       = ImGui::GetItemRectMin();
                audioMenuMax       = ImGui::GetItemRectMax();
                openAudioMenu      = true;
            }
        } else {
            // Reordering changes which camera wins a tick, so it is only offered on the unfiltered list.
            if (allowInput && !mTrackDragCancelled && rowActive && mTrackDragCameraId.empty() && mTrackSearch.empty()
                && project && project->cameras.size() > 1 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f * scale)
                && ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x < controlsLeft) {
                mTrackDragCameraId = row.cameraId;
            }
            if (rowStationary && !eyeHovered && !lockHovered && mTrackDragCameraId.empty() && project
                && project->cameras.size() > 1) {
                widgets::hoverTooltip("playback.refactorEditor.timeline.priorityHint"_tr().c_str());
            }
            if (clicked && mouse.x < controlsLeft) {
                ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                if (ctx.commands.isAutoPreviewEnabled()) {
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = row.cameraId;
                    ctx.submitAction(std::move(action));
                }
            } else if (rowRightClicked && mouse.x < controlsLeft && mTrackDragCameraId.empty()) {
                ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                mTrackMenuCameraId = row.cameraId;
                trackMenuMin       = ImGui::GetItemRectMin();
                trackMenuMax       = ImGui::GetItemRectMax();
                openTrackMenu      = true;
            }
        }
        rowY = rowBottom;
    }
    if (!mTrackDragCameraId.empty()) {
        // The slot is the boundary before camera row `slot`; the camera lands where that boundary is.
        auto const& rows          = mTrackTree.rows();
        float const mouseY        = ImGui::GetMousePos().y;
        float       boundary      = layout.camerasTop - mScrollY;
        float       lineY         = boundary;
        int         slot          = 0;
        int         cameraCounter = 0;
        bool        initialized   = false;
        for (auto const& row : rows) {
            if (row.kind != TrackRowKind::Camera) {
                boundary += row.height;
                continue;
            }
            if (!initialized) {
                lineY       = boundary;
                initialized = true;
            }
            if (mouseY > boundary + row.height * 0.5f) {
                slot  = cameraCounter + 1;
                lineY = boundary + row.height;
            }
            ++cameraCounter;
            boundary += row.height;
        }
        float rowTop = layout.camerasTop - mScrollY;
        for (auto const& row : rows) {
            if (row.kind == TrackRowKind::Camera && row.cameraId == mTrackDragCameraId) {
                drawList->AddRectFilled(
                    {listLeft, rowTop},
                    {listLeft + listW, rowTop + row.height},
                    theme::withAlpha(theme::kAccent, 0x33)
                );
            }
            rowTop += row.height;
        }
        drawList->AddLine(
            {listLeft + metrics::trackSwatch(), lineY},
            {listLeft + listW, lineY},
            theme::kAccent,
            3.0f * metrics::scale()
        );
        if (!allowInput || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mTrackDragCameraId.clear();
            mTrackDragCancelled = true;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (project) {
                auto const found =
                    std::ranges::find(project->cameras, mTrackDragCameraId, &state::editing::model::CameraEntity::id);
                if (found != project->cameras.end()) {
                    int const from = static_cast<int>(found - project->cameras.begin());
                    int const to   = slot <= from ? slot : slot - 1;
                    if (to != from) {
                        EditorAction action{EditorActionType::MoveCamera};
                        action.id   = mTrackDragCameraId;
                        action.kind = to;
                        ctx.submitAction(std::move(action));
                    }
                }
            }
            mTrackDragCameraId.clear();
        }
    }
    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (openTrackMenu) {
        widgets::openAnchoredPopup("##timeline-track-menu", trackMenuMin, trackMenuMax);
    }
    if (openSpeedMenu) {
        widgets::openAnchoredPopup("##timeline-speed-menu", speedMenuMin, speedMenuMax);
    }
    if (openFreezeMenu) {
        widgets::openAnchoredPopup("##timeline-freeze-menu", freezeMenuMin, freezeMenuMax);
    }
    if (openTimelapseMenu) {
        widgets::openAnchoredPopup("##timeline-timelapse-menu", timelapseMenuMin, timelapseMenuMax);
    }
    if (openTimeOfDayMenu) {
        widgets::openAnchoredPopup("##timeline-time-of-day-menu", timeOfDayMenuMin, timeOfDayMenuMax);
    }
    if (openWeatherMenu) {
        widgets::openAnchoredPopup("##weather-lane-menu", weatherMenuMin, weatherMenuMax);
    }
    if (openAudioMenu) {
        widgets::openAnchoredPopup("##timeline-audio-menu", audioMenuMin, audioMenuMax);
    }

    drawList->AddLine({listLeft + listW, layout.rulerTop}, {listLeft + listW, layout.bodyBottom}, theme::kLine);
}

void TimelinePanel::drawRuler(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    int                  displayTick
) {
    auto const&   state         = ctx.state;
    auto*         drawList      = ImGui::GetWindowDrawList();
    float const   uiScale       = metrics::scale();
    float const   labelFontSize = std::max(10.5f, std::round(metrics::font() * 0.76f));
    ImFont* const font          = ImGui::GetFont();
    drawList->AddRectFilled(
        {layout.canvasLeft, layout.rulerTop},
        {layout.fullMax.x, layout.bodyTop},
        theme::kTimelineRuler
    );
    drawList->AddLine({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyTop}, theme::kLine);

    auto const playheadBadge = computePlayheadBadge(
        layout.canvasLeft,
        layout.fullMax.x,
        layout.rulerTop,
        layout.headerHeight,
        scale,
        displayTick
    );
    int const   majorStep     = computeMajorTickStep(scale.pixelsPerTick, labelFontSize, state.totalTicks);
    int const   minorStep     = std::max(1, majorStep / 5);
    int const   firstTick     = scale.firstVisibleTick(minorStep);
    float const rulerBaseline = layout.bodyTop - 1.0f;
    float       lastLabelEnd  = layout.canvasLeft - 6.0f * uiScale;
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        bool const  major      = tick % majorStep == 0;
        float const tickHeight = (major ? 6.5f : 3.5f) * uiScale;
        drawList->AddLine(
            {x, rulerBaseline - tickHeight},
            {x, rulerBaseline},
            major ? theme::kRulerMajor : theme::kRulerMinor
        );
        if (!major) continue;
        std::string const label      = formatTickCompact(tick);
        float const       labelWidth = font->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, label.c_str()).x;
        float const       labelX     = x + 3.0f * uiScale;
        float const       labelY           = layout.rulerTop + 2.0f * uiScale;
        float const       clearPad         = 3.0f * uiScale;
        bool const        overlapsPlayhead = labelX + labelWidth > playheadBadge.badgeX - clearPad
                                   && labelX < playheadBadge.badgeX + playheadBadge.badgeW + clearPad;
        // Skip instead of clamping into place; clamping stacks labels on top of each other at the edges.
        if (!overlapsPlayhead && labelX >= lastLabelEnd + 6.0f * uiScale
            && labelX + labelWidth <= layout.fullMax.x - 4.0f * uiScale) {
            drawList->AddText(font, labelFontSize, {labelX, labelY}, theme::kTextDim, label.c_str());
            lastLabelEnd = labelX + labelWidth;
        }
    }
}

void TimelinePanel::drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const& state         = ctx.state;
    auto*       drawList      = ImGui::GetWindowDrawList();
    float const labelFontSize = std::max(10.5f, std::round(metrics::font() * 0.76f));
    int const   majorStep     = computeMajorTickStep(scale.pixelsPerTick, labelFontSize, state.totalTicks);
    for (int tick = scale.firstVisibleTick(majorStep); tick <= state.totalTicks; tick += majorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        drawList->AddLine({x, layout.bodyTop}, {x, layout.bodyBottom}, theme::kTrackGrid);
    }
}

void TimelinePanel::drawExportRange(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    bool                 allowInput
) {
    int const total = ctx.state.totalTicks;
    if (total <= 0) return;
    // Unset means "whole replay", so nothing is drawn until the user marks a point.
    int const storedIn  = ctx.commands.exportStartTick();
    int const storedOut = ctx.commands.exportEndTick();
    int const inTick    = mExportMarkerDrag != 0 ? mExportDragInTick : storedIn;
    int const outTick   = mExportMarkerDrag != 0 ? mExportDragOutTick : storedOut;
    if (inTick < 0 || outTick < 0) return;
    if (mExportMarkerDrag == 0 && inTick <= 0 && outTick >= total) return;

    auto*       drawList = ImGui::GetWindowDrawList();
    float const left     = scale.xAt(static_cast<float>(std::clamp(inTick, 0, total)));
    float const right    = scale.xAt(static_cast<float>(std::clamp(outTick, 0, total)));
    float const top      = layout.rulerTop;
    float const bottom   = layout.bodyTop;
    drawList->PushClipRect({layout.canvasLeft, top}, {layout.fullMax.x, bottom}, true);

    // The band lives in the ruler strip, bracketed at each end; the bracket is the grab handle.
    drawList->AddRectFilled({left, top}, {right, bottom}, theme::kExportBand);
    ImVec2 const mouse  = ImGui::GetMousePos();
    bool const   inBand = allowInput && mouse.y >= top && mouse.y <= bottom;
    float const  stem   = std::max(1.0f, 1.5f * metrics::scale());
    float const  jaw    = std::max(4.0f, metrics::gutter() * 0.9f);
    auto const   marker = [&](float x, bool isIn, int dragMode) {
        // Grab area stays wide even though the bracket is thin.
        bool const  hovered = inBand && std::abs(mouse.x - x) <= jaw + 3.0f;
        ImU32 const color = (mExportMarkerDrag == dragMode || hovered) ? theme::kIconHighlight : theme::kExportBracket;
        float const inner = isIn ? x + stem : x - stem;
        float const tip   = isIn ? x + jaw : x - jaw;
        drawList->AddRectFilled({std::min(x, inner), top}, {std::max(x, inner), bottom}, color);
        // Jaws turn the stem into a bracket that faces into the range.
        drawList->AddRectFilled({std::min(inner, tip), top}, {std::max(inner, tip), top + stem}, color);
        drawList->AddRectFilled({std::min(inner, tip), bottom - stem}, {std::max(inner, tip), bottom}, color);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            mExportMarkerDrag            = dragMode;
            mExportMarkerDragStartMouseX = mouse.x;
            mExportMarkerDragMoved       = false;
            mExportDragInTick            = inTick;
            mExportDragOutTick           = outTick;
        }
        return hovered;
    };
    bool const overIn  = marker(left, true, 1);
    bool const overOut = marker(right, false, 2);
    if (overIn || overOut || mExportMarkerDrag != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    drawList->PopClipRect();

    if (mExportMarkerDrag == 0) return;
    if (!allowInput || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int const  mode        = mExportMarkerDrag;
        bool const moved       = mExportMarkerDragMoved;
        int const  finalIn     = mExportDragInTick;
        int const  finalOut    = mExportDragOutTick;
        mExportMarkerDrag      = 0;
        mExportMarkerDragMoved = false;
        mExportDragInTick      = -1;
        mExportDragOutTick     = -1;
        if (allowInput && !moved) {
            seekTo(ctx, mode == 1 ? finalIn : finalOut);
        } else if (moved) {
            if (finalIn <= 0 && finalOut >= total) {
                ctx.commands.clearExportRange();
            } else if (finalIn < finalOut && (finalIn != storedIn || finalOut != storedOut)) {
                ctx.commands.setExportRange(finalIn, finalOut);
            }
        }
        return;
    }
    if (!mExportMarkerDragMoved && std::abs(mouse.x - mExportMarkerDragStartMouseX) >= 3.0f) {
        mExportMarkerDragMoved = true;
    }
    if (!mExportMarkerDragMoved) return;
    // Shift snaps to keyframes, markers and the playhead, matching the modifier used elsewhere on the timeline.
    int dragged = std::clamp(scale.tickAt(mouse.x), 0, total);
    if (ImGui::GetIO().KeyShift) {
        dragged = snapToEditPoints(ctx, dragged, scale.pixelsPerTick, true, false, {}, {});
    }
    if (mExportMarkerDrag == 1) mExportDragInTick = std::min(dragged, std::max(0, mExportDragOutTick - 1));
    else mExportDragOutTick = std::max(dragged, std::min(total, mExportDragInTick + 1));
}

void TimelinePanel::drawContextMenus(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    using CameraInterpolationType = state::editing::model::CameraInterpolationType;

    if (widgets::beginAnchoredPopup("##timeline-keyframe-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mKeyframeMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        auto const key = [&]() -> state::editing::model::CameraKeyframe const* {
            if (!camera) return nullptr;
            auto const found = camera->keysByTick.find(mKeyframeMenuTick);
            return found == camera->keysByTick.end() ? nullptr : &found->second;
        }();
        if (!camera || !key) {
            ImGui::CloseCurrentPopup();
        } else {
            // Right-clicking a key inside a multi-selection edits the whole set.
            bool const group    = mKeyframeMenuGroup && ctx.selection.keyframeCount() > 1;
            bool const editable = group || !camera->locked;
            if (group) {
                ImGui::TextDisabled(
                    "%s",
                    "playback.refactorEditor.details.keyframesSelected"_tr(ctx.selection.keyframeCount()).c_str()
                );
                ImGui::Separator();
            }
            using CameraKeyframeMode = state::editing::model::CameraKeyframeMode;
            auto const setLensMode = [&](char const* label, CameraKeyframeMode mode) {
                if (ImGui::MenuItem(label, nullptr, !group && key->mode == mode, editable)) {
                    EditorAction action{group ? EditorActionType::SetKeyframesMode : EditorActionType::SetKeyframeMode};
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    action.kind = static_cast<int>(mode);
                    if (group) action.keyframeRefs = selectedKeyframeRefs(ctx);
                    ctx.submitAction(std::move(action));
                }
            };
            if (ImGui::BeginMenu("playback.refactorEditor.details.lensMode"_tr().c_str(), editable)) {
                setLensMode(
                    "playback.refactorEditor.details.lensModeType.interpolated"_tr().c_str(),
                    CameraKeyframeMode::Interpolated
                );
                setLensMode(
                    "playback.refactorEditor.details.lensModeType.firstPerson"_tr().c_str(),
                    CameraKeyframeMode::FirstPerson
                );
                setLensMode(
                    "playback.refactorEditor.details.lensModeType.thirdPerson"_tr().c_str(),
                    CameraKeyframeMode::ThirdPerson
                );
                ImGui::EndMenu();
            }
            auto const setInterpolation = [&](char const* label, CameraInterpolationType type) {
                if (ImGui::MenuItem(label, nullptr, !group && key->interpolationType == type, editable)) {
                    EditorAction action{
                        group ? EditorActionType::SetKeyframesInterpolation : EditorActionType::SetKeyframeInterpolation
                    };
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    action.kind = static_cast<int>(type);
                    if (group) action.keyframeRefs = selectedKeyframeRefs(ctx);
                    ctx.submitAction(std::move(action));
                }
            };
            bool const keyIsEvaluator =
                key->mode == CameraKeyframeMode::Interpolated
                || (key->mode == CameraKeyframeMode::ThirdPerson && key->thirdPersonCustom);
            bool const interpEnabled = editable && (group || keyIsEvaluator);
            if (ImGui::BeginMenu("playback.refactorEditor.details.interpolation"_tr().c_str(), interpEnabled)) {
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.smooth"_tr().c_str(),
                    CameraInterpolationType::Smooth
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.linear"_tr().c_str(),
                    CameraInterpolationType::Linear
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hold"_tr().c_str(),
                    CameraInterpolationType::Hold
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeIn"_tr().c_str(),
                    CameraInterpolationType::EaseIn
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeOut"_tr().c_str(),
                    CameraInterpolationType::EaseOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeInOut"_tr().c_str(),
                    CameraInterpolationType::EaseInOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hermite"_tr().c_str(),
                    CameraInterpolationType::Hermite
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.cubicBezier"_tr().c_str(),
                    CameraInterpolationType::CubicBezier
                );
                ImGui::EndMenu();
            }
            {
                bool allIgnore = !group && key->ignoreClipSpeed;
                if (group) {
                    auto const& selectedKeys = ctx.selection.keyframes();
                    allIgnore = !selectedKeys.empty() && std::ranges::all_of(selectedKeys, [&](auto const& ref) {
                        auto const cam =
                            std::ranges::find(project->cameras, ref.trackId, &state::editing::model::CameraEntity::id);
                        if (cam == project->cameras.end()) return false;
                        auto const it = cam->keysByTick.find(ref.tick);
                        return it != cam->keysByTick.end() && it->second.ignoreClipSpeed;
                    });
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.ignoreClipSpeed"_tr().c_str(),
                        nullptr,
                        allIgnore,
                        editable && (group || keyIsEvaluator)
                    )) {
                    EditorAction action{
                        group ? EditorActionType::SetKeyframesIgnoreClipSpeed
                              : EditorActionType::SetKeyframeIgnoreClipSpeed
                    };
                    action.id    = camera->id;
                    action.tick  = mKeyframeMenuTick;
                    action.value = !allIgnore;
                    if (group) action.keyframeRefs = selectedKeyframeRefs(ctx);
                    ctx.submitAction(std::move(action));
                }
            }
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.timeline.copyKeyframes"_tr(ctx.selection.keyframeCount()).c_str()
                          : "playback.refactorEditor.timeline.copyKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::Copy).c_str()
                )) {
                if (group) copySelection(ctx);
                else mKeyframeClipboard = {ClipboardKey{camera->id, 0, *key}};
            }

            ImGui::SetNextItemWidth(96.0f * metrics::scale());
            ImGui::BeginDisabled(!editable || group);
            ImGui::InputInt("##timeline-keyframe-tick", &mKeyframeMenuTickEdit, 0, 0);
            bool const tickCommitted = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.setTick"_tr().c_str());
            if (editable && tickCommitted) {
                int const next = std::clamp(mKeyframeMenuTickEdit, 0, ctx.state.totalTicks);
                if (next != mKeyframeMenuTick && !camera->keysByTick.contains(next)) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = camera->id;
                    action.tick          = mKeyframeMenuTick;
                    action.secondaryTick = next;
                    ctx.submitAction(std::move(action));
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, next});
                    mKeyframeMenuTick     = next;
                    mKeyframeMenuTickEdit = next;
                } else {
                    mKeyframeMenuTickEdit = mKeyframeMenuTick;
                }
            }

            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mKeyframeMenuTick);
            if (!group
                && ImGui::MenuItem(
                    "playback.refactorEditor.timeline.captureKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    ctx.state.replayVisible && !camera->locked && keyIsEvaluator
                )) {
                EditorAction action{EditorActionType::CaptureKeyframeFromView};
                action.id   = camera->id;
                action.tick = mKeyframeMenuTick;
                ctx.submitAction(std::move(action));
            }
            // Apply: the free camera jumps to this keyframe's pose without touching the track.
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.applyKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    ctx.state.replayVisible && keyIsEvaluator
                )) {
                ctx.selection.clear();
                EditorAction action{EditorActionType::MoveViewToKeyframe};
                action.id   = camera->id;
                action.tick = mKeyframeMenuTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                    nullptr,
                    false,
                    state::editing::model::isCameraRenderable(*camera)
                )) {
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera->id;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.details.deleteKeyframes"_tr(ctx.selection.keyframeCount()).c_str()
                          : "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                if (group) {
                    deleteSelection(ctx);
                } else {
                    EditorAction action{EditorActionType::DeleteCameraKeyframe};
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    auto const drawAddAndPasteKeyframeItems = [&](state::editing::model::CameraEntity const& camera, int tick) {
        bool const editable = !camera.locked;
        bool const occupied = camera.keysByTick.contains(tick);
        if (ImGui::MenuItem(
                (occupied ? "playback.refactorEditor.timeline.overwriteKeyframeHere"_tr()
                          : "playback.refactorEditor.timeline.addKeyframeHere"_tr())
                    .c_str(),
                nullptr,
                false,
                editable
            )) {
            EditorAction action{EditorActionType::AddCameraKeyframe};
            action.id   = camera.id;
            action.tick = tick;
            ctx.submitAction(std::move(action));
            queueCameraKeyframeSelection(camera.id, tick);
            ctx.commands.showSelectionInspector();
        }
        if (ImGui::MenuItem(
                mKeyframeClipboard.size() > 1
                    ? "playback.refactorEditor.timeline.pasteKeyframes"_tr(mKeyframeClipboard.size()).c_str()
                    : "playback.refactorEditor.timeline.pasteKeyframe"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::Paste).c_str(),
                false,
                editable && !mKeyframeClipboard.empty()
            )) {
            pasteKeyframes(ctx, tick, camera.id);
        }
    };

    if (widgets::beginAnchoredPopup("##timeline-lane-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mLaneMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        if (!camera) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  total    = ctx.state.totalTicks;
            bool const editable = !camera->locked;
            drawAddAndPasteKeyframeItems(*camera, mLaneMenuTick);
            ImGui::Separator();
            int const inTick       = std::clamp(mLaneMenuTick, 0, std::max(0, total - 1));
            int       nextIn       = total;
            bool      clipOccupied = false;
            for (auto const& c : camera->clips) {
                if (c.inTick <= inTick && inTick < c.outTick) clipOccupied = true;
                if (c.inTick > inTick) nextIn = std::min(nextIn, c.inTick);
            }
            int const  outTick    = std::min(nextIn, inTick + kTicksPerSecond * 5);
            bool const canAddClip = editable && total > 0 && !clipOccupied && inTick < outTick;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.addClipHere"_tr().c_str(),
                    nullptr,
                    false,
                    canAddClip
                )) {
                EditorAction action{EditorActionType::AddClip};
                action.id            = camera->id;
                action.tick          = inTick;
                action.secondaryTick = outTick;
                action.name          = "playback.refactorEditor.defaults.clip"_tr(camera->clips.size() + 1);
                action.speed         = 1.0f;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mLaneMenuTick);
            ImGui::Separator();
            bool const canReset =
                editable && total > 0 && std::ranges::none_of(camera->clips, &state::editing::model::Clip::locked);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.resetTrackClips"_tr().c_str(),
                    nullptr,
                    false,
                    canReset
                )) {
                EditorAction action{EditorActionType::ResetClipsToFullRange};
                action.id   = camera->id;
                action.name = "playback.refactorEditor.defaults.clip"_tr(1);
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-track-menu")) {
        {
            state::editing::model::CameraEntity const* camera = nullptr;
            if (project) {
                auto const found =
                    std::ranges::find(project->cameras, mTrackMenuCameraId, &state::editing::model::CameraEntity::id);
                if (found != project->cameras.end()) camera = &*found;
            }
            if (!camera) {
                ImGui::CloseCurrentPopup();
            } else {
                bool const editable = !camera->locked;
                if (mRenameCameraId != camera->id) {
                    mRenameCameraId = camera->id;
                    mRenameBuffer   = {};
                    camera->name.copy(mRenameBuffer.data(), mRenameBuffer.size() - 1);
                }
                ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameTrack"_tr().c_str());
                ImGui::SetNextItemWidth(160.0f * metrics::scale());
                ImGui::BeginDisabled(!editable);
                bool const renameEntered = ImGui::InputText(
                    "##timeline-track-rename",
                    mRenameBuffer.data(),
                    mRenameBuffer.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                );
                bool const renameCommitted = renameEntered || ImGui::IsItemDeactivatedAfterEdit();
                ImGui::EndDisabled();
                if (editable && renameCommitted) {
                    std::string name = mRenameBuffer.data();
                    auto const  first = name.find_first_not_of(" \t");
                    name = first == std::string::npos ? std::string{} : name.substr(first, name.find_last_not_of(" \t") - first + 1);
                    if (!name.empty() && name != camera->name) {
                        EditorAction action{EditorActionType::RenameCamera};
                        action.id   = camera->id;
                        action.name = std::move(name);
                        ctx.submitAction(std::move(action));
                    }
                    mRenameCameraId.clear();
                    if (renameEntered) ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        (camera->enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                         : "playback.refactorEditor.timeline.enableTrack"_tr())
                            .c_str()
                    )) {
                    EditorAction action{EditorActionType::SetCameraEnabled};
                    action.id    = camera->id;
                    action.value = !camera->enabled;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(
                        (camera->locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                        : "playback.refactorEditor.timeline.lockTrack"_tr())
                            .c_str()
                    )) {
                    EditorAction action{EditorActionType::SetCameraLocked};
                    action.id    = camera->id;
                    action.value = !camera->locked;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                        nullptr,
                        false,
                        state::editing::model::isCameraRenderable(*camera)
                    )) {
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                }
                ImGui::Separator();
                int const  cameraIndex = static_cast<int>(camera - project->cameras.data());
                int const  lastIndex   = static_cast<int>(project->cameras.size()) - 1;
                auto const moveTo      = [&](int target) {
                    EditorAction action{EditorActionType::MoveCamera};
                    action.id   = camera->id;
                    action.kind = target;
                    ctx.submitAction(std::move(action));
                };
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.moveTrackTop"_tr().c_str(),
                        nullptr,
                        false,
                        cameraIndex > 0
                    ))
                    moveTo(0);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.moveTrackUp"_tr().c_str(),
                        nullptr,
                        false,
                        cameraIndex > 0
                    ))
                    moveTo(cameraIndex - 1);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.moveTrackDown"_tr().c_str(),
                        nullptr,
                        false,
                        cameraIndex < lastIndex
                    ))
                    moveTo(cameraIndex + 1);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.moveTrackBottom"_tr().c_str(),
                        nullptr,
                        false,
                        cameraIndex < lastIndex
                    ))
                    moveTo(lastIndex);
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.enableAllClips"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !camera->clips.empty()
                    )) {
                    EditorAction action{EditorActionType::SetAllClipsExportEnabled};
                    action.id    = camera->id;
                    action.value = true;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.disableAllClips"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !camera->clips.empty()
                    )) {
                    EditorAction action{EditorActionType::SetAllClipsExportEnabled};
                    action.id    = camera->id;
                    action.value = false;
                    ctx.submitAction(std::move(action));
                }
                bool const canResetTrackClips =
                    editable && ctx.state.totalTicks > 0
                    && std::ranges::none_of(camera->clips, &state::editing::model::Clip::locked);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.resetTrackClips"_tr().c_str(),
                        nullptr,
                        false,
                        canResetTrackClips
                    )) {
                    EditorAction action{EditorActionType::ResetClipsToFullRange};
                    action.id   = camera->id;
                    action.name = "playback.refactorEditor.defaults.clip"_tr(1);
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.clearKeyframes"_tr(camera->keysByTick.size()).c_str(),
                        nullptr,
                        false,
                        editable && !camera->keysByTick.empty()
                    )) {
                    EditorAction action{EditorActionType::ClearCameraKeyframes};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
                        selected && selected->trackId == camera->id) {
                        ctx.selection.select(state::editing::model::SelectedCamera{camera->id});
                    }
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.deleteTrack"_tr().c_str(),
                        nullptr,
                        false,
                        editable && project->cameras.size() > 1
                    )) {
                    EditorAction action{EditorActionType::DeleteCamera};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-scene-menu")) {
        auto const* scene = project ? state::editing::ClipOps::findScene(*project, mSceneMenuId) : nullptr;
        if (!scene) {
            ImGui::CloseCurrentPopup();
        } else {
            if (mSceneRenameId != scene->id) {
                mSceneRenameId     = scene->id;
                mSceneRenameBuffer = {};
                scene->name.copy(mSceneRenameBuffer.data(), mSceneRenameBuffer.size() - 1);
            }
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameScene"_tr().c_str());
            if (mFocusSceneRename && ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
                mFocusSceneRename = false;
            }
            ImGui::SetNextItemWidth(160.0f * metrics::scale());
            bool const renameEntered = ImGui::InputText(
                "##timeline-scene-rename",
                mSceneRenameBuffer.data(),
                mSceneRenameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
            );
            bool const renameCommitted = renameEntered || ImGui::IsItemDeactivatedAfterEdit();
            if (renameCommitted) {
                std::string name = trimWhitespace(mSceneRenameBuffer.data());
                if (!name.empty() && name != scene->name) {
                    EditorAction action{EditorActionType::RenameScene};
                    action.id   = scene->id;
                    action.name = std::move(name);
                    ctx.submitAction(std::move(action));
                }
                mSceneRenameId.clear();
                if (renameEntered) ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.timeline.duplicateScene"_tr().c_str())) {
                EditorAction action{EditorActionType::DuplicateScene};
                action.id = scene->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
            int const sceneIndex = static_cast<int>(scene - project->scenes.data());
            int const lastScene  = static_cast<int>(project->scenes.size()) - 1;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.moveSceneLeft"_tr().c_str(),
                    nullptr,
                    false,
                    sceneIndex > 0
                )) {
                EditorAction action{EditorActionType::MoveScene};
                action.id   = scene->id;
                action.kind = sceneIndex - 1;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.moveSceneRight"_tr().c_str(),
                    nullptr,
                    false,
                    sceneIndex < lastScene
                )) {
                EditorAction action{EditorActionType::MoveScene};
                action.id   = scene->id;
                action.kind = sceneIndex + 1;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            bool const canExport = ctx.state.capabilities.videoExport
                                && !exporting::isExportActive(ctx.state.exportStatus.state) && ctx.state.totalTicks > 0;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.exportScene"_tr().c_str(),
                    nullptr,
                    false,
                    canExport
                )) {
                ctx.commands.openExportScene(scene->id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.deleteScene"_tr().c_str(),
                    nullptr,
                    false,
                    project->scenes.size() > 1
                )) {
                EditorAction action{EditorActionType::DeleteScene};
                action.id = scene->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-clip-menu")) {
        auto const hit = project ? state::editing::ClipOps::findClipWithCamera(*project, mClipMenuClipId)
                                 : state::editing::ClipOps::ClipWithCamera{};
        if (!project || !hit) {
            ImGui::CloseCurrentPopup();
        } else {
            auto const* camera   = hit.camera;
            auto const* clip     = hit.clip;
            bool const  editable = !camera->locked && !clip->locked;
            if (mClipRenameId != clip->id) {
                mClipRenameId     = clip->id;
                mClipRenameBuffer = {};
                clip->name.copy(mClipRenameBuffer.data(), mClipRenameBuffer.size() - 1);
            }
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameClip"_tr().c_str());
            if (mFocusClipRename && ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
                mFocusClipRename = false;
            }
            ImGui::SetNextItemWidth(160.0f * metrics::scale());
            ImGui::BeginDisabled(!editable);
            bool const renameEntered = ImGui::InputText(
                "##timeline-clip-rename",
                mClipRenameBuffer.data(),
                mClipRenameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
            );
            bool const renameCommitted = renameEntered || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::EndDisabled();
            if (editable && renameCommitted) {
                std::string name = trimWhitespace(mClipRenameBuffer.data());
                if (!name.empty() && name != clip->name) {
                    EditorAction action{EditorActionType::RenameClip};
                    action.id   = clip->id;
                    action.name = std::move(name);
                    ctx.submitAction(std::move(action));
                }
                mClipRenameId.clear();
                if (renameEntered) ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            drawAddAndPasteKeyframeItems(*camera, mClipMenuTick);
            ImGui::Separator();
            int const playheadTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.splitClipHere"_tr().c_str(),
                    nullptr,
                    false,
                    editable && clip->inTick < mClipMenuTick && mClipMenuTick < clip->outTick
                )) {
                EditorAction action{EditorActionType::SplitClip};
                action.id   = clip->id;
                action.tick = mClipMenuTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.splitClip"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::SplitClip).c_str(),
                    false,
                    editable && clip->inTick < playheadTick && playheadTick < clip->outTick
                )) {
                EditorAction action{EditorActionType::SplitClip};
                action.id   = clip->id;
                action.tick = playheadTick;
                ctx.submitAction(std::move(action));
            }
            auto const [minIn, maxOut] = clipBoundsOnCamera(*camera, *clip, ctx.state.totalTicks);
            bool const canTrimIn =
                editable && playheadTick >= minIn && playheadTick < clip->outTick && playheadTick != clip->inTick;
            bool const canTrimOut =
                editable && playheadTick > clip->inTick && playheadTick <= maxOut && playheadTick != clip->outTick;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.trimClipInToPlayhead"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::TrimClipInToPlayhead).c_str(),
                    false,
                    canTrimIn
                )) {
                EditorAction action{EditorActionType::MoveAndTrimClip};
                action.id            = clip->id;
                action.tick          = playheadTick;
                action.secondaryTick = clip->outTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.trimClipOutToPlayhead"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::TrimClipOutToPlayhead).c_str(),
                    false,
                    canTrimOut
                )) {
                EditorAction action{EditorActionType::MoveAndTrimClip};
                action.id            = clip->id;
                action.tick          = clip->inTick;
                action.secondaryTick = playheadTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.toggleClipExport"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::ToggleClipExport).c_str(),
                    clip->exportEnabled,
                    !camera->locked
                )) {
                EditorAction action{EditorActionType::SetClipExportEnabled};
                action.id    = clip->id;
                action.value = !clip->exportEnabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    (clip->locked ? "playback.refactorEditor.timeline.unlockClip"_tr()
                                  : "playback.refactorEditor.timeline.lockClip"_tr())
                        .c_str(),
                    nullptr,
                    clip->locked,
                    !camera->locked
                )) {
                EditorAction action{EditorActionType::SetClipLocked};
                action.id    = clip->id;
                action.value = !clip->locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            bool const canExport = ctx.state.capabilities.videoExport
                                && !exporting::isExportActive(ctx.state.exportStatus.state) && ctx.state.totalTicks > 0;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.exportClip"_tr().c_str(),
                    nullptr,
                    false,
                    canExport
                )) {
                ctx.commands.openExportSingleClip(clip->id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.deleteClip"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::DeleteClip};
                action.id = clip->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-ruler-menu")) {
        int const total = ctx.state.totalTicks;
        if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mRulerMenuTick);
        bool const markerFree = project && std::ranges::none_of(project->markers, [&](auto const& marker) {
                                    return marker.tick == mRulerMenuTick;
                                });
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.addMarkerHere"_tr().c_str(),
                nullptr,
                false,
                markerFree
            )) {
            addMarkerAt(ctx, mRulerMenuTick);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setInPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, 0, std::max(0, total - 1));
            int       out  = ctx.commands.exportEndTick();
            if (out <= tick) out = total;
            if (tick <= 0 && out >= total) {
                ctx.commands.clearExportRange();
            } else {
                ctx.commands.setExportRange(tick, out);
            }
        }
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setOutPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, std::min(1, total), total);
            int       in   = ctx.commands.exportStartTick();
            if (in < 0 || in >= tick) in = 0;
            if (in <= 0 && tick >= total) {
                ctx.commands.clearExportRange();
            } else {
                ctx.commands.setExportRange(in, tick);
            }
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.jumpToInPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::JumpExportIn).c_str(),
                false,
                total > 0
            )) {
            seekTo(
                ctx,
                state::editing::TimelineNavigation::resolveExportPointTick(
                    total,
                    ctx.commands.exportStartTick(),
                    ctx.commands.exportEndTick(),
                    false
                )
            );
        }
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.jumpToOutPoint"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::JumpExportOut).c_str(),
                false,
                total > 0
            )) {
            seekTo(
                ctx,
                state::editing::TimelineNavigation::resolveExportPointTick(
                    total,
                    ctx.commands.exportStartTick(),
                    ctx.commands.exportEndTick(),
                    true
                )
            );
        }
        bool const ranged = ctx.commands.exportStartTick() > 0
                         || (ctx.commands.exportEndTick() >= 0 && ctx.commands.exportEndTick() < total);
        if (ImGui::MenuItem(
                "playback.refactorEditor.timeline.clearExportRange"_tr().c_str(),
                input::KeyMap::displayString(input::EditorKeybind::ClearExportRange).c_str(),
                false,
                ranged
            )) {
            ctx.commands.clearExportRange();
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-marker-menu")) {
        state::editing::model::Marker const* marker = nullptr;
        if (project) {
            auto const found = std::ranges::find(project->markers, mMarkerMenuId, &state::editing::model::Marker::id);
            if (found != project->markers.end()) marker = &*found;
        }
        if (!marker) {
            ImGui::CloseCurrentPopup();
        } else {
            if (mMarkerRenameId != marker->id) {
                mMarkerRenameId     = marker->id;
                mMarkerRenameBuffer = {};
                marker->label.copy(mMarkerRenameBuffer.data(), mMarkerRenameBuffer.size() - 1);
            }
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameMarker"_tr().c_str());
            ImGui::SetNextItemWidth(160.0f * metrics::scale());
            bool const entered = ImGui::InputText(
                "##timeline-marker-rename",
                mMarkerRenameBuffer.data(),
                mMarkerRenameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
            );
            if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
                std::string label = mMarkerRenameBuffer.data();
                if (!label.empty() && label != marker->label) {
                    EditorAction action{EditorActionType::RenameMarker};
                    action.id   = marker->id;
                    action.name = std::move(label);
                    ctx.submitAction(std::move(action));
                }
                mMarkerRenameId.clear();
                if (entered) ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekToMarker"_tr().c_str())) seekTo(ctx, marker->tick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.deleteMarker"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str()
                )) {
                EditorAction action{EditorActionType::DeleteMarker};
                action.id = marker->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-speed-keyframe-menu")) {
        auto const keyIt = project ? project->speedTrack.keysByTick.find(mSpeedKeyframeMenuTick)
                                   : std::map<int, state::editing::model::SpeedKeyframe>::const_iterator{};
        if (!project || keyIt == project->speedTrack.keysByTick.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable              = !project->speedTrack.locked;
            auto const setSpeedInterpolation = [&](char const* label, CameraInterpolationType type) {
                if (ImGui::MenuItem(label, nullptr, keyIt->second.interpolationType == type, editable)) {
                    EditorAction action{EditorActionType::SetSpeedKeyframeInterpolation};
                    action.tick = mSpeedKeyframeMenuTick;
                    action.kind = static_cast<int>(type);
                    ctx.submitAction(std::move(action));
                }
            };
            if (ImGui::BeginMenu("playback.refactorEditor.details.interpolation"_tr().c_str(), editable)) {
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.smooth"_tr().c_str(),
                    CameraInterpolationType::Smooth
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.linear"_tr().c_str(),
                    CameraInterpolationType::Linear
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.hold"_tr().c_str(),
                    CameraInterpolationType::Hold
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeIn"_tr().c_str(),
                    CameraInterpolationType::EaseIn
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeOut"_tr().c_str(),
                    CameraInterpolationType::EaseOut
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeInOut"_tr().c_str(),
                    CameraInterpolationType::EaseInOut
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.hermite"_tr().c_str(),
                    CameraInterpolationType::Hermite
                );
                setSpeedInterpolation(
                    "playback.refactorEditor.details.interpolationType.cubicBezier"_tr().c_str(),
                    CameraInterpolationType::CubicBezier
                );
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, mSpeedKeyframeMenuTick);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.speed.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::DeleteSpeedKeyframe};
                action.tick = mSpeedKeyframeMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-add-track-menu")) {
        std::string const cameraItem =
            std::string(ICON_CAMERA) + "  " + "playback.refactorEditor.timeline.addCameraTrack"_tr();
        if (ImGui::MenuItem(cameraItem.c_str())) {
            ctx.submitAction({EditorActionType::AddFreeCamera});
        }
        bool const        hasSpeedTrack = project && project->speedTrack.isVisible();
        std::string const speedItem =
            std::string(ICON_CHEVRONS_RIGHT) + "  " + "playback.refactorEditor.timeline.addSpeedTrack"_tr();
        if (ImGui::MenuItem(speedItem.c_str(), nullptr, hasSpeedTrack, !hasSpeedTrack)) {
            ctx.submitAction({EditorActionType::AddSpeedTrack});
            queueSpeedTrackSelection();
        }
        bool const        hasFreezeTrack = project && project->freezeTrack.isVisible();
        std::string const freezeItem =
            std::string(ICON_FREEZE) + "  " + "playback.refactorEditor.timeline.addFreezeTrack"_tr();
        if (ImGui::MenuItem(freezeItem.c_str(), nullptr, hasFreezeTrack, !hasFreezeTrack)) {
            ctx.submitAction({EditorActionType::AddFreezeTrack});
            queueFreezeTrackSelection();
        }
        bool const        hasTimelapseTrack = project && project->timelapseTrack.isVisible();
        std::string const timelapseItem =
            std::string(ICON_TIMELAPSE) + "  " + "playback.refactorEditor.timeline.addTimelapseTrack"_tr();
        if (ImGui::MenuItem(timelapseItem.c_str(), nullptr, hasTimelapseTrack, !hasTimelapseTrack)) {
            ctx.submitAction({EditorActionType::AddTimelapseTrack});
            queueTimelapseTrackSelection();
        }
        bool const        hasTimeOfDayTrack = project && project->timeOfDayTrack.isVisible();
        std::string const timeOfDayItem =
            std::string(ICON_CLOCK) + "  " + "playback.refactorEditor.timeOfDay.addTrack"_tr();
        if (ImGui::MenuItem(timeOfDayItem.c_str(), nullptr, hasTimeOfDayTrack, !hasTimeOfDayTrack)) {
            ctx.submitAction({EditorActionType::AddTimeOfDayTrack});
            queueTimeOfDayTrackSelection();
        }
        bool const        hasWeatherTrack = project && project->weatherTrack.isVisible();
        std::string const weatherItem =
            std::string(ICON_WEATHER) + "  " + "playback.refactorEditor.weather.addTrack"_tr();
        if (ImGui::MenuItem(weatherItem.c_str(), nullptr, hasWeatherTrack, !hasWeatherTrack)) {
            ctx.submitAction({EditorActionType::AddWeatherTrack});
            queueWeatherTrackSelection();
        }
        std::string const audioItem =
            std::string(ICON_AUDIO) + "  " + "playback.refactorEditor.timeline.addAudioTrack"_tr();
        if (ImGui::MenuItem(audioItem.c_str())) {
            std::string const newTrackId =
                project ? state::editing::ClipOps::nextAudioTrackId(*project) : std::string{};
            ctx.submitAction({EditorActionType::AddAudioTrack});
            if (!newTrackId.empty()) queueAudioTrackSelection(newTrackId);
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-speed-menu")) {
        if (!project || !project->speedTrack.isVisible()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable         = !project->speedTrack.locked;
            bool const hasKeyAtPlayhead = project->speedTrack.keysByTick.contains(playheadTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.speed.addKeyframeAtPlayhead"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !hasKeyAtPlayhead
                )) {
                float const sampled = static_cast<float>(
                    keyframe::sampleSpeedTrackAt(project->speedTrack, static_cast<long double>(playheadTick))
                );
                EditorAction action{EditorActionType::AddSpeedKeyframe};
                action.tick       = playheadTick;
                action.speedValue = sampled;
                ctx.submitAction(std::move(action));
                queueSpeedKeyframeSelection(playheadTick);
                ctx.commands.showSelectionInspector();
            }
            if (mSpeedLaneMenuTick >= 0 && mSpeedLaneMenuTick != playheadTick) {
                bool const hasKeyAtMenu = project->speedTrack.keysByTick.contains(mSpeedLaneMenuTick);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.speed.addKeyframe"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !hasKeyAtMenu
                    )) {
                    float const sampled = static_cast<float>(
                        keyframe::sampleSpeedTrackAt(project->speedTrack, static_cast<long double>(mSpeedLaneMenuTick))
                    );
                    EditorAction action{EditorActionType::AddSpeedKeyframe};
                    action.tick       = mSpeedLaneMenuTick;
                    action.speedValue = sampled;
                    ctx.submitAction(std::move(action));
                    queueSpeedKeyframeSelection(mSpeedLaneMenuTick);
                    ctx.commands.showSelectionInspector();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((project->speedTrack.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                             : "playback.refactorEditor.timeline.enableTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetSpeedTrackEnabled};
                action.value = !project->speedTrack.enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((project->speedTrack.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                                            : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetSpeedTrackLocked};
                action.value = !project->speedTrack.locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.speed.clearKeyframes"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !project->speedTrack.keysByTick.empty()
                )) {
                EditorAction action{EditorActionType::ClearSpeedKeyframes};
                ctx.submitAction(std::move(action));
                if (ctx.selection.selectedSpeedKeyframe().has_value()) {
                    ctx.selection.selectSpeedTrack();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.speed.removeTrack"_tr().c_str(), nullptr, false, editable)) {
                ctx.submitAction({EditorActionType::RemoveSpeedTrack});
                if (ctx.selection.isSpeedTrackSelected() || ctx.selection.selectedSpeedKeyframe().has_value()) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-freeze-keyframe-menu")) {
        auto const keyIt = project ? project->freezeTrack.keysByTick.find(mFreezeKeyframeMenuTick)
                                   : std::map<int, state::editing::model::FreezeKeyframe>::const_iterator{};
        if (!project || keyIt == project->freezeTrack.keysByTick.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !project->freezeTrack.locked;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.freeze.frozen"_tr().c_str(),
                    nullptr,
                    keyIt->second.frozen,
                    editable
                )) {
                EditorAction action{EditorActionType::SetFreezeKeyframe};
                action.tick  = mFreezeKeyframeMenuTick;
                action.value = !keyIt->second.frozen;
                action.kind  = keyIt->second.frozenDelay;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, mFreezeKeyframeMenuTick);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.freeze.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::DeleteFreezeKeyframe};
                action.tick = mFreezeKeyframeMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-freeze-menu")) {
        if (!project || !project->freezeTrack.isVisible()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable         = !project->freezeTrack.locked;
            bool const hasKeyAtPlayhead = project->freezeTrack.keysByTick.contains(playheadTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.freeze.addKeyframeAtPlayhead"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !hasKeyAtPlayhead
                )) {
                addFreezeKeyframeAt(ctx, playheadTick);
            }
            if (mFreezeLaneMenuTick >= 0 && mFreezeLaneMenuTick != playheadTick) {
                bool const hasKeyAtMenu = project->freezeTrack.keysByTick.contains(mFreezeLaneMenuTick);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.freeze.addKeyframe"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !hasKeyAtMenu
                    )) {
                    addFreezeKeyframeAt(ctx, mFreezeLaneMenuTick);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((project->freezeTrack.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                              : "playback.refactorEditor.timeline.enableTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetFreezeTrackEnabled};
                action.value = !project->freezeTrack.enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((project->freezeTrack.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                                             : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetFreezeTrackLocked};
                action.value = !project->freezeTrack.locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.freeze.clearKeyframes"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !project->freezeTrack.keysByTick.empty()
                )) {
                ctx.submitAction({EditorActionType::ClearFreezeKeyframes});
                if (ctx.selection.selectedFreezeKeyframe().has_value()) {
                    ctx.selection.selectFreezeTrack();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.freeze.removeTrack"_tr().c_str(), nullptr, false, editable)) {
                ctx.submitAction({EditorActionType::RemoveFreezeTrack});
                if (ctx.selection.isFreezeTrackSelected() || ctx.selection.selectedFreezeKeyframe().has_value()) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-timelapse-keyframe-menu")) {
        auto const keyIt = project ? project->timelapseTrack.keysByTick.find(mTimelapseKeyframeMenuTick)
                                   : std::map<int, state::editing::model::TimelapseKeyframe>::const_iterator{};
        if (!project || keyIt == project->timelapseTrack.keysByTick.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !project->timelapseTrack.locked;
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, mTimelapseKeyframeMenuTick);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timelapse.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::DeleteTimelapseKeyframe};
                action.tick = mTimelapseKeyframeMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-timelapse-menu")) {
        if (!project || !project->timelapseTrack.isVisible()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable         = !project->timelapseTrack.locked;
            bool const hasKeyAtPlayhead = project->timelapseTrack.keysByTick.contains(playheadTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timelapse.addKeyframeAtPlayhead"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !hasKeyAtPlayhead
                )) {
                addTimelapseKeyframeAt(ctx, playheadTick);
            }
            if (mTimelapseLaneMenuTick >= 0 && mTimelapseLaneMenuTick != playheadTick) {
                bool const hasKeyAtMenu = project->timelapseTrack.keysByTick.contains(mTimelapseLaneMenuTick);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timelapse.addKeyframe"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !hasKeyAtMenu
                    )) {
                    addTimelapseKeyframeAt(ctx, mTimelapseLaneMenuTick);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((project->timelapseTrack.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                                 : "playback.refactorEditor.timeline.enableTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetTimelapseTrackEnabled};
                action.value = !project->timelapseTrack.enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((project->timelapseTrack.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                                                : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetTimelapseTrackLocked};
                action.value = !project->timelapseTrack.locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timelapse.clearKeyframes"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !project->timelapseTrack.keysByTick.empty()
                )) {
                ctx.submitAction({EditorActionType::ClearTimelapseKeyframes});
                if (ctx.selection.selectedTimelapseKeyframe().has_value()) {
                    ctx.selection.selectTimelapseTrack();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timelapse.removeTrack"_tr().c_str(),
                    nullptr,
                    false,
                    editable
                )) {
                ctx.submitAction({EditorActionType::RemoveTimelapseTrack});
                if (ctx.selection.isTimelapseTrackSelected() || ctx.selection.selectedTimelapseKeyframe().has_value()) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-time-of-day-keyframe-menu")) {
        auto const keyIt = project ? project->timeOfDayTrack.keysByTick.find(mTimeOfDayKeyframeMenuTick)
                                   : std::map<int, state::editing::model::TimeOfDayKeyframe>::const_iterator{};
        if (!project || keyIt == project->timeOfDayTrack.keysByTick.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const                             editable      = !project->timeOfDayTrack.locked;
            std::vector<int> const                 selectedTicks = ctx.selection.selectedTimeOfDayKeyframes();
            bool const                             group = mTimeOfDayKeyframeMenuGroup && selectedTicks.size() > 1;
            std::optional<CameraInterpolationType> sharedInterp;
            bool                                   mixedInterp = false;
            if (group) {
                for (int const tick : selectedTicks) {
                    auto const it = project->timeOfDayTrack.keysByTick.find(tick);
                    if (it == project->timeOfDayTrack.keysByTick.end()) continue;
                    if (!sharedInterp) sharedInterp = it->second.interpolationType;
                    else if (*sharedInterp != it->second.interpolationType) mixedInterp = true;
                }
            } else {
                sharedInterp = keyIt->second.interpolationType;
            }
            auto const setTimeOfDayInterpolation = [&](char const* label, CameraInterpolationType type) {
                bool const checked = !mixedInterp && sharedInterp && *sharedInterp == type;
                if (ImGui::MenuItem(label, nullptr, checked, editable)) {
                    if (group) {
                        EditorAction action{EditorActionType::SetTimeOfDayKeyframesInterpolation};
                        action.ticks = selectedTicks;
                        action.kind  = static_cast<int>(type);
                        ctx.submitAction(std::move(action));
                    } else {
                        EditorAction action{EditorActionType::SetTimeOfDayKeyframeInterpolation};
                        action.tick = mTimeOfDayKeyframeMenuTick;
                        action.kind = static_cast<int>(type);
                        ctx.submitAction(std::move(action));
                    }
                }
            };
            if (ImGui::BeginMenu("playback.refactorEditor.details.interpolation"_tr().c_str(), editable)) {
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.smooth"_tr().c_str(),
                    CameraInterpolationType::Smooth
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.linear"_tr().c_str(),
                    CameraInterpolationType::Linear
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.hold"_tr().c_str(),
                    CameraInterpolationType::Hold
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeIn"_tr().c_str(),
                    CameraInterpolationType::EaseIn
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeOut"_tr().c_str(),
                    CameraInterpolationType::EaseOut
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeInOut"_tr().c_str(),
                    CameraInterpolationType::EaseInOut
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.hermite"_tr().c_str(),
                    CameraInterpolationType::Hermite
                );
                setTimeOfDayInterpolation(
                    "playback.refactorEditor.details.interpolationType.cubicBezier"_tr().c_str(),
                    CameraInterpolationType::CubicBezier
                );
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, mTimeOfDayKeyframeMenuTick);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.details.deleteKeyframes"_tr(selectedTicks.size()).c_str()
                          : "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                if (group) {
                    deleteSelection(ctx);
                } else {
                    EditorAction action{EditorActionType::RemoveTimeOfDayKeyframe};
                    action.tick = mTimeOfDayKeyframeMenuTick;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-time-of-day-menu")) {
        if (!project || !project->timeOfDayTrack.isVisible()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable         = !project->timeOfDayTrack.locked;
            bool const hasKeyAtPlayhead = project->timeOfDayTrack.keysByTick.contains(playheadTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeOfDay.addKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !hasKeyAtPlayhead
                )) {
                addTimeOfDayKeyframeAt(ctx, playheadTick);
            }
            if (mTimeOfDayLaneMenuTick >= 0 && mTimeOfDayLaneMenuTick != playheadTick) {
                bool const hasKeyAtMenu = project->timeOfDayTrack.keysByTick.contains(mTimeOfDayLaneMenuTick);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.addKeyframeHere"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !hasKeyAtMenu
                    )) {
                    addTimeOfDayKeyframeAt(ctx, mTimeOfDayLaneMenuTick);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((project->timeOfDayTrack.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                                 : "playback.refactorEditor.timeline.enableTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetTimeOfDayTrackEnabled};
                action.value = !project->timeOfDayTrack.enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((project->timeOfDayTrack.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                                                : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetTimeOfDayTrackLocked};
                action.value = !project->timeOfDayTrack.locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeOfDay.clearKeyframes"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !project->timeOfDayTrack.keysByTick.empty()
                )) {
                ctx.submitAction({EditorActionType::ClearTimeOfDayKeyframes});
                if (!ctx.selection.selectedTimeOfDayKeyframes().empty()) {
                    ctx.selection.selectTimeOfDayTrack();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeOfDay.removeTrack"_tr().c_str(),
                    nullptr,
                    false,
                    editable
                )) {
                ctx.submitAction({EditorActionType::RemoveTimeOfDayTrack});
                if (ctx.selection.isTimeOfDayTrackSelected() || !ctx.selection.selectedTimeOfDayKeyframes().empty()) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##weather-keyframe-menu")) {
        auto const keyIt = project ? project->weatherTrack.keysByTick.find(mWeatherKeyframeMenuTick)
                                   : std::map<int, state::editing::model::WeatherKeyframe>::const_iterator{};
        if (!project || keyIt == project->weatherTrack.keysByTick.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const             editable      = !project->weatherTrack.locked;
            std::vector<int> const selectedTicks = ctx.selection.selectedWeatherKeyframes();
            bool const             group         = mWeatherKeyframeMenuGroup && selectedTicks.size() > 1;
            using Weather                        = state::editing::model::WeatherOverride;
            std::optional<Weather> sharedMode;
            bool                   mixedMode = false;
            if (group) {
                for (int const tick : selectedTicks) {
                    auto const it = project->weatherTrack.keysByTick.find(tick);
                    if (it == project->weatherTrack.keysByTick.end()) continue;
                    if (!sharedMode) sharedMode = it->second.weather;
                    else if (*sharedMode != it->second.weather) mixedMode = true;
                }
            } else {
                sharedMode = keyIt->second.weather;
            }
            constexpr std::array<Weather, 6> kWeatherModes{
                Weather::Recorded,
                Weather::Clear,
                Weather::Overcast,
                Weather::Rain,
                Weather::Snow,
                Weather::Thunder,
            };
            for (Weather const mode : kWeatherModes) {
                bool const        checked = !mixedMode && sharedMode && *sharedMode == mode;
                std::string const label   = std::string(weatherModeIcon(mode)) + "  " + weatherModeLabel(mode);
                if (ImGui::MenuItem(label.c_str(), nullptr, checked, editable)) {
                    if (group) {
                        EditorAction action{EditorActionType::SetWeatherKeyframesValue};
                        action.ticks = selectedTicks;
                        action.kind  = static_cast<int>(mode);
                        ctx.submitAction(std::move(action));
                    } else {
                        EditorAction action{EditorActionType::SetWeatherKeyframeValue};
                        action.tick = mWeatherKeyframeMenuTick;
                        action.kind = static_cast<int>(mode);
                        ctx.submitAction(std::move(action));
                    }
                }
            }
            ImGui::Separator();
            if (!group && ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, mWeatherKeyframeMenuTick);
            }
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.details.deleteKeyframes"_tr(selectedTicks.size()).c_str()
                          : "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                if (group) {
                    deleteSelection(ctx);
                } else {
                    EditorAction action{EditorActionType::RemoveWeatherKeyframe};
                    action.tick = mWeatherKeyframeMenuTick;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##weather-lane-menu")) {
        if (!project || !project->weatherTrack.isVisible()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable         = !project->weatherTrack.locked;
            bool const hasKeyAtPlayhead = project->weatherTrack.keysByTick.contains(playheadTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.weather.addKeyframeAtPlayhead"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !hasKeyAtPlayhead
                )) {
                addWeatherKeyframeAt(ctx, playheadTick);
            }
            if (mWeatherLaneMenuTick >= 0 && mWeatherLaneMenuTick != playheadTick) {
                bool const hasKeyAtMenu = project->weatherTrack.keysByTick.contains(mWeatherLaneMenuTick);
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.addKeyframeHere"_tr().c_str(),
                        nullptr,
                        false,
                        editable && !hasKeyAtMenu
                    )) {
                    addWeatherKeyframeAt(ctx, mWeatherLaneMenuTick);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem((project->weatherTrack.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                               : "playback.refactorEditor.timeline.enableTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetWeatherTrackEnabled};
                action.value = !project->weatherTrack.enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((project->weatherTrack.locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                                              : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetWeatherTrackLocked};
                action.value = !project->weatherTrack.locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.weather.removeTrack"_tr().c_str(), nullptr, false, editable)) {
                ctx.submitAction({EditorActionType::RemoveWeatherTrack});
                if (ctx.selection.isWeatherTrackSelected() || !ctx.selection.selectedWeatherKeyframes().empty()) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-audio-clip-menu")) {
        auto const* track =
            project ? state::editing::ClipOps::findAudioTrack(*project, mAudioClipMenuTrackId) : nullptr;
        auto const* clip = track ? state::editing::ClipOps::findAudioClip(*track, mAudioClipMenuClipId) : nullptr;
        if (!project || !track || !clip) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !track->locked;
            if (ImGui::MenuItem(
                    (clip->muted ? "playback.refactorEditor.audio.unmuteClip"_tr()
                                 : "playback.refactorEditor.audio.muteClip"_tr())
                        .c_str(),
                    nullptr,
                    clip->muted,
                    editable
                )) {
                EditorAction action{EditorActionType::SetAudioClipProps};
                action.id               = track->id;
                action.secondaryId      = clip->id;
                action.trimStartSeconds = clip->trimStartSeconds;
                action.durationSeconds  = clip->durationSeconds;
                action.volume           = clip->volume;
                action.muted            = !clip->muted;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) {
                seekTo(ctx, clip->startTick);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.audio.deleteClip"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::DeleteSelection).c_str(),
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::RemoveAudioClip};
                action.id          = track->id;
                action.secondaryId = clip->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-audio-menu")) {
        auto const* track = project ? state::editing::ClipOps::findAudioTrack(*project, mAudioTrackMenuId) : nullptr;
        if (!project || !track) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playheadTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
            bool const editable     = !track->locked;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.audio.addAudioAtPlayhead"_tr().c_str(),
                    nullptr,
                    false,
                    editable
                )) {
                promptAndAddAudioClip(ctx, track->id, playheadTick);
            }
            if (mAudioLaneMenuTick >= 0 && mAudioLaneMenuTick != playheadTick) {
                if (ImGui::MenuItem(
                        "playback.refactorEditor.audio.addAudioAtCursor"_tr().c_str(),
                        nullptr,
                        false,
                        editable
                    )) {
                    promptAndAddAudioClip(ctx, track->id, mAudioLaneMenuTick);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    (track->muted ? "playback.refactorEditor.audio.unmuteTrack"_tr()
                                  : "playback.refactorEditor.audio.muteTrack"_tr())
                        .c_str(),
                    nullptr,
                    track->muted,
                    editable
                )) {
                EditorAction action{EditorActionType::SetAudioTrackMuted};
                action.id    = track->id;
                action.value = !track->muted;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    (track->enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                    : "playback.refactorEditor.timeline.enableTrack"_tr())
                        .c_str(),
                    nullptr,
                    false,
                    editable
                )) {
                EditorAction action{EditorActionType::SetAudioTrackEnabled};
                action.id    = track->id;
                action.value = !track->enabled;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem((track->locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                               : "playback.refactorEditor.timeline.lockTrack"_tr())
                                    .c_str())) {
                EditorAction action{EditorActionType::SetAudioTrackLocked};
                action.id    = track->id;
                action.value = !track->locked;
                ctx.submitAction(std::move(action));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.audio.removeTrack"_tr().c_str(), nullptr, false, editable)) {
                EditorAction action{EditorActionType::RemoveAudioTrack};
                action.id = track->id;
                ctx.submitAction(std::move(action));
                if (ctx.selection.selectedAudioTrackId() == track->id
                    || (ctx.selection.selectedAudioClip() && ctx.selection.selectedAudioClip()->trackId == track->id)) {
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }
}

void TimelinePanel::drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const& state     = ctx.state;
    auto*       drawList  = ImGui::GetWindowDrawList();
    float const barTop    = layout.bodyBottom;
    float const barLeft   = layout.canvasLeft;
    float const barWidth  = layout.canvasWidth;
    float const barHeight = layout.fullMax.y - barTop;
    if (barHeight <= 4.0f) return;

    drawList->AddRectFilled({layout.fullMin.x, barTop}, {layout.fullMax.x, layout.fullMax.y}, theme::kBgHeader);
    drawList->AddLine({layout.fullMin.x, barTop}, {layout.fullMax.x, barTop}, theme::kLine);
    drawList->AddLine(
        {layout.listLeft + layout.listWidth, barTop},
        {layout.listLeft + layout.listWidth, layout.fullMax.y},
        theme::kLine
    );

    float const uiScale = metrics::scale();
    float const centreY = (barTop + 1.0f + layout.fullMax.y) * 0.5f;

    {
        float const   hintFontSize = std::max(10.5f, std::round(metrics::font() * 0.74f));
        ImFont* const font         = ImGui::GetFont();
        float const   stripLeft    = layout.fullMin.x + metrics::gutter();
        float const   stripRight   = layout.listLeft + layout.listWidth - metrics::gutter();
        if (stripRight > stripLeft + 24.0f * uiScale) {
            char zoomLabel[16]{};
            std::snprintf(zoomLabel, sizeof(zoomLabel), "%d%%", static_cast<int>(std::round(mZoomScale * 100.0f)));
            ImVec2 const zoomSize    = font->CalcTextSizeA(hintFontSize, FLT_MAX, 0.0f, zoomLabel);
            float const  zoomX       = std::max(stripLeft, stripRight - zoomSize.x);
            float const  textY       = std::floor(centreY - zoomSize.y * 0.5f + 0.5f);
            bool         zoomHovered = false;
            if (mZoomScale > 1.01f) {
                ImGui::SetCursorScreenPos({zoomX - 3.0f * uiScale, barTop + 1.0f});
                ImGui::InvisibleButton("##timeline-zoom-pct", {zoomSize.x + 6.0f * uiScale, barHeight - 1.0f});
                zoomHovered = allowInput && ImGui::IsItemHovered();
                if (zoomHovered) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    widgets::hoverTooltip("playback.refactorEditor.timeline.zoomReset"_tr().c_str());
                }
                if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    resetZoom();
                }
            }
            ImU32 const zoomCol =
                zoomHovered ? theme::kIconHighlight : (mZoomScale > 1.01f ? theme::kAccent : theme::kTextDim);
            drawList->AddText(font, hintFontSize, {zoomX, textY}, zoomCol, zoomLabel);

            float const hintAvail = zoomX - metrics::gutter() - stripLeft;
            if (hintAvail > 20.0f * uiScale) {
                std::string const toolHint =
                    mTool == TimelineTool::BoxSelect
                        ? "playback.refactorEditor.timeline.toolHintBoxSelect"_tr()
                        : (mTool == TimelineTool::Razor
                               ? "playback.refactorEditor.timeline.toolHintRazor"_tr()
                               : (mTool == TimelineTool::Pen ? "playback.refactorEditor.timeline.toolHintPen"_tr()
                                                             : "playback.refactorEditor.timeline.toolHintSelect"_tr()));
                std::string const elided = elideTextToWidth(font, hintFontSize, toolHint, hintAvail);
                ImVec4 const      hintClip{stripLeft, barTop, zoomX - metrics::gutter(), layout.fullMax.y};
                drawList->AddText(
                    font,
                    hintFontSize,
                    {stripLeft, textY},
                    theme::kTextDim,
                    elided.c_str(),
                    nullptr,
                    0.0f,
                    &hintClip
                );
            }
        }
    }

    if (barWidth <= 8.0f) return;

    // The thumb is the visible window over the whole replay: its end handles scale, the body pans.
    float const windowSpan  = std::clamp(1.0f / std::max(kMinZoomScale, mZoomScale), 0.02f, 1.0f);
    float const maxScroll   = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const windowStart = maxScroll > 0.0f ? (mScrollX / maxScroll) * (1.0f - windowSpan) : 0.0f;
    // Thin bar whose ends carry ring handles centred on the window edges.
    float const radius    = std::max(4.0f, std::min(barHeight, metrics::rangeBar()) * 0.26f);
    float const thumbH    = std::max(3.0f, radius * 1.1f);
    float const top       = centreY - thumbH * 0.5f;
    float const bottom    = centreY + thumbH * 0.5f;
    float const rounding  = thumbH * 0.5f;
    float const ring      = std::max(1.5f, 1.5f * uiScale);
    float const handleW   = radius + 2.0f * uiScale;
    float const minThumb  = handleW * 2.0f + 6.0f * uiScale;
    float const railLeft  = barLeft + radius + ring;
    float const railWidth = barWidth - (radius + ring) * 2.0f;
    if (railWidth <= minThumb) {
        mRangeDragMode = 0;
        return;
    }

    // Deep zoom still draws the thumb wide enough to hold both handles.
    float winLeft  = railLeft + windowStart * railWidth;
    float winRight = winLeft + windowSpan * railWidth;
    if (winRight - winLeft < minThumb) {
        float const centre =
            std::clamp((winLeft + winRight) * 0.5f, railLeft + minThumb * 0.5f, railLeft + railWidth - minThumb * 0.5f);
        winLeft  = centre - minThumb * 0.5f;
        winRight = centre + minThumb * 0.5f;
    }

    ImGui::SetCursorScreenPos({barLeft, barTop + 1.0f});
    ImGui::InvisibleButton("##timeline-range", {barWidth, barHeight - 1.0f});
    ImVec2 const mouse      = ImGui::GetMousePos();
    bool const   barHovered = ImGui::IsItemHovered();
    bool const   nearLeft   = barHovered && std::abs(mouse.x - winLeft) <= handleW;
    bool const   nearRight  = barHovered && !nearLeft && std::abs(mouse.x - winRight) <= handleW;
    bool const   overBody   = barHovered && !nearLeft && !nearRight && mouse.x > winLeft && mouse.x < winRight;
    bool const   idle       = mRangeDragMode == 0;
    float const  leftAmount =
        mAnimator.animate("range-bar", "left", mRangeDragMode == 1 || (idle && nearLeft) ? 1.0f : 0.0f);
    float const rightAmount =
        mAnimator.animate("range-bar", "right", mRangeDragMode == 2 || (idle && nearRight) ? 1.0f : 0.0f);
    // Half way is hover, full is dragging, so each state gets its own colour.
    float const bodyAmount =
        mAnimator.animate("range-bar", "body", mRangeDragMode == 3 ? 1.0f : (idle && overBody ? 0.5f : 0.0f));

    drawList->AddRectFilled({railLeft, top}, {railLeft + railWidth, bottom}, theme::kRangeTrack, rounding);
    ImU32 const thumbColor = bodyAmount <= 0.5f
                               ? lerpColor(theme::kRangeThumb, theme::kRangeThumbHover, bodyAmount * 2.0f)
                               : lerpColor(theme::kRangeThumbHover, theme::kRangeThumbActive, bodyAmount * 2.0f - 1.0f);
    drawList->AddRectFilled({winLeft, top}, {winRight, bottom}, thumbColor, rounding);

    // Hollow rings: the panel colour fills the centre so the bar appears to pass behind them.
    auto const handle = [&](float centreX, float amount) {
        float const r = radius * (1.0f + 0.15f * amount);
        drawList->AddCircleFilled({centreX, centreY}, r, theme::kBgHeader);
        drawList->AddCircle(
            {centreX, centreY},
            r - ring * 0.5f,
            lerpColor(theme::kRangeHandle, theme::kIconHighlight, amount),
            0,
            ring
        );
    };
    handle(winLeft, leftAmount);
    handle(winRight, rightAmount);

    if (!allowInput) {
        mRangeDragMode = 0;
        return;
    }
    if (barHovered || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGui::IsItemActive() ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (ImGui::IsItemActivated()) {
        if (nearLeft) mRangeDragMode = 1;
        else if (nearRight) mRangeDragMode = 2;
        else if (mouse.x > winLeft && mouse.x < winRight) mRangeDragMode = 3;
        else mRangeDragMode = 4;
        mRangeDragOriginX      = mouse.x;
        mRangeDragOriginScroll = mScrollX;
        mRangeDragOriginZoom   = mZoomScale;
    }
    if (ImGui::IsItemDeactivated()) {
        if (mRangeDragMode == 4) resetZoom();
        mRangeDragMode = 0;
    }
    if (!ImGui::IsItemActive() || mRangeDragMode == 0) return;

    float const spanBefore      = std::clamp(1.0f / std::max(kMinZoomScale, mRangeDragOriginZoom), 0.02f, 1.0f);
    float const maxScrollBefore = maxScrollFor(layout.canvasWidth, mRangeDragOriginZoom, state.totalTicks);
    float const startBefore =
        maxScrollBefore > 0.0f ? (mRangeDragOriginScroll / maxScrollBefore) * (1.0f - spanBefore) : 0.0f;
    float const delta = (mouse.x - mRangeDragOriginX) / railWidth;

    float newStart = startBefore;
    float newSpan  = spanBefore;
    if (mRangeDragMode == 1) {
        newStart = std::clamp(startBefore + delta, 0.0f, startBefore + spanBefore - 0.02f);
        newSpan  = startBefore + spanBefore - newStart;
    } else if (mRangeDragMode == 2) {
        newSpan = std::clamp(spanBefore + delta, 0.02f, 1.0f - startBefore);
    } else if (mRangeDragMode == 3) {
        newStart = std::clamp(startBefore + delta, 0.0f, 1.0f - spanBefore);
    } else {
        return;
    }

    mZoomScale               = std::clamp(1.0f / std::max(0.02f, newSpan), kMinZoomScale, kMaxZoomScale);
    float const clampedSpan  = std::clamp(1.0f / mZoomScale, 0.02f, 1.0f);
    float const maxScrollNow = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const startSpan    = std::max(0.0001f, 1.0f - clampedSpan);
    mScrollX                 = std::clamp(newStart / startSpan * maxScrollNow, 0.0f, maxScrollNow);
}

void TimelinePanel::draw(PanelContext const& ctx, bool allowInput) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) {
        widgets::noActiveProjectPlaceholder();
        return;
    }

    if (!allowInput) {
        mRulerDragTick    = -1;
        mDraggingPlayhead = false;
        mDraggingKeyframeCameraId.clear();
        mDraggingKeyframeStartTick       = -1;
        mDraggingKeyframeMoved           = false;
        mDraggingSpeedKeyframeOriginTick = -1;
        mDraggingSpeedKeyframeTick       = -1;
        mDraggingSpeedKeyframeMoved      = false;
        mDraggingFreezeKeyframeOriginTick = -1;
        mDraggingFreezeKeyframeTick       = -1;
        mDraggingFreezeKeyframeMoved      = false;
        mDraggingTimelapseKeyframeOriginTick = -1;
        mDraggingTimelapseKeyframeTick       = -1;
        mDraggingTimelapseKeyframeMoved      = false;
        mDraggingTimeOfDayKeyframeOriginTick = -1;
        mDraggingTimeOfDayKeyframeTick       = -1;
        mDraggingTimeOfDayKeyframeMoved      = false;
        mTimeOfDayGroupDrag                  = false;
        mTimeOfDayGroupDragDelta             = 0;
        mDraggingWeatherKeyframeOriginTick   = -1;
        mDraggingWeatherKeyframeTick         = -1;
        mDraggingWeatherKeyframeMoved        = false;
        mWeatherGroupDrag                    = false;
        mWeatherGroupDragDelta               = 0;
        mDraggingAudioTrackId.clear();
        mDraggingAudioClipId.clear();
        mDraggingAudioOriginTick   = -1;
        mDraggingAudioDraftTick    = -1;
        mDraggingAudioMoved        = false;
        mRangeDragMode             = 0;
        mExportMarkerDrag          = 0;
        mExportDragInTick          = -1;
        mExportDragOutTick         = -1;
        mGroupDrag                 = false;
        mGroupDragDelta            = 0;
        mBoxPending                = false;
        mBoxSelecting              = false;
        mBoxClickTimeOfDay         = false;
        mBoxClickWeather           = false;
        mPenPendingAdd             = false;
        mPenPendingTimeOfDay       = false;
        mPenPendingWeather         = false;
        mPenPendingTick            = -1;
        mBoxClickCameraId.clear();
        mBoxClickClipId.clear();
        mBoxClickKeyTick = -1;
        mMarkerDragId.clear();
        mMarkerDragMoved = false;
        mClipDragMode    = 0;
        mClipDragCameraId.clear();
        mClipDragId.clear();
        mClipDragMoved = false;
    }
    if (mLastActiveSceneId != project->activeSceneId) {
        mLastActiveSceneId = project->activeSceneId;
        mScrollY           = 0.0f;
        mDraggingKeyframeCameraId.clear();
        mDraggingKeyframeStartTick       = -1;
        mDraggingKeyframeMoved           = false;
        mDraggingSpeedKeyframeOriginTick = -1;
        mDraggingSpeedKeyframeTick       = -1;
        mDraggingSpeedKeyframeMoved      = false;
        mDraggingFreezeKeyframeOriginTick = -1;
        mDraggingFreezeKeyframeTick       = -1;
        mDraggingFreezeKeyframeMoved      = false;
        mDraggingTimelapseKeyframeOriginTick = -1;
        mDraggingTimelapseKeyframeTick       = -1;
        mDraggingTimelapseKeyframeMoved      = false;
        mDraggingTimeOfDayKeyframeOriginTick = -1;
        mDraggingTimeOfDayKeyframeTick       = -1;
        mDraggingTimeOfDayKeyframeMoved      = false;
        mTimeOfDayGroupDrag                  = false;
        mTimeOfDayGroupDragDelta             = 0;
        mDraggingWeatherKeyframeOriginTick   = -1;
        mDraggingWeatherKeyframeTick         = -1;
        mDraggingWeatherKeyframeMoved        = false;
        mWeatherGroupDrag                    = false;
        mWeatherGroupDragDelta               = 0;
        mDraggingAudioTrackId.clear();
        mDraggingAudioClipId.clear();
        mDraggingAudioOriginTick   = -1;
        mDraggingAudioDraftTick    = -1;
        mDraggingAudioMoved        = false;
        mGroupDrag                 = false;
        mGroupDragDelta            = 0;
        mBoxPending                = false;
        mBoxSelecting              = false;
        mBoxClickTimeOfDay         = false;
        mBoxClickWeather           = false;
        mPenPendingAdd             = false;
        mPenPendingTimeOfDay       = false;
        mPenPendingWeather         = false;
        mPenPendingTick            = -1;
        mBoxClickCameraId.clear();
        mBoxClickClipId.clear();
        mBoxClickKeyTick = -1;
        mTrackDragCameraId.clear();
        mClipDragMode = 0;
        mClipDragCameraId.clear();
        mClipDragId.clear();
        mClipDragMoved     = false;
        mExportMarkerDrag  = 0;
        mExportDragInTick  = -1;
        mExportDragOutTick = -1;
    }
    mSnapGuideTick = -1;
    applyPendingSelection(ctx);

    mAnimator.beginFrame();
    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mRulerDragTick >= 0) displayTick = mRulerDragTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 140.0f) return;

    float const uiScale = metrics::scale();
    Layout      layout;
    layout.fullMin        = fullMin;
    layout.fullMax        = {fullMin.x + available.x, fullMin.y + available.y};
    layout.titleBottom    = fullMin.y + metrics::toolbarRow();
    layout.sceneTabBottom = layout.titleBottom + metrics::sceneTabRow();
    layout.rulerTop       = layout.sceneTabBottom;
    layout.headerHeight   = metrics::ruler();
    layout.bodyTop        = layout.rulerTop + layout.headerHeight;
    layout.camerasTop     = layout.bodyTop;
    layout.bodyBottom     = layout.fullMax.y - metrics::rangeBar();

    float const toolStripW       = metrics::rail();
    layout.toolStripRight        = fullMin.x + toolStripW;
    layout.listLeft              = layout.toolStripRight;
    float const fontSize         = ImGui::GetFontSize();
    float const minimumListWidth = std::max(fontSize * 8.5f, 140.0f * uiScale);
    float const maxListWidth =
        std::max(minimumListWidth, available.x - toolStripW - std::max(fontSize * 12.0f, 180.0f * uiScale));
    layout.listWidth   = std::clamp(available.x * mTrackListWidthRatio, minimumListWidth, maxListWidth);
    layout.canvasLeft  = layout.listLeft + layout.listWidth + metrics::splitter();
    layout.canvasWidth = std::max(1.0f, layout.fullMax.x - layout.canvasLeft);

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, layout.fullMax, theme::kTimelineBg);

    drawTitleRow(ctx, layout, displayTick);
    drawSceneTabs(ctx, layout, allowInput);
    drawToolStrip(ctx, layout, allowInput);

    float const fitPixelsPerTick = layout.canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick    = fitPixelsPerTick * mZoomScale;
    float const maxScroll        = std::max(0.0f, state.totalTicks * pixelsPerTick - layout.canvasWidth);
    mScrollX                     = std::clamp(mScrollX, 0.0f, maxScroll);

    TimelineScale const scale{layout.canvasLeft, layout.canvasWidth, pixelsPerTick, mScrollX, state.totalTicks};

    float trackContentHeight = 0.0f;
    for (auto const& row : mTrackTree.rows()) trackContentHeight += row.height;
    float const visibleTrackHeight = std::max(0.0f, layout.bodyBottom - layout.camerasTop);
    float const maxScrollY         = std::max(0.0f, trackContentHeight - visibleTrackHeight);
    mScrollY                       = std::clamp(mScrollY, 0.0f, maxScrollY);

    // The band is the gap between the track list and the canvas; the child windows on either side would take its hover.
    ImVec2 const listSplitterMin{layout.listLeft + layout.listWidth, layout.rulerTop};
    ImVec2 const listSplitterMax{layout.canvasLeft, layout.bodyBottom};
    ImGui::SetCursorScreenPos(listSplitterMin);
    ImGui::InvisibleButton(
        "##timeline-list-splitter",
        {listSplitterMax.x - listSplitterMin.x, listSplitterMax.y - listSplitterMin.y}
    );
    bool const listSplitterHovered = ImGui::IsItemHovered();
    bool const listSplitterActive  = ImGui::IsItemActive();
    if (listSplitterHovered || listSplitterActive) {
        ImGui::SetMouseCursor(listSplitterActive ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (allowInput && listSplitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float const listRight = ImGui::GetMousePos().x - metrics::splitter() * 0.5f;
        mTrackListWidthRatio  = std::clamp((listRight - layout.listLeft) / available.x, 0.15f, 0.50f);
    }

    drawTrackHeaders(ctx, layout, allowInput);
    if (listSplitterHovered || listSplitterActive) {
        drawList->AddRectFilled(
            listSplitterMin,
            listSplitterMax,
            listSplitterActive ? theme::kAccent : theme::withAlpha(theme::kAccent, 0xa0)
        );
    }

    drawList
        ->AddRectFilled({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, theme::kTimelineBg);
    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    drawRuler(ctx, layout, scale, displayTick);

    ImGui::SetCursorScreenPos({layout.canvasLeft, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {layout.canvasWidth, layout.bodyTop - layout.rulerTop});
    // A marker pin under the cursor takes the press, so markers can be dragged without scrubbing.
    std::string const hoveredMarker =
        ImGui::IsItemHovered() ? markerAt(ctx, layout, scale, ImGui::GetMousePos()) : std::string{};
    if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!hoveredMarker.empty()) {
            auto const marker = std::ranges::find(project->markers, hoveredMarker, &state::editing::model::Marker::id);
            ctx.selection.select(state::editing::model::SelectedMarker{hoveredMarker});
            mMarkerMenuId   = hoveredMarker;
            mMarkerRenameId.clear();
            float const x = scale.xAt(static_cast<float>(marker->tick));
            widgets::openAnchoredPopup("##timeline-marker-menu", {x, layout.rulerTop}, {x, layout.bodyTop});
        } else {
            mRulerMenuTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
            ImVec2 const anchor{scale.xAt(static_cast<float>(mRulerMenuTick)), layout.rulerTop};
            widgets::openAnchoredPopup("##timeline-ruler-menu", anchor, {anchor.x, layout.bodyTop});
        }
    }
    if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left) && !hoveredMarker.empty()) {
        auto const marker = std::ranges::find(project->markers, hoveredMarker, &state::editing::model::Marker::id);
        ctx.selection.select(state::editing::model::SelectedMarker{hoveredMarker});
        mMarkerDragId     = hoveredMarker;
        mMarkerDragStartX = ImGui::GetMousePos().x;
        mMarkerDragTick   = marker->tick;
        mMarkerDragMoved  = false;
    }
    if (!hoveredMarker.empty() || !mMarkerDragId.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (allowInput && mMarkerDragId.empty() && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mRulerDragTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
        if (ImGui::GetIO().KeyShift) {
            mRulerDragTick = snapToEditPoints(ctx, mRulerDragTick, scale.pixelsPerTick, false, true, {}, {});
        }
        displayTick = mRulerDragTick;
    }
    if (allowInput && ImGui::IsItemDeactivated()) {
        if (mRulerDragTick >= 0) submitSeek(ctx, mRulerDragTick);
        mRulerDragTick = -1;
    }
    if (allowInput && !mMarkerDragId.empty()) {
        auto const marker = std::ranges::find(project->markers, mMarkerDragId, &state::editing::model::Marker::id);
        if (marker == project->markers.end()) {
            mMarkerDragId.clear();
        } else {
            if (std::abs(ImGui::GetMousePos().x - mMarkerDragStartX) >= 2.0f) mMarkerDragMoved = true;
            if (mMarkerDragMoved) {
                int tick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
                if (ImGui::GetIO().KeyShift) {
                    tick = snapToEditPoints(ctx, tick, scale.pixelsPerTick, true, true, {}, mMarkerDragId);
                }
                // Never land on another marker's tick; the command would reject it anyway.
                if (std::ranges::any_of(project->markers, [&](auto const& other) {
                        return other.id != mMarkerDragId && other.tick == tick;
                    })) {
                    tick = mMarkerDragTick;
                }
                mMarkerDragTick = tick;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!mMarkerDragMoved) {
                    submitSeek(ctx, marker->tick);
                } else if (mMarkerDragTick != marker->tick) {
                    EditorAction action{EditorActionType::MoveMarker};
                    action.id   = mMarkerDragId;
                    action.tick = mMarkerDragTick;
                    ctx.submitAction(std::move(action));
                }
                mMarkerDragId.clear();
                mMarkerDragMoved = false;
            }
        }
    }

    auto tickFromMouse = [&] { return std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks); };
    auto snapTick      = [&](int tick) {
        tick = std::clamp(tick, 0, state.totalTicks);
        if (!mSnapEnabled) return tick;
        constexpr float GridTicks = static_cast<float>(kTicksPerSecond);
        return std::clamp(
            static_cast<int>(std::round(static_cast<float>(tick) / GridTicks) * GridTicks),
            0,
            state.totalTicks
        );
    };
    auto boundedKeyframeTick = [&](int tick) {
        auto const camera =
            std::ranges::find(project->cameras, mDraggingKeyframeCameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) return tick;
        auto const key = camera->keysByTick.find(mDraggingKeyframeStartTick);
        if (key == camera->keysByTick.end()) return tick;

        int minimum = 0;
        int maximum = state.totalTicks;
        if (key != camera->keysByTick.begin()) minimum = std::min(state.totalTicks, std::prev(key)->first + 1);
        auto const next = std::next(key);
        if (next != camera->keysByTick.end()) maximum = std::max(0, next->first - 1);
        if (minimum > maximum) return mDraggingKeyframeStartTick;
        return std::clamp(tick, minimum, maximum);
    };

    // Box select only engages once the press has travelled a few pixels, so plain clicks still seek.
    if (allowInput && mBoxPending && !mBoxSelecting) {
        ImVec2 const now = ImGui::GetMousePos();
        if (std::abs(now.x - mBoxStart.x) + std::abs(now.y - mBoxStart.y) >= kBoxDragThreshold * metrics::scale()) {
            mBoxSelecting  = true;
            mPenPendingAdd = false;
        }
    }
    ImVec2 const boxMouse = ImGui::GetMousePos();
    ImVec2 const boxMin{std::min(mBoxStart.x, boxMouse.x), std::min(mBoxStart.y, boxMouse.y)};
    ImVec2 const boxMax{std::max(mBoxStart.x, boxMouse.x), std::max(mBoxStart.y, boxMouse.y)};
    auto const   boxContains = [&](float x, float centerY) {
        float const reach = metrics::keyframeRadius();
        return x + reach >= boxMin.x && x - reach <= boxMax.x && centerY + reach >= boxMin.y
            && centerY - reach <= boxMax.y;
    };
    std::vector<state::editing::model::SelectedKeyframe> boxHits;
    std::vector<state::editing::model::SelectedClipCut>  cutBoxHits;
    std::vector<int>                                     timeOfDayBoxHits;
    std::vector<int>                                     weatherBoxHits;

    auto const*  selectedClip  = ctx.selection.getAs<state::editing::model::SelectedClip>();
    float const  keyRadius     = metrics::keyframeRadius();
    bool         clickConsumed = false;
    bool const   shiftSnap     = ImGui::GetIO().KeyShift;
    ImVec2 const mouse         = ImGui::GetMousePos();

    drawList->PushClipRect({layout.canvasLeft, layout.camerasTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    float y         = layout.camerasTop - mScrollY;
    int   bandIndex = 0;
    // Alternating bands continue past the last track so the empty area still reads as timeline lanes.
    {
        float const bandHeight = metrics::cameraRow();
        int         band       = 0;
        for (float bandY = layout.camerasTop - mScrollY; bandY < layout.bodyBottom; bandY += bandHeight, ++band) {
            if (bandY + bandHeight < layout.camerasTop) continue;
            if (band % 2 != 0) {
                drawList->AddRectFilled(
                    {layout.canvasLeft, std::max(bandY, layout.camerasTop)},
                    {layout.fullMax.x, std::min(bandY + bandHeight, layout.bodyBottom)},
                    theme::kTimelineTrackBg
                );
            }
            if (bandY + bandHeight < layout.bodyBottom) {
                drawList->AddLine(
                    {layout.canvasLeft, bandY + bandHeight},
                    {layout.fullMax.x, bandY + bandHeight},
                    IM_COL32(0x22, 0x22, 0x26, 0xff)
                );
            }
        }
    }
    auto const intervalEndForTick = [&](int tick) -> int {
        if (!state.cameraTimeline) return state.totalTicks;
        auto const& transitions = state.cameraTimeline->dimensionTransitionTicks();
        auto const  it          = std::lower_bound(transitions.begin(), transitions.end(), tick);
        return it == transitions.end() ? state.totalTicks : std::min(state.totalTicks, *it);
    };
    std::optional<keyframe::SpeedTimeMapping> audioTimeMapping;
    if (!project->audioTracks.empty()
        && (keyframe::hasActiveSpeedCurve(*project) || keyframe::hasActiveTimelapseTrack(*project))) {
        audioTimeMapping.emplace(*project);
    }
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        if (rowBottom < layout.camerasTop || y > layout.bodyBottom) {
            y = rowBottom;
            ++bandIndex;
            continue;
        }
        float const vPad           = std::max(2.5f, std::round(3.0f * uiScale));
        float const blockTop       = y + vPad;
        float const blockBot       = rowBottom - vPad;
        float const speedTabH      = std::max(10.5f, std::round(11.0f * uiScale));
        float const keyCenterY     = std::round(blockTop + (blockBot - blockTop) * 0.515f);
        float const cornerFontSize = std::max(9.0f, std::round(metrics::font() * 0.58f));
        float const cornerH        = speedTabH;
        float const cornerCenterY  = blockTop + speedTabH * 0.5f;
        float const eyeIconSize    = std::max(9.0f, std::round(metrics::font() * 0.64f));
        float const belowFontSize  = std::max(8.5f, std::round(metrics::font() * 0.54f));

        // Rows carry their own band so real tracks stay distinct from the filler bands underneath.
        drawList->AddRectFilled(
            {layout.canvasLeft, std::max(y, layout.camerasTop)},
            {layout.fullMax.x, std::min(rowBottom, layout.bodyBottom)},
            bandIndex % 2 == 0 ? theme::kTimelineBg : theme::kTimelineTrackBg
        );
        drawList
            ->AddLine({layout.canvasLeft, rowBottom}, {layout.fullMax.x, rowBottom}, IM_COL32(0x22, 0x22, 0x26, 0xff));
        ++bandIndex;

        if (row.kind == TrackRowKind::Speed) {
            auto const& speedTrack   = project->speedTrack;
            bool const  trackEnabled = speedTrack.enabled;
            bool const  trackLocked  = speedTrack.locked;
            float const plotTop      = blockTop + 1.0f * uiScale;
            float const plotBottom   = blockBot - 1.0f * uiScale;
            float const centerY      = speedToCurveY(1.0, plotTop, plotBottom);
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            state::editing::model::SpeedTrack const* activeTrack = &speedTrack;
            state::editing::model::SpeedTrack        dragPreviewTrack;
            if (mDraggingSpeedKeyframeOriginTick >= 0 && mDraggingSpeedKeyframeTick >= 0
                && mDraggingSpeedKeyframeTick != mDraggingSpeedKeyframeOriginTick) {
                if (auto const it = speedTrack.keysByTick.find(mDraggingSpeedKeyframeOriginTick);
                    it != speedTrack.keysByTick.end()) {
                    dragPreviewTrack = speedTrack;
                    auto movedKey    = it->second;
                    dragPreviewTrack.keysByTick.erase(mDraggingSpeedKeyframeOriginTick);
                    dragPreviewTrack.keysByTick.emplace(mDraggingSpeedKeyframeTick, std::move(movedKey));
                    activeTrack = &dragPreviewTrack;
                }
            }

            // 1) Horizontal 1.0x dashed baseline and right-hand 1x label.
            ImU32 const baselineCol = theme::withAlpha(theme::kTextDim, trackEnabled ? 0x60 : 0x38);
            drawDashedHorizontalLine(
                drawList,
                layout.canvasLeft,
                layout.fullMax.x,
                centerY,
                baselineCol,
                4.0f * uiScale,
                4.0f * uiScale,
                1.0f
            );
            {
                float const   labelFontSize = std::max(8.5f, std::round(metrics::font() * 0.58f));
                ImFont* const labelFont     = ImGui::GetFont();
                char const*   oneX          = "1x";
                ImVec2 const  oneXSize      = labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, oneX);
                float const   oneXX         = layout.fullMax.x - oneXSize.x - 5.0f * uiScale;
                float const oneXY = std::clamp(centerY - oneXSize.y - 1.0f * uiScale, plotTop, plotBottom - oneXSize.y);
                drawList->AddText(
                    labelFont,
                    labelFontSize,
                    {oneXX, oneXY},
                    theme::withAlpha(theme::kTextDim, trackEnabled ? 0xb0 : 0x60),
                    oneX
                );
            }

            // 2) Continuous speed curve and translucent fast/slow area fill when keyframes exist.
            if (!activeTrack->keysByTick.empty() && state.totalTicks > 0) {
                float const visX0 = std::max(layout.canvasLeft, scale.xAt(0.0f));
                float const visX1 = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(state.totalTicks)));
                if (visX1 > visX0) {
                    float const stepPx = std::max(2.0f, 3.0f * uiScale);
                    struct CurveSample {
                        float  x;
                        float  y;
                        double speed;
                    };
                    std::vector<CurveSample> samples;
                    samples.reserve(static_cast<size_t>((visX1 - visX0) / stepPx) + 2);
                    auto const appendSample = [&](float sx) {
                        long double const tick = std::clamp(
                            static_cast<long double>(scale.tickAtExact(sx)),
                            0.0L,
                            static_cast<long double>(state.totalTicks)
                        );
                        double const spd = keyframe::sampleSpeedTrackAt(*activeTrack, tick);
                        samples.push_back({sx, speedToCurveY(spd, plotTop, plotBottom), spd});
                    };
                    for (float sx = visX0; sx < visX1; sx += stepPx) {
                        appendSample(sx);
                    }
                    appendSample(visX1);

                    ImU32 const fastBase    = trackEnabled ? theme::kSpeedFast : theme::kTrackDisabled;
                    ImU32 const slowBase    = trackEnabled ? theme::kSpeedSlow : theme::kTrackDisabled;
                    ImU32 const neutralBase = trackEnabled ? theme::kAccent : theme::kTrackDisabled;
                    int const   fillAlpha   = trackEnabled ? 0x36 : 0x18;
                    ImU32 const fastFill    = theme::withAlpha(fastBase, fillAlpha);
                    ImU32 const slowFill    = theme::withAlpha(slowBase, fillAlpha);
                    float const strokeWidth = trackEnabled ? 1.8f : 1.25f;

                    for (size_t i = 1; i < samples.size(); ++i) {
                        auto const& p0 = samples[i - 1];
                        auto const& p1 = samples[i];

                        if ((p0.y - centerY) * (p1.y - centerY) < 0.0f) {
                            float const t  = (centerY - p0.y) / (p1.y - p0.y);
                            float const xc = p0.x + (p1.x - p0.x) * t;
                            drawList->AddTriangleFilled(
                                {p0.x, centerY},
                                {p0.x, p0.y},
                                {xc, centerY},
                                p0.y < centerY ? fastFill : slowFill
                            );
                            drawList->AddTriangleFilled(
                                {xc, centerY},
                                {p1.x, p1.y},
                                {p1.x, centerY},
                                p1.y < centerY ? fastFill : slowFill
                            );
                        } else if (p0.y < centerY - 0.5f || p1.y < centerY - 0.5f) {
                            drawList
                                ->AddQuadFilled({p0.x, centerY}, {p0.x, p0.y}, {p1.x, p1.y}, {p1.x, centerY}, fastFill);
                        } else if (p0.y > centerY + 0.5f || p1.y > centerY + 0.5f) {
                            drawList
                                ->AddQuadFilled({p0.x, centerY}, {p0.x, p0.y}, {p1.x, p1.y}, {p1.x, centerY}, slowFill);
                        }

                        double const midSpd = (p0.speed + p1.speed) * 0.5;
                        ImU32 const  segCol = midSpd > 1.005 ? fastBase : (midSpd < 0.995 ? slowBase : neutralBase);
                        drawList->AddLine({p0.x, p0.y}, {p1.x, p1.y}, segCol, strokeWidth);
                    }
                }
            }

            // 3) Hit-test speed keyframes on this lane.
            auto const         selectedSpeedTick = ctx.selection.selectedSpeedKeyframe();
            std::optional<int> hoveredSpeedKeyTick;
            float              hoveredSpeedKeyX      = 0.0f;
            float              hoveredSpeedKeyY      = centerY;
            float              hoveredSpeedKeyRadius = keyRadius;
            if (mouseInRow && mTool != TimelineTool::Razor) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, key] : speedTrack.keysByTick) {
                    bool const  dragging  = mDraggingSpeedKeyframeOriginTick == keyTick;
                    int const   drawnTick = dragging ? mDraggingSpeedKeyframeTick : keyTick;
                    float const kx        = scale.xAt(static_cast<float>(drawnTick));
                    if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                    float const ky     = speedToCurveY(key.speed, plotTop, plotBottom);
                    bool const  sel    = selectedSpeedTick && *selectedSpeedTick == keyTick;
                    float const radius = sel ? keyRadius + 1.0f : keyRadius;
                    float const dx     = std::abs(mouse.x - kx);
                    float const dy     = std::abs(mouse.y - ky);
                    if (dx <= radius + 4.0f && dy <= radius + 5.0f) {
                        float const dist = std::hypot(dx, dy);
                        if (dist < bestDist) {
                            bestDist              = dist;
                            hoveredSpeedKeyTick   = keyTick;
                            hoveredSpeedKeyX      = kx;
                            hoveredSpeedKeyY      = ky;
                            hoveredSpeedKeyRadius = radius;
                        }
                    }
                }
            }

            // 4) Speed keyframe hover tooltip and mouse interaction.
            bool const overSpeedKey = hoveredSpeedKeyTick.has_value();
            if (overSpeedKey) {
                if (mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (mDraggingSpeedKeyframeOriginTick < 0) {
                    if (auto const keyIt = speedTrack.keysByTick.find(*hoveredSpeedKeyTick);
                        keyIt != speedTrack.keysByTick.end()) {
                        char speedBuf[32]{};
                        std::snprintf(speedBuf, sizeof(speedBuf), "%.2fx", keyIt->second.speed);
                        std::string const tip = formatTick(*hoveredSpeedKeyTick) + " ("
                                              + std::to_string(*hoveredSpeedKeyTick) + "t) · " + speedBuf + " · "
                                              + interpolationTypeLabel(keyIt->second.interpolationType);
                        widgets::hoverTooltip(tip.c_str());
                    }
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed          = true;
                    mSpeedKeyframeMenuTick = *hoveredSpeedKeyTick;
                    ctx.selection.selectSpeedKeyframe(*hoveredSpeedKeyTick);
                    widgets::openAnchoredPopup(
                        "##timeline-speed-keyframe-menu",
                        {hoveredSpeedKeyX - hoveredSpeedKeyRadius, hoveredSpeedKeyY - hoveredSpeedKeyRadius},
                        {hoveredSpeedKeyX + hoveredSpeedKeyRadius, hoveredSpeedKeyY + hoveredSpeedKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredSpeedKeyTick;
                    ctx.selection.selectSpeedKeyframe(keyTick);
                    if (trackLocked) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingSpeedKeyframeOriginTick  = keyTick;
                        mDraggingSpeedKeyframeStartMouseX = mouse.x;
                        mDraggingSpeedKeyframeTick        = keyTick;
                        mDraggingSpeedKeyframeMoved       = false;
                    }
                }
            }

            // 5) Pen preview / Ctrl+Click / double-click / context menu / seek on empty speed lane.
            bool const penMode = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen =
                mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt;
            bool const         altBoxSelect = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
            std::optional<int> penPreviewTick;
            if (mouseInRow && !overSpeedKey && !trackLocked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick = std::clamp(penTick, 0, state.totalTicks);
                if (!speedTrack.keysByTick.contains(penTick)) penPreviewTick = penTick;
            }
            if (penPreviewTick.has_value()) {
                double const previewSpeed =
                    keyframe::sampleSpeedTrackAt(*activeTrack, static_cast<long double>(*penPreviewTick));
                float const penX = scale.xAt(static_cast<float>(*penPreviewTick));
                float const penY = speedToCurveY(previewSpeed, plotTop, plotBottom);
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    penY,
                    keyRadius,
                    theme::withAlpha(theme::kSpeedFast, 0x88),
                    theme::withAlpha(theme::kKeyframeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    EditorAction action{EditorActionType::AddSpeedKeyframe};
                    action.tick       = *penPreviewTick;
                    action.speedValue = static_cast<float>(previewSpeed);
                    ctx.submitAction(std::move(action));
                    queueSpeedKeyframeSelection(*penPreviewTick);
                    ctx.commands.showSelectionInspector();
                }
            } else if (mouseInRow && (!overSpeedKey || mTool == TimelineTool::BoxSelect) && !clickConsumed) {
                if (!overSpeedKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) tick = snapTick(tick);
                    mSpeedLaneMenuTick  = tick;
                    float const anchorX = scale.xAt(static_cast<float>(tick));
                    ctx.selection.selectSpeedTrack();
                    widgets::openAnchoredPopup("##timeline-speed-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (!overSpeedKey && !trackLocked && mTool == TimelineTool::Select
                           && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    int addTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (shiftSnap) {
                        addTick = snapToEditPoints(ctx, addTick, scale.pixelsPerTick, true, true, {}, {});
                    } else if (mSnapEnabled) {
                        addTick = snapTick(addTick);
                    }
                    addTick = std::clamp(addTick, 0, state.totalTicks);
                    if (!speedTrack.keysByTick.contains(addTick)) {
                        clickConsumed       = true;
                        float const sampled = static_cast<float>(
                            keyframe::sampleSpeedTrackAt(speedTrack, static_cast<long double>(addTick))
                        );
                        EditorAction action{EditorActionType::AddSpeedKeyframe};
                        action.tick       = addTick;
                        action.speedValue = sampled;
                        ctx.submitAction(std::move(action));
                        queueSpeedKeyframeSelection(addTick);
                        ctx.commands.showSelectionInspector();
                    }
                } else if (mTool != TimelineTool::BoxSelect && !altBoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.selectSpeedTrack();
                    submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                }
            }

            // 6) Draw speed keyframe diamond nodes on top of the curve.
            for (auto const& [keyTick, key] : speedTrack.keysByTick) {
                bool const  dragging  = mDraggingSpeedKeyframeOriginTick == keyTick;
                int const   drawnTick = dragging ? mDraggingSpeedKeyframeTick : keyTick;
                float const kx        = scale.xAt(static_cast<float>(drawnTick));
                if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                float const ky       = speedToCurveY(key.speed, plotTop, plotBottom);
                bool const  selected = selectedSpeedTick && *selectedSpeedTick == keyTick;
                bool const  hovered  = hoveredSpeedKeyTick && *hoveredSpeedKeyTick == keyTick;
                bool const  lit      = selected || dragging;
                float const radius   = lit ? keyRadius + 1.0f : (hovered ? keyRadius + 0.5f : keyRadius);
                ImU32 const nodeBase =
                    !trackEnabled ? theme::kKeyframeDisabled
                                  : (key.speed > 1.005f ? theme::kSpeedFast
                                                        : (key.speed < 0.995f ? theme::kSpeedSlow : theme::kAccent));
                ImU32 const fill    = lit ? theme::kKeyframeSelected : nodeBase;
                ImU32 const outline = lit ? theme::kIconHighlight : theme::kKeyframeOutline;
                drawKeyframeDiamond(drawList, kx, ky, radius, fill, outline);
            }
        } else if (row.kind == TrackRowKind::Freeze) {
            auto const& freezeTrack  = project->freezeTrack;
            bool const  trackEnabled = freezeTrack.enabled;
            bool const  trackLocked  = freezeTrack.locked;
            float const bandTop      = blockTop + 3.0f * uiScale;
            float const bandBot      = blockBot - 3.0f * uiScale;
            float const bandCenterY  = (bandTop + bandBot) * 0.5f;
            float const bandRounding = 3.0f * uiScale;
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            state::editing::model::FreezeTrack const* activeTrack = &freezeTrack;
            state::editing::model::FreezeTrack        dragPreviewTrack;
            if (mDraggingFreezeKeyframeOriginTick >= 0 && mDraggingFreezeKeyframeTick >= 0
                && mDraggingFreezeKeyframeTick != mDraggingFreezeKeyframeOriginTick) {
                if (auto const it = freezeTrack.keysByTick.find(mDraggingFreezeKeyframeOriginTick);
                    it != freezeTrack.keysByTick.end()) {
                    dragPreviewTrack = freezeTrack;
                    auto movedKey    = it->second;
                    dragPreviewTrack.keysByTick.erase(mDraggingFreezeKeyframeOriginTick);
                    dragPreviewTrack.keysByTick.emplace(mDraggingFreezeKeyframeTick, std::move(movedKey));
                    activeTrack = &dragPreviewTrack;
                }
            }

            // 1) Subtle horizontal guide line and freeze span bands (with ease-in ramp for slowdown delay).
            drawDashedHorizontalLine(
                drawList,
                layout.canvasLeft,
                layout.fullMax.x,
                bandCenterY,
                theme::withAlpha(theme::kTextDim, trackEnabled ? 0x48 : 0x28),
                5.0f * uiScale,
                4.0f * uiScale,
                1.0f
            );

            state::editing::model::FreezeTrack        spanSource;
            state::editing::model::FreezeTrack const* spanTrack = activeTrack;
            if (!spanTrack->enabled) {
                spanSource         = *spanTrack;
                spanSource.enabled = true;
                spanTrack          = &spanSource;
            }
            auto const  spans     = keyframe::collectFreezeSpans(*spanTrack, std::max(1, project->totalTicks));
            ImU32 const spanBase  = trackEnabled ? kFreezeAccent : theme::kTrackDisabled;
            ImU32 const spanFill  = theme::withAlpha(spanBase, trackEnabled ? 0x44 : 0x28);
            ImU32 const spanEdge  = theme::withAlpha(spanBase, trackEnabled ? 0xc0 : 0x70);
            ImU32 const delayTint = trackEnabled ? kFreezeDelayTint : theme::kTrackDisabled;
            ImU32 const delayFill = theme::withAlpha(delayTint, trackEnabled ? 0x58 : 0x30);
            ImU32 const delayLine = theme::withAlpha(delayTint, trackEnabled ? 0xd8 : 0x78);

            for (auto const& span : spans) {
                float const rawX0 = scale.xAt(static_cast<float>(span.startTick));
                float const rawX1 = scale.xAt(static_cast<float>(span.endTick));
                float const x0    = std::max(layout.canvasLeft, rawX0);
                float const x1    = std::min(layout.fullMax.x, rawX1);
                if (x1 <= x0) continue;

                drawList->AddRectFilled({x0, bandTop}, {x1, bandBot}, spanFill, bandRounding);
                if (span.frozenDelay > 0) {
                    int const   delayEndTick = std::min(span.endTick, span.startTick + span.frozenDelay);
                    float const rawDelayX1   = scale.xAt(static_cast<float>(delayEndTick));
                    float const delayX0      = std::max(layout.canvasLeft, rawX0);
                    float const delayX1      = std::min(x1, rawDelayX1);
                    if (delayX1 > delayX0) {
                        float const rampSpan = std::max(1.0f, rawDelayX1 - rawX0);
                        float const t0       = std::clamp((delayX0 - rawX0) / rampSpan, 0.0f, 1.0f);
                        float const t1       = std::clamp((delayX1 - rawX0) / rampSpan, 0.0f, 1.0f);
                        float const y0       = bandBot - (bandBot - bandTop) * t0;
                        float const y1       = bandBot - (bandBot - bandTop) * t1;
                        drawList->AddRectFilled(
                            {delayX0, bandTop},
                            {delayX1, bandBot},
                            theme::withAlpha(delayTint, trackEnabled ? 0x26 : 0x16)
                        );
                        drawList->AddQuadFilled(
                            {delayX0, bandBot},
                            {delayX0, y0},
                            {delayX1, y1},
                            {delayX1, bandBot},
                            delayFill
                        );
                        drawList->AddLine({delayX0, y0}, {delayX1, y1}, delayLine, 1.5f);
                    }
                }
                drawList->AddRect({x0, bandTop}, {x1, bandBot}, spanEdge, bandRounding, 0, 1.0f);
            }

            // 2) Hit-test freeze keyframes on this lane.
            auto const         selectedFreezeTick = ctx.selection.selectedFreezeKeyframe();
            std::optional<int> hoveredFreezeKeyTick;
            float              hoveredFreezeKeyX      = 0.0f;
            float              hoveredFreezeKeyRadius = keyRadius;
            if (mouseInRow && mTool != TimelineTool::Razor) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, key] : freezeTrack.keysByTick) {
                    bool const  dragging  = mDraggingFreezeKeyframeOriginTick == keyTick;
                    int const   drawnTick = dragging ? mDraggingFreezeKeyframeTick : keyTick;
                    float const kx        = scale.xAt(static_cast<float>(drawnTick));
                    if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                    bool const  sel    = selectedFreezeTick && *selectedFreezeTick == keyTick;
                    float const radius = sel ? keyRadius + 1.0f : keyRadius;
                    float const dx     = std::abs(mouse.x - kx);
                    float const dy     = std::abs(mouse.y - bandCenterY);
                    if (dx <= radius + 4.0f && dy <= radius + 5.0f) {
                        float const dist = std::hypot(dx, dy);
                        if (dist < bestDist) {
                            bestDist               = dist;
                            hoveredFreezeKeyTick   = keyTick;
                            hoveredFreezeKeyX      = kx;
                            hoveredFreezeKeyRadius = radius;
                        }
                    }
                }
            }

            // 3) Freeze keyframe hover tooltip and mouse interaction.
            bool const overFreezeKey = hoveredFreezeKeyTick.has_value();
            if (overFreezeKey) {
                if (mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (mDraggingFreezeKeyframeOriginTick < 0) {
                    if (auto const keyIt = freezeTrack.keysByTick.find(*hoveredFreezeKeyTick);
                        keyIt != freezeTrack.keysByTick.end()) {
                        std::string tip = formatTick(*hoveredFreezeKeyTick) + " ("
                                        + std::to_string(*hoveredFreezeKeyTick) + "t) · "
                                        + (keyIt->second.frozen ? "playback.refactorEditor.freeze.frozenState"_tr()
                                                                : "playback.refactorEditor.freeze.unfrozenState"_tr());
                        if (keyIt->second.frozen && keyIt->second.frozenDelay > 0) {
                            tip += " · " + "playback.refactorEditor.freeze.slowdownDelay"_tr() + " "
                                 + std::to_string(keyIt->second.frozenDelay) + "t";
                        }
                        widgets::hoverTooltip(tip.c_str());
                    }
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed           = true;
                    mFreezeKeyframeMenuTick = *hoveredFreezeKeyTick;
                    ctx.selection.selectFreezeKeyframe(*hoveredFreezeKeyTick);
                    widgets::openAnchoredPopup(
                        "##timeline-freeze-keyframe-menu",
                        {hoveredFreezeKeyX - hoveredFreezeKeyRadius, bandCenterY - hoveredFreezeKeyRadius},
                        {hoveredFreezeKeyX + hoveredFreezeKeyRadius, bandCenterY + hoveredFreezeKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredFreezeKeyTick;
                    ctx.selection.selectFreezeKeyframe(keyTick);
                    if (trackLocked) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingFreezeKeyframeOriginTick  = keyTick;
                        mDraggingFreezeKeyframeStartMouseX = mouse.x;
                        mDraggingFreezeKeyframeTick        = keyTick;
                        mDraggingFreezeKeyframeMoved       = false;
                    }
                }
            }

            // 4) Pen preview / Ctrl+Click / double-click / context menu / seek on empty freeze lane.
            bool const penMode = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen =
                mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt;
            bool const         altBoxSelect = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
            std::optional<int> penPreviewTick;
            if (mouseInRow && !overFreezeKey && !trackLocked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick = std::clamp(penTick, 0, state.totalTicks);
                if (!freezeTrack.keysByTick.contains(penTick)) penPreviewTick = penTick;
            }
            if (penPreviewTick.has_value()) {
                bool const  previewFrozen = defaultFreezeStateAt(*activeTrack, *penPreviewTick);
                float const penX          = scale.xAt(static_cast<float>(*penPreviewTick));
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    bandCenterY,
                    keyRadius,
                    theme::withAlpha(previewFrozen ? kFreezeKeyFill : kFreezeUnfreezeFill, 0x88),
                    theme::withAlpha(previewFrozen ? theme::kKeyframeOutline : kFreezeUnfreezeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    addFreezeKeyframeAt(ctx, *penPreviewTick);
                }
            } else if (mouseInRow && (!overFreezeKey || mTool == TimelineTool::BoxSelect) && !clickConsumed) {
                if (!overFreezeKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) tick = snapTick(tick);
                    mFreezeLaneMenuTick = tick;
                    float const anchorX = scale.xAt(static_cast<float>(tick));
                    ctx.selection.selectFreezeTrack();
                    widgets::openAnchoredPopup("##timeline-freeze-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (!overFreezeKey && !trackLocked && mTool == TimelineTool::Select
                           && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    int addTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (shiftSnap) {
                        addTick = snapToEditPoints(ctx, addTick, scale.pixelsPerTick, true, true, {}, {});
                    } else if (mSnapEnabled) {
                        addTick = snapTick(addTick);
                    }
                    addTick = std::clamp(addTick, 0, state.totalTicks);
                    if (!freezeTrack.keysByTick.contains(addTick)) {
                        clickConsumed = true;
                        addFreezeKeyframeAt(ctx, addTick);
                    }
                } else if (mTool != TimelineTool::BoxSelect && !altBoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.selectFreezeTrack();
                    submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                }
            }

            // 5) Draw freeze keyframe diamond nodes.
            for (auto const& [keyTick, key] : freezeTrack.keysByTick) {
                bool const  dragging  = mDraggingFreezeKeyframeOriginTick == keyTick;
                int const   drawnTick = dragging ? mDraggingFreezeKeyframeTick : keyTick;
                float const kx        = scale.xAt(static_cast<float>(drawnTick));
                if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                bool const  selected = selectedFreezeTick && *selectedFreezeTick == keyTick;
                bool const  hovered  = hoveredFreezeKeyTick && *hoveredFreezeKeyTick == keyTick;
                bool const  lit      = selected || dragging;
                float const radius   = lit ? keyRadius + 1.0f : (hovered ? keyRadius + 0.5f : keyRadius);
                ImU32 const fill =
                    !trackEnabled ? theme::kKeyframeDisabled : (key.frozen ? kFreezeKeyFill : kFreezeUnfreezeFill);
                ImU32 const outline =
                    lit ? theme::kKeyframeSelected : (key.frozen ? theme::kKeyframeOutline : kFreezeUnfreezeOutline);
                drawKeyframeDiamond(drawList, kx, bandCenterY, radius, fill, outline);
            }
        } else if (row.kind == TrackRowKind::Timelapse) {
            auto const& timelapseTrack = project->timelapseTrack;
            bool const  trackEnabled   = timelapseTrack.enabled;
            bool const  trackLocked    = timelapseTrack.locked;
            float const bandTop        = blockTop + 3.0f * uiScale;
            float const bandBot        = blockBot - 3.0f * uiScale;
            float const bandCenterY    = (bandTop + bandBot) * 0.5f;
            float const bandRounding   = 3.0f * uiScale;
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            state::editing::model::TimelapseTrack const* activeTrack = &timelapseTrack;
            state::editing::model::TimelapseTrack        dragPreviewTrack;
            if (mDraggingTimelapseKeyframeOriginTick >= 0 && mDraggingTimelapseKeyframeTick >= 0
                && mDraggingTimelapseKeyframeTick != mDraggingTimelapseKeyframeOriginTick) {
                if (auto const it = timelapseTrack.keysByTick.find(mDraggingTimelapseKeyframeOriginTick);
                    it != timelapseTrack.keysByTick.end()) {
                    dragPreviewTrack = timelapseTrack;
                    auto movedKey    = it->second;
                    dragPreviewTrack.keysByTick.erase(mDraggingTimelapseKeyframeOriginTick);
                    dragPreviewTrack.keysByTick.emplace(mDraggingTimelapseKeyframeTick, std::move(movedKey));
                    activeTrack = &dragPreviewTrack;
                }
            }

            // 1) Guide line and mapped timelapse segments with output duration/speed labels.
            drawDashedHorizontalLine(
                drawList,
                layout.canvasLeft,
                layout.fullMax.x,
                bandCenterY,
                theme::withAlpha(theme::kTextDim, trackEnabled ? 0x48 : 0x28),
                5.0f * uiScale,
                4.0f * uiScale,
                1.0f
            );

            auto const    segments = keyframe::collectTimelapseSegments(*activeTrack);
            ImU32 const   segBase  = trackEnabled ? kTimelapseAccent : theme::kTrackDisabled;
            ImU32 const   segFill  = theme::withAlpha(segBase, trackEnabled ? 0x44 : 0x26);
            ImU32 const   segEdge  = theme::withAlpha(segBase, trackEnabled ? 0xc0 : 0x6c);
            ImU32 const   invFill  = trackEnabled ? kTimelapseInvalidFill : theme::withAlpha(kTimelapseInvalidFill, 36);
            ImU32 const   invEdge  = trackEnabled ? kTimelapseInvalidEdge : theme::withAlpha(kTimelapseInvalidEdge, 96);
            float const   labelFontSize = std::max(8.5f, std::round(metrics::font() * 0.58f));
            ImFont* const labelFont     = ImGui::GetFont();

            for (auto const& seg : segments) {
                float const rawX0 = scale.xAt(static_cast<float>(seg.startTick));
                float const rawX1 = scale.xAt(static_cast<float>(seg.endTick));
                float const x0    = std::max(layout.canvasLeft, rawX0);
                float const x1    = std::min(layout.fullMax.x, rawX1);
                if (x1 <= x0) continue;

                float const availW = (x1 - x0) - 8.0f * uiScale;
                if (seg.valid) {
                    drawList->AddRectFilled({x0, bandTop}, {x1, bandBot}, segFill, bandRounding);
                    drawList->AddRect({x0, bandTop}, {x1, bandBot}, segEdge, bandRounding, 0, 1.0f);
                    if (availW > 8.0f * uiScale) {
                        std::string const durStr = formatTick(std::max(0, seg.endOutputTicks - seg.startOutputTicks));
                        char              speedBuf[32]{};
                        std::snprintf(speedBuf, sizeof(speedBuf), "%.1fx", seg.speed);
                        std::string const fullLabel = durStr + " (" + speedBuf + ")";
                        std::string       text;
                        ImVec2 const      fullSize =
                            labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, fullLabel.c_str());
                        if (fullSize.x <= availW) {
                            text = fullLabel;
                        } else {
                            ImVec2 const durSize =
                                labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, durStr.c_str());
                            if (durSize.x <= availW) {
                                text = durStr;
                            }
                        }
                        if (!text.empty()) {
                            ImVec2 const textSize =
                                labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, text.c_str());
                            float const  tx = x0 + ((x1 - x0) - textSize.x) * 0.5f;
                            float const  ty = std::floor(bandCenterY - textSize.y * 0.5f + 0.5f);
                            ImVec4 const textClip{x0 + 2.0f, bandTop, x1 - 2.0f, bandBot};
                            drawList->AddText(
                                labelFont,
                                labelFontSize,
                                {tx, ty},
                                trackEnabled ? theme::kText : theme::kTextDim,
                                text.c_str(),
                                nullptr,
                                0.0f,
                                &textClip
                            );
                        }
                    }
                } else {
                    drawList->AddRectFilled({x0, bandTop}, {x1, bandBot}, invFill, bandRounding);
                    drawList->AddRect({x0, bandTop}, {x1, bandBot}, invEdge, bandRounding, 0, 1.0f);
                    if (availW > 8.0f * uiScale) {
                        std::string const invalidLabel = "playback.refactorEditor.timelapse.invalidSegment"_tr();
                        ImVec2 const      textSize =
                            labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, invalidLabel.c_str());
                        if (textSize.x <= availW) {
                            float const  tx = x0 + ((x1 - x0) - textSize.x) * 0.5f;
                            float const  ty = std::floor(bandCenterY - textSize.y * 0.5f + 0.5f);
                            ImVec4 const textClip{x0 + 2.0f, bandTop, x1 - 2.0f, bandBot};
                            drawList->AddText(
                                labelFont,
                                labelFontSize,
                                {tx, ty},
                                trackEnabled ? kTimelapseInvalidText : theme::kTextDim,
                                invalidLabel.c_str(),
                                nullptr,
                                0.0f,
                                &textClip
                            );
                        }
                    }
                }
            }

            // 2) Hit-test timelapse keyframes on this lane.
            auto const         selectedTimelapseTick = ctx.selection.selectedTimelapseKeyframe();
            std::optional<int> hoveredTimelapseKeyTick;
            float              hoveredTimelapseKeyX      = 0.0f;
            float              hoveredTimelapseKeyRadius = keyRadius;
            if (mouseInRow && mTool != TimelineTool::Razor) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, _] : timelapseTrack.keysByTick) {
                    bool const  dragging  = mDraggingTimelapseKeyframeOriginTick == keyTick;
                    int const   drawnTick = dragging ? mDraggingTimelapseKeyframeTick : keyTick;
                    float const kx        = scale.xAt(static_cast<float>(drawnTick));
                    if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                    bool const  sel    = selectedTimelapseTick && *selectedTimelapseTick == keyTick;
                    float const radius = sel ? keyRadius + 1.0f : keyRadius;
                    float const dx     = std::abs(mouse.x - kx);
                    float const dy     = std::abs(mouse.y - bandCenterY);
                    if (dx <= radius + 4.0f && dy <= radius + 5.0f) {
                        float const dist = std::hypot(dx, dy);
                        if (dist < bestDist) {
                            bestDist                  = dist;
                            hoveredTimelapseKeyTick   = keyTick;
                            hoveredTimelapseKeyX      = kx;
                            hoveredTimelapseKeyRadius = radius;
                        }
                    }
                }
            }

            // 3) Timelapse keyframe hover tooltip and mouse interaction.
            bool const overTimelapseKey = hoveredTimelapseKeyTick.has_value();
            if (overTimelapseKey) {
                if (mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (mDraggingTimelapseKeyframeOriginTick < 0) {
                    if (auto const keyIt = timelapseTrack.keysByTick.find(*hoveredTimelapseKeyTick);
                        keyIt != timelapseTrack.keysByTick.end()) {
                        std::string tip = formatTick(*hoveredTimelapseKeyTick) + " ("
                                        + std::to_string(*hoveredTimelapseKeyTick) + "t) -> "
                                        + "playback.refactorEditor.timelapse.outputTime"_tr() + " "
                                        + formatTick(keyIt->second.outputTicks) + " ("
                                        + std::to_string(keyIt->second.outputTicks) + "t)";
                        if (timelapseTrack.keysByTick.size() == 1) {
                            tip += "\n" + "playback.refactorEditor.timelapse.requiresTwoKeyframes"_tr();
                        }
                        if (keyIt != timelapseTrack.keysByTick.begin()
                            && std::prev(keyIt)->second.outputTicks >= keyIt->second.outputTicks) {
                            tip += "\n" + "playback.refactorEditor.timelapse.invalidOrderHint"_tr();
                        }
                        widgets::hoverTooltip(tip.c_str());
                    }
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed              = true;
                    mTimelapseKeyframeMenuTick = *hoveredTimelapseKeyTick;
                    ctx.selection.selectTimelapseKeyframe(*hoveredTimelapseKeyTick);
                    widgets::openAnchoredPopup(
                        "##timeline-timelapse-keyframe-menu",
                        {hoveredTimelapseKeyX - hoveredTimelapseKeyRadius, bandCenterY - hoveredTimelapseKeyRadius},
                        {hoveredTimelapseKeyX + hoveredTimelapseKeyRadius, bandCenterY + hoveredTimelapseKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredTimelapseKeyTick;
                    ctx.selection.selectTimelapseKeyframe(keyTick);
                    if (trackLocked) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingTimelapseKeyframeOriginTick  = keyTick;
                        mDraggingTimelapseKeyframeStartMouseX = mouse.x;
                        mDraggingTimelapseKeyframeTick        = keyTick;
                        mDraggingTimelapseKeyframeMoved       = false;
                    }
                }
            }

            // 4) Pen preview / Ctrl+Click / double-click / context menu / seek on empty timelapse lane.
            bool const penMode = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen =
                mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt;
            bool const         altBoxSelect = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
            std::optional<int> penPreviewTick;
            if (mouseInRow && !overTimelapseKey && !trackLocked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick = std::clamp(penTick, 0, state.totalTicks);
                if (!timelapseTrack.keysByTick.contains(penTick)) penPreviewTick = penTick;
            }
            if (penPreviewTick.has_value()) {
                float const penX = scale.xAt(static_cast<float>(*penPreviewTick));
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    bandCenterY,
                    keyRadius,
                    theme::withAlpha(kTimelapseKeyFill, 0x88),
                    theme::withAlpha(theme::kKeyframeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    addTimelapseKeyframeAt(ctx, *penPreviewTick);
                }
            } else if (mouseInRow && (!overTimelapseKey || mTool == TimelineTool::BoxSelect) && !clickConsumed) {
                if (!overTimelapseKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) tick = snapTick(tick);
                    mTimelapseLaneMenuTick = tick;
                    float const anchorX    = scale.xAt(static_cast<float>(tick));
                    ctx.selection.selectTimelapseTrack();
                    widgets::openAnchoredPopup("##timeline-timelapse-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (!overTimelapseKey && !trackLocked && mTool == TimelineTool::Select
                           && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    int addTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (shiftSnap) {
                        addTick = snapToEditPoints(ctx, addTick, scale.pixelsPerTick, true, true, {}, {});
                    } else if (mSnapEnabled) {
                        addTick = snapTick(addTick);
                    }
                    addTick = std::clamp(addTick, 0, state.totalTicks);
                    if (!timelapseTrack.keysByTick.contains(addTick)) {
                        clickConsumed = true;
                        addTimelapseKeyframeAt(ctx, addTick);
                    }
                } else if (mTool != TimelineTool::BoxSelect && !altBoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.selectTimelapseTrack();
                    submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                }
            }

            // 5) Draw timelapse keyframe diamond nodes.
            bool const singleKey = activeTrack->keysByTick.size() == 1;
            for (auto const& [keyTick, _] : timelapseTrack.keysByTick) {
                bool const  dragging  = mDraggingTimelapseKeyframeOriginTick == keyTick;
                int const   drawnTick = dragging ? mDraggingTimelapseKeyframeTick : keyTick;
                float const kx        = scale.xAt(static_cast<float>(drawnTick));
                if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                bool const  selected = selectedTimelapseTick && *selectedTimelapseTick == keyTick;
                bool const  hovered  = hoveredTimelapseKeyTick && *hoveredTimelapseKeyTick == keyTick;
                bool const  lit      = selected || dragging;
                float const radius   = lit ? keyRadius + 1.0f : (hovered ? keyRadius + 0.5f : keyRadius);
                auto const  activeIt = activeTrack->keysByTick.find(drawnTick);
                bool const  conflict = activeIt != activeTrack->keysByTick.end()
                                   && activeIt != activeTrack->keysByTick.begin()
                                   && std::prev(activeIt)->second.outputTicks >= activeIt->second.outputTicks;
                bool const  isWarning = singleKey || conflict;
                ImU32 const fill      = !trackEnabled ? theme::kKeyframeDisabled
                                                      : (isWarning ? kTimelapseInvalidKeyFill : kTimelapseKeyFill);
                ImU32 const outline   = lit ? theme::kKeyframeSelected : theme::kKeyframeOutline;
                drawKeyframeDiamond(drawList, kx, bandCenterY, radius, fill, outline);
            }
        } else if (row.kind == TrackRowKind::TimeOfDay) {
            auto const& timeOfDayTrack = project->timeOfDayTrack;
            bool const  trackEnabled   = timeOfDayTrack.enabled;
            bool const  trackLocked    = timeOfDayTrack.locked;
            float const plotTop        = blockTop + 1.0f * uiScale;
            float const plotBottom     = blockBot - 1.0f * uiScale;
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            auto const drawnTickForTodKey = [&](int keyTick, bool selected) {
                if (mTimeOfDayGroupDrag && selected) return keyTick + mTimeOfDayGroupDragDelta;
                if (!mTimeOfDayGroupDrag && mDraggingTimeOfDayKeyframeOriginTick == keyTick) {
                    return mDraggingTimeOfDayKeyframeTick;
                }
                return keyTick;
            };

            state::editing::model::TimeOfDayTrack const* activeTrack = &timeOfDayTrack;
            state::editing::model::TimeOfDayTrack        dragPreviewTrack;
            if (mTimeOfDayGroupDrag && mTimeOfDayGroupDragDelta != 0) {
                dragPreviewTrack = timeOfDayTrack;
                std::vector<std::pair<int, state::editing::model::TimeOfDayKeyframe>> movedKeys;
                for (int const tick : ctx.selection.selectedTimeOfDayKeyframes()) {
                    if (auto node = dragPreviewTrack.keysByTick.extract(tick); !node.empty()) {
                        movedKeys.emplace_back(tick + mTimeOfDayGroupDragDelta, std::move(node.mapped()));
                    }
                }
                for (auto& [tick, key] : movedKeys) {
                    dragPreviewTrack.keysByTick.emplace(tick, std::move(key));
                }
                activeTrack = &dragPreviewTrack;
            } else if (!mTimeOfDayGroupDrag && mDraggingTimeOfDayKeyframeOriginTick >= 0
                       && mDraggingTimeOfDayKeyframeTick >= 0
                       && mDraggingTimeOfDayKeyframeTick != mDraggingTimeOfDayKeyframeOriginTick) {
                if (auto const it = timeOfDayTrack.keysByTick.find(mDraggingTimeOfDayKeyframeOriginTick);
                    it != timeOfDayTrack.keysByTick.end()) {
                    dragPreviewTrack = timeOfDayTrack;
                    auto movedKey    = it->second;
                    dragPreviewTrack.keysByTick.erase(mDraggingTimeOfDayKeyframeOriginTick);
                    dragPreviewTrack.keysByTick.emplace(mDraggingTimeOfDayKeyframeTick, std::move(movedKey));
                    activeTrack = &dragPreviewTrack;
                }
            }

            state::editing::model::TimeOfDayTrack evalTrackStorage;
            auto const*                           evalTrack = activeTrack;
            if (!activeTrack->enabled && !activeTrack->keysByTick.empty()) {
                evalTrackStorage         = *activeTrack;
                evalTrackStorage.enabled = true;
                evalTrack                = &evalTrackStorage;
            }

            double minTod = 0.0;
            double maxTod = static_cast<double>(state::editing::model::DayCycleTicks);
            for (auto const& [_, key] : activeTrack->keysByTick) {
                maxTod = std::max(maxTod, static_cast<double>(key.timeOfDay));
            }

            struct TodCurveSample {
                float  x;
                double value;
            };
            std::vector<TodCurveSample> samples;
            if (!activeTrack->keysByTick.empty() && state.totalTicks > 0) {
                float const visX0 = std::max(layout.canvasLeft, scale.xAt(0.0f));
                float const visX1 = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(state.totalTicks)));
                if (visX1 > visX0) {
                    float const stepPx = std::max(2.0f, 3.0f * uiScale);
                    samples.reserve(static_cast<size_t>((visX1 - visX0) / stepPx) + 2);
                    auto const appendSample = [&](float sx) {
                        double const tick = std::clamp(
                            static_cast<double>(scale.tickAtExact(sx)),
                            0.0,
                            static_cast<double>(state.totalTicks)
                        );
                        if (auto const val = keyframe::evaluateTimeOfDayAt(*evalTrack, tick)) {
                            maxTod = std::max(maxTod, *val);
                            samples.push_back({sx, *val});
                        }
                    };
                    for (float sx = visX0; sx < visX1; sx += stepPx) {
                        appendSample(sx);
                    }
                    appendSample(visX1);
                }
            }

            // 1) Horizontal midpoint guide (18:00 / 12000 ticks on a 0..24000 day cycle).
            float const guideY   = timeOfDayToCurveY(12000.0, minTod, maxTod, plotTop, plotBottom);
            ImU32 const guideCol = theme::withAlpha(theme::kTextDim, trackEnabled ? 0x60 : 0x38);
            drawDashedHorizontalLine(
                drawList,
                layout.canvasLeft,
                layout.fullMax.x,
                guideY,
                guideCol,
                4.0f * uiScale,
                4.0f * uiScale,
                1.0f
            );
            {
                float const   labelFontSize = std::max(8.5f, std::round(metrics::font() * 0.58f));
                ImFont* const labelFont     = ImGui::GetFont();
                char const*   midLabel      = "18:00";
                ImVec2 const  midSize       = labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, midLabel);
                float const   midX          = layout.fullMax.x - midSize.x - 5.0f * uiScale;
                float const   midY = std::clamp(guideY - midSize.y - 1.0f * uiScale, plotTop, plotBottom - midSize.y);
                drawList->AddText(
                    labelFont,
                    labelFontSize,
                    {midX, midY},
                    theme::withAlpha(theme::kTextDim, trackEnabled ? 0xb0 : 0x60),
                    midLabel
                );
            }

            // 2) Continuous interpolated time-of-day curve and translucent warm amber fill.
            if (samples.size() >= 2) {
                ImU32 const curveCol    = trackEnabled ? kTimeOfDayAccent : theme::kTrackDisabled;
                ImU32 const fillCol     = theme::withAlpha(curveCol, trackEnabled ? 0x2c : 0x14);
                float const strokeWidth = trackEnabled ? 1.8f : 1.25f;
                for (size_t i = 1; i < samples.size(); ++i) {
                    float const x0 = samples[i - 1].x;
                    float const y0 = timeOfDayToCurveY(samples[i - 1].value, minTod, maxTod, plotTop, plotBottom);
                    float const x1 = samples[i].x;
                    float const y1 = timeOfDayToCurveY(samples[i].value, minTod, maxTod, plotTop, plotBottom);
                    drawList->AddQuadFilled({x0, plotBottom}, {x0, y0}, {x1, y1}, {x1, plotBottom}, fillCol);
                    drawList->AddLine({x0, y0}, {x1, y1}, curveCol, strokeWidth);
                }
            }

            // 3) Hit-test time-of-day keyframes on this lane.
            std::optional<int> hoveredTodKeyTick;
            float              hoveredTodKeyX        = 0.0f;
            float              hoveredTodKeyY        = (plotTop + plotBottom) * 0.5f;
            float              hoveredTodKeyRadius   = keyRadius;
            bool               hoveredTodKeySelected = false;
            if (mouseInRow && mTool != TimelineTool::Razor) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, key] : timeOfDayTrack.keysByTick) {
                    bool const  selected  = ctx.selection.isTimeOfDayKeyframeSelected(keyTick);
                    int const   drawnTick = drawnTickForTodKey(keyTick, selected);
                    float const kx        = scale.xAt(static_cast<float>(drawnTick));
                    if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                    float const ky =
                        timeOfDayToCurveY(static_cast<double>(key.timeOfDay), minTod, maxTod, plotTop, plotBottom);
                    float const radius = selected ? keyRadius + 1.0f : keyRadius;
                    float const dx     = std::abs(mouse.x - kx);
                    float const dy     = std::abs(mouse.y - ky);
                    if (dx <= radius + 4.0f && dy <= radius + 5.0f) {
                        float const dist = std::hypot(dx, dy);
                        if (dist < bestDist) {
                            bestDist              = dist;
                            hoveredTodKeyTick     = keyTick;
                            hoveredTodKeyX        = kx;
                            hoveredTodKeyY        = ky;
                            hoveredTodKeyRadius   = radius;
                            hoveredTodKeySelected = selected;
                        }
                    }
                }
            }

            // 4) Keyframe hover tooltip and mouse interaction.
            bool const overTodKey = hoveredTodKeyTick.has_value();
            if (overTodKey) {
                if (mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (mDraggingTimeOfDayKeyframeOriginTick < 0) {
                    if (auto const keyIt = timeOfDayTrack.keysByTick.find(*hoveredTodKeyTick);
                        keyIt != timeOfDayTrack.keysByTick.end()) {
                        std::string const tip = formatTick(*hoveredTodKeyTick) + " ("
                                              + std::to_string(*hoveredTodKeyTick) + "t) · "
                                              + std::to_string(keyIt->second.timeOfDay) + " ("
                                              + formatMinecraftClock(keyIt->second.timeOfDay) + ") · "
                                              + interpolationTypeLabel(keyIt->second.interpolationType);
                        widgets::hoverTooltip(tip.c_str());
                    }
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed              = true;
                    int const keyTick          = *hoveredTodKeyTick;
                    mTimeOfDayKeyframeMenuTick = keyTick;
                    mTimeOfDayKeyframeMenuGroup =
                        hoveredTodKeySelected && ctx.selection.selectedTimeOfDayKeyframes().size() > 1;
                    if (!mTimeOfDayKeyframeMenuGroup) {
                        ctx.selection.selectTimeOfDayKeyframe(keyTick);
                    }
                    widgets::openAnchoredPopup(
                        "##timeline-time-of-day-keyframe-menu",
                        {hoveredTodKeyX - hoveredTodKeyRadius, hoveredTodKeyY - hoveredTodKeyRadius},
                        {hoveredTodKeyX + hoveredTodKeyRadius, hoveredTodKeyY + hoveredTodKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredTodKeyTick;
                    if (ImGui::GetIO().KeyCtrl && !trackLocked) {
                        std::vector<int> ticks = ctx.selection.selectedTimeOfDayKeyframes();
                        auto const       it    = std::ranges::find(ticks, keyTick);
                        if (it != ticks.end()) ticks.erase(it);
                        else ticks.push_back(keyTick);
                        if (ticks.empty()) {
                            ctx.selection.clear();
                        } else if (ticks.size() == 1) {
                            ctx.selection.selectTimeOfDayKeyframe(ticks.front());
                        } else {
                            ctx.selection.selectTimeOfDayKeyframes(std::move(ticks));
                        }
                    } else if (hoveredTodKeySelected && ctx.selection.selectedTimeOfDayKeyframes().size() > 1
                               && !trackLocked) {
                        mTimeOfDayGroupDrag                   = true;
                        mTimeOfDayGroupDragDelta              = 0;
                        mDraggingTimeOfDayKeyframeOriginTick  = keyTick;
                        mDraggingTimeOfDayKeyframeStartMouseX = mouse.x;
                        mDraggingTimeOfDayKeyframeTick        = keyTick;
                        mDraggingTimeOfDayKeyframeMoved       = false;
                    } else {
                        ctx.selection.selectTimeOfDayKeyframe(keyTick);
                        if (trackLocked) {
                            submitSeek(ctx, keyTick);
                        } else {
                            mDraggingTimeOfDayKeyframeOriginTick  = keyTick;
                            mDraggingTimeOfDayKeyframeStartMouseX = mouse.x;
                            mDraggingTimeOfDayKeyframeTick        = keyTick;
                            mDraggingTimeOfDayKeyframeMoved       = false;
                        }
                    }
                }
            }

            // 5) Pen preview / Ctrl+Click / Alt+Drag box select / double-click / context menu / seek.
            bool const penMode = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen =
                mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt;
            bool const altBoxSelect       = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
            auto const startTodBoxPending = [&](bool penAdd, int penTick) {
                mBoxPending          = true;
                mBoxSelecting        = false;
                mBoxAdditive         = ImGui::GetIO().KeyCtrl;
                mBoxStart            = mouse;
                mBoxClickTimeOfDay   = true;
                mBoxClickWeather     = false;
                mPenPendingAdd       = penAdd;
                mPenPendingTimeOfDay = penAdd;
                mPenPendingWeather   = false;
                mPenPendingTick      = penTick;
                mBoxClickCameraId.clear();
                mBoxClickClipId.clear();
                mBoxClickKeyTick = -1;
            };

            std::optional<int> penPreviewTick;
            if (mouseInRow && !overTodKey && !trackLocked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick = std::clamp(penTick, 0, state.totalTicks);
                if (!timeOfDayTrack.keysByTick.contains(penTick)) penPreviewTick = penTick;
            }
            if (penPreviewTick.has_value()) {
                std::optional<double> previewVal;
                if (!evalTrack->keysByTick.empty()) {
                    previewVal = keyframe::evaluateTimeOfDayAt(*evalTrack, static_cast<double>(*penPreviewTick));
                } else {
                    previewVal = keyframe::resolveEffectiveTimeOfDay(*project, static_cast<double>(*penPreviewTick));
                }
                double const previewTod = previewVal ? *previewVal : 6000.0;
                float const  penX       = scale.xAt(static_cast<float>(*penPreviewTick));
                float const  penY       = timeOfDayToCurveY(previewTod, minTod, maxTod, plotTop, plotBottom);
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    penY,
                    keyRadius,
                    theme::withAlpha(kTimeOfDayAccent, 0x88),
                    theme::withAlpha(theme::kKeyframeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (penMode) {
                        startTodBoxPending(true, *penPreviewTick);
                    } else {
                        addTimeOfDayKeyframeAt(ctx, *penPreviewTick);
                    }
                }
            } else if (mouseInRow && (!overTodKey || mTool == TimelineTool::BoxSelect) && !clickConsumed) {
                if (!overTodKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) tick = snapTick(tick);
                    mTimeOfDayLaneMenuTick = tick;
                    float const anchorX    = scale.xAt(static_cast<float>(tick));
                    ctx.selection.selectTimeOfDayTrack();
                    widgets::openAnchoredPopup("##timeline-time-of-day-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (!overTodKey && !trackLocked && mTool == TimelineTool::Select
                           && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    int addTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (shiftSnap) {
                        addTick = snapToEditPoints(ctx, addTick, scale.pixelsPerTick, true, true, {}, {});
                    } else if (mSnapEnabled) {
                        addTick = snapTick(addTick);
                    }
                    addTick = std::clamp(addTick, 0, state.totalTicks);
                    if (!timeOfDayTrack.keysByTick.contains(addTick)) {
                        clickConsumed = true;
                        addTimeOfDayKeyframeAt(ctx, addTick);
                    }
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (penMode || altBoxSelect || mTool == TimelineTool::Select || mTool == TimelineTool::BoxSelect) {
                        startTodBoxPending(false, -1);
                    } else {
                        ctx.selection.selectTimeOfDayTrack();
                        submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                    }
                }
            }

            // 6) Draw time-of-day keyframe diamond nodes on top of the curve.
            for (auto const& [keyTick, key] : timeOfDayTrack.keysByTick) {
                bool const selected = ctx.selection.isTimeOfDayKeyframeSelected(keyTick);
                bool const dragging = (mTimeOfDayGroupDrag && selected)
                                   || (!mTimeOfDayGroupDrag && mDraggingTimeOfDayKeyframeOriginTick == keyTick);
                int const   drawnTick = drawnTickForTodKey(keyTick, selected);
                float const kx        = scale.xAt(static_cast<float>(drawnTick));
                if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                float const ky =
                    timeOfDayToCurveY(static_cast<double>(key.timeOfDay), minTod, maxTod, plotTop, plotBottom);
                bool const inBox = mBoxSelecting && !trackLocked && boxContains(kx, ky);
                if (inBox) timeOfDayBoxHits.push_back(keyTick);
                bool const  hovered = hoveredTodKeyTick && *hoveredTodKeyTick == keyTick;
                bool const  lit     = selected || dragging || inBox;
                float const radius  = lit ? keyRadius + 1.0f : (hovered ? keyRadius + 0.5f : keyRadius);
                ImU32 const fill =
                    lit ? theme::kKeyframeSelected : (trackEnabled ? kTimeOfDayKeyFill : theme::kKeyframeDisabled);
                ImU32 const outline = lit ? theme::kIconHighlight : theme::kKeyframeOutline;
                drawKeyframeDiamond(drawList, kx, ky, radius, fill, outline);
            }
        } else if (row.kind == TrackRowKind::Weather) {
            auto const& weatherTrack = project->weatherTrack;
            bool const  trackEnabled = weatherTrack.enabled;
            bool const  trackLocked  = weatherTrack.locked;
            float const bandTop      = blockTop + 3.0f * uiScale;
            float const bandBot      = blockBot - 3.0f * uiScale;
            float const bandCenterY  = (bandTop + bandBot) * 0.5f;
            float const bandRounding = 3.0f * uiScale;
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            auto const drawnTickForWeatherKey = [&](int keyTick, bool selected) {
                if (mWeatherGroupDrag && selected) return keyTick + mWeatherGroupDragDelta;
                if (!mWeatherGroupDrag && mDraggingWeatherKeyframeOriginTick == keyTick) {
                    return mDraggingWeatherKeyframeTick;
                }
                return keyTick;
            };

            state::editing::model::WeatherTrack const* activeTrack = &weatherTrack;
            state::editing::model::WeatherTrack        dragPreviewTrack;
            if (mWeatherGroupDrag && mWeatherGroupDragDelta != 0) {
                dragPreviewTrack = weatherTrack;
                std::vector<std::pair<int, state::editing::model::WeatherKeyframe>> movedKeys;
                for (int const tick : ctx.selection.selectedWeatherKeyframes()) {
                    if (auto node = dragPreviewTrack.keysByTick.extract(tick); !node.empty()) {
                        movedKeys.emplace_back(tick + mWeatherGroupDragDelta, std::move(node.mapped()));
                    }
                }
                for (auto& [tick, key] : movedKeys) {
                    dragPreviewTrack.keysByTick.emplace(tick, std::move(key));
                }
                activeTrack = &dragPreviewTrack;
            } else if (!mWeatherGroupDrag && mDraggingWeatherKeyframeOriginTick >= 0
                       && mDraggingWeatherKeyframeTick >= 0
                       && mDraggingWeatherKeyframeTick != mDraggingWeatherKeyframeOriginTick) {
                if (auto const it = weatherTrack.keysByTick.find(mDraggingWeatherKeyframeOriginTick);
                    it != weatherTrack.keysByTick.end()) {
                    dragPreviewTrack = weatherTrack;
                    auto movedKey    = it->second;
                    dragPreviewTrack.keysByTick.erase(mDraggingWeatherKeyframeOriginTick);
                    dragPreviewTrack.keysByTick.emplace(mDraggingWeatherKeyframeTick, std::move(movedKey));
                    activeTrack = &dragPreviewTrack;
                }
            }

            // 1) Subtle dashed horizontal baseline representing inherited scene/recorded weather before first keyframe.
            drawDashedHorizontalLine(
                drawList,
                layout.canvasLeft,
                layout.fullMax.x,
                bandCenterY,
                theme::withAlpha(theme::kTextDim, trackEnabled ? 0x48 : 0x28),
                5.0f * uiScale,
                4.0f * uiScale,
                1.0f
            );

            // 2) Colored weather segment bands with mode icon and localized label.
            float const   labelFontSize = std::max(8.5f, std::round(metrics::font() * 0.58f));
            ImFont* const labelFont     = ImGui::GetFont();
            float const   iconSize      = std::max(10.0f, std::round(metrics::font() * 0.72f));
            for (auto it = activeTrack->keysByTick.begin(); it != activeTrack->keysByTick.end(); ++it) {
                int const  startTick = it->first;
                auto const next      = std::next(it);
                int const  endTick =
                    next != activeTrack->keysByTick.end() ? next->first : std::max(project->totalTicks, startTick);
                float const rawX0 = scale.xAt(static_cast<float>(startTick));
                float const rawX1 = scale.xAt(static_cast<float>(endTick));
                float const x0    = std::max(layout.canvasLeft, rawX0);
                float const x1    = std::min(layout.fullMax.x, rawX1);
                if (x1 <= x0) continue;

                ImU32 const modeCol  = trackEnabled ? weatherModeColor(it->second.weather) : theme::kTrackDisabled;
                ImU32 const spanFill = theme::withAlpha(modeCol, trackEnabled ? 0x42 : 0x22);
                ImU32 const spanEdge = theme::withAlpha(modeCol, trackEnabled ? 0xb8 : 0x68);
                drawList->AddRectFilled({x0, bandTop}, {x1, bandBot}, spanFill, bandRounding);
                drawList->AddRect({x0, bandTop}, {x1, bandBot}, spanEdge, bandRounding, 0, 1.0f);

                float const contentLeft  = std::max(x0 + 5.0f * uiScale, rawX0 + keyRadius + 5.0f * uiScale);
                float const contentRight = x1 - 4.0f * uiScale;
                if (contentRight - contentLeft >= 36.0f * uiScale) {
                    drawList->PushClipRect({contentLeft, bandTop}, {contentRight, bandBot}, true);
                    ImU32 const iconCol = trackEnabled ? lerpColor(modeCol, theme::kText, 0.35f) : theme::kTextDim;
                    drawCompactIconAtCentre(
                        drawList,
                        weatherModeIcon(it->second.weather),
                        {contentLeft + iconSize * 0.5f, bandCenterY},
                        iconSize,
                        iconCol
                    );
                    float const textX = contentLeft + iconSize + 4.0f * uiScale;
                    if (contentRight - textX >= 8.0f * uiScale) {
                        std::string const modeText = weatherModeLabel(it->second.weather);
                        std::string const elided =
                            elideTextToWidth(labelFont, labelFontSize, modeText, contentRight - textX);
                        if (!elided.empty()) {
                            ImVec2 const textSize =
                                labelFont->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, elided.c_str());
                            float const ty = std::floor(bandCenterY - textSize.y * 0.5f + 0.5f);
                            drawList->AddText(
                                labelFont,
                                labelFontSize,
                                {textX, ty},
                                trackEnabled ? theme::kText : theme::kTextDim,
                                elided.c_str()
                            );
                        }
                    }
                    drawList->PopClipRect();
                }
            }

            // 3) Hit-test weather keyframes on this lane.
            std::optional<int> hoveredWeatherKeyTick;
            float              hoveredWeatherKeyX        = 0.0f;
            float              hoveredWeatherKeyRadius   = keyRadius;
            bool               hoveredWeatherKeySelected = false;
            if (mouseInRow && mTool != TimelineTool::Razor) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, _] : weatherTrack.keysByTick) {
                    bool const  selected  = ctx.selection.isWeatherKeyframeSelected(keyTick);
                    int const   drawnTick = drawnTickForWeatherKey(keyTick, selected);
                    float const kx        = scale.xAt(static_cast<float>(drawnTick));
                    if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                    float const radius = selected ? keyRadius + 1.0f : keyRadius;
                    float const dx     = std::abs(mouse.x - kx);
                    float const dy     = std::abs(mouse.y - bandCenterY);
                    if (dx <= radius + 4.0f && dy <= radius + 5.0f) {
                        float const dist = std::hypot(dx, dy);
                        if (dist < bestDist) {
                            bestDist                  = dist;
                            hoveredWeatherKeyTick     = keyTick;
                            hoveredWeatherKeyX        = kx;
                            hoveredWeatherKeyRadius   = radius;
                            hoveredWeatherKeySelected = selected;
                        }
                    }
                }
            }

            // 4) Keyframe hover tooltip and mouse interaction.
            bool const overWeatherKey = hoveredWeatherKeyTick.has_value();
            if (overWeatherKey) {
                if (mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (mDraggingWeatherKeyframeOriginTick < 0) {
                    if (auto const keyIt = weatherTrack.keysByTick.find(*hoveredWeatherKeyTick);
                        keyIt != weatherTrack.keysByTick.end()) {
                        std::string const tip =
                            formatTick(*hoveredWeatherKeyTick) + " · " + weatherModeLabel(keyIt->second.weather);
                        widgets::hoverTooltip(tip.c_str());
                    }
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed            = true;
                    int const keyTick        = *hoveredWeatherKeyTick;
                    mWeatherKeyframeMenuTick = keyTick;
                    mWeatherKeyframeMenuGroup =
                        hoveredWeatherKeySelected && ctx.selection.selectedWeatherKeyframes().size() > 1;
                    if (!mWeatherKeyframeMenuGroup) {
                        ctx.selection.selectWeatherKeyframe(keyTick);
                    }
                    widgets::openAnchoredPopup(
                        "##weather-keyframe-menu",
                        {hoveredWeatherKeyX - hoveredWeatherKeyRadius, bandCenterY - hoveredWeatherKeyRadius},
                        {hoveredWeatherKeyX + hoveredWeatherKeyRadius, bandCenterY + hoveredWeatherKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredWeatherKeyTick;
                    if (ImGui::GetIO().KeyShift && !trackLocked) {
                        if (auto const anchor = ctx.selection.selectedWeatherKeyframe()) {
                            int const        lo = std::min(*anchor, keyTick);
                            int const        hi = std::max(*anchor, keyTick);
                            std::vector<int> ticks;
                            for (auto const& [t, _] : weatherTrack.keysByTick) {
                                if (t >= lo && t <= hi) ticks.push_back(t);
                            }
                            if (ticks.size() <= 1) ctx.selection.selectWeatherKeyframe(keyTick);
                            else ctx.selection.selectWeatherKeyframes(std::move(ticks));
                        } else {
                            ctx.selection.selectWeatherKeyframe(keyTick);
                        }
                    } else if (ImGui::GetIO().KeyCtrl && !trackLocked) {
                        std::vector<int> ticks = ctx.selection.selectedWeatherKeyframes();
                        auto const       it    = std::ranges::find(ticks, keyTick);
                        if (it != ticks.end()) ticks.erase(it);
                        else ticks.push_back(keyTick);
                        if (ticks.empty()) {
                            ctx.selection.clear();
                        } else if (ticks.size() == 1) {
                            ctx.selection.selectWeatherKeyframe(ticks.front());
                        } else {
                            ctx.selection.selectWeatherKeyframes(std::move(ticks));
                        }
                    } else if (hoveredWeatherKeySelected && ctx.selection.selectedWeatherKeyframes().size() > 1
                               && !trackLocked) {
                        mWeatherGroupDrag                   = true;
                        mWeatherGroupDragDelta              = 0;
                        mDraggingWeatherKeyframeOriginTick  = keyTick;
                        mDraggingWeatherKeyframeStartMouseX = mouse.x;
                        mDraggingWeatherKeyframeTick        = keyTick;
                        mDraggingWeatherKeyframeMoved       = false;
                    } else {
                        ctx.selection.selectWeatherKeyframe(keyTick);
                        if (trackLocked) {
                            submitSeek(ctx, keyTick);
                        } else {
                            mDraggingWeatherKeyframeOriginTick  = keyTick;
                            mDraggingWeatherKeyframeStartMouseX = mouse.x;
                            mDraggingWeatherKeyframeTick        = keyTick;
                            mDraggingWeatherKeyframeMoved       = false;
                        }
                    }
                }
            }

            // 5) Pen preview / Ctrl+Click / Alt+Drag box select / double-click / context menu / seek.
            bool const penMode = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen =
                mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt;
            bool const altBoxSelect           = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
            auto const startWeatherBoxPending = [&](bool penAdd, int penTick) {
                mBoxPending          = true;
                mBoxSelecting        = false;
                mBoxAdditive         = ImGui::GetIO().KeyCtrl;
                mBoxStart            = mouse;
                mBoxClickTimeOfDay   = false;
                mBoxClickWeather     = true;
                mPenPendingAdd       = penAdd;
                mPenPendingTimeOfDay = false;
                mPenPendingWeather   = penAdd;
                mPenPendingTick      = penTick;
                mBoxClickCameraId.clear();
            };

            std::optional<int> penPreviewTick;
            if (mouseInRow && !overWeatherKey && !trackLocked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick = std::clamp(penTick, 0, state.totalTicks);
                if (!weatherTrack.keysByTick.contains(penTick)) penPreviewTick = penTick;
            }
            if (penPreviewTick.has_value()) {
                float const penX = scale.xAt(static_cast<float>(*penPreviewTick));
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    bandCenterY,
                    keyRadius,
                    theme::withAlpha(kWeatherAccent, 0x88),
                    theme::withAlpha(theme::kKeyframeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (penMode) {
                        startWeatherBoxPending(true, *penPreviewTick);
                    } else {
                        addWeatherKeyframeAt(ctx, *penPreviewTick);
                    }
                }
            } else if (mouseInRow && (!overWeatherKey || mTool == TimelineTool::BoxSelect) && !clickConsumed) {
                if (!overWeatherKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) tick = snapTick(tick);
                    mWeatherLaneMenuTick = tick;
                    float const anchorX  = scale.xAt(static_cast<float>(tick));
                    ctx.selection.selectWeatherTrack();
                    widgets::openAnchoredPopup("##weather-lane-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (!overWeatherKey && !trackLocked && mTool == TimelineTool::Select
                           && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    int addTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (shiftSnap) {
                        addTick = snapToEditPoints(ctx, addTick, scale.pixelsPerTick, true, true, {}, {});
                    } else if (mSnapEnabled) {
                        addTick = snapTick(addTick);
                    }
                    addTick = std::clamp(addTick, 0, state.totalTicks);
                    if (!weatherTrack.keysByTick.contains(addTick)) {
                        clickConsumed = true;
                        addWeatherKeyframeAt(ctx, addTick);
                    }
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (penMode || altBoxSelect || mTool == TimelineTool::Select || mTool == TimelineTool::BoxSelect) {
                        startWeatherBoxPending(false, -1);
                    } else {
                        ctx.selection.selectWeatherTrack();
                        submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                    }
                }
            }

            // 6) Draw weather keyframe diamond nodes on top of the spans.
            for (auto const& [keyTick, key] : weatherTrack.keysByTick) {
                bool const selected = ctx.selection.isWeatherKeyframeSelected(keyTick);
                bool const dragging = (mWeatherGroupDrag && selected)
                                   || (!mWeatherGroupDrag && mDraggingWeatherKeyframeOriginTick == keyTick);
                int const   drawnTick = drawnTickForWeatherKey(keyTick, selected);
                float const kx        = scale.xAt(static_cast<float>(drawnTick));
                if (kx < layout.canvasLeft - keyRadius || kx > layout.fullMax.x + keyRadius) continue;
                bool const inBox = mBoxSelecting && !trackLocked && boxContains(kx, bandCenterY);
                if (inBox) weatherBoxHits.push_back(keyTick);
                bool const  hovered = hoveredWeatherKeyTick && *hoveredWeatherKeyTick == keyTick;
                bool const  lit     = selected || dragging || inBox;
                float const radius  = lit ? keyRadius + 1.0f : (hovered ? keyRadius + 0.5f : keyRadius);
                ImU32 const modeCol = weatherModeColor(key.weather);
                ImU32 const fill    = lit ? theme::kSelected : (trackEnabled ? modeCol : theme::kKeyframeDisabled);
                ImU32 const outline = lit ? theme::kIconHighlight : theme::kKeyframeOutline;
                if (hovered) {
                    ImU32 const glowCol = lit ? theme::kSelected : modeCol;
                    drawKeyframeDiamond(
                        drawList,
                        kx,
                        bandCenterY,
                        radius + 2.5f * uiScale,
                        theme::withAlpha(glowCol, 0x44),
                        theme::withAlpha(glowCol, 0x90)
                    );
                }
                drawKeyframeDiamond(drawList, kx, bandCenterY, radius, fill, outline);
            }
        } else if (row.kind == TrackRowKind::Audio) {
            auto const* audioTrack = state::editing::ClipOps::findAudioTrack(*project, row.id);
            if (audioTrack) {
                bool const trackEnabled = audioTrack->enabled;
                bool const trackMuted   = audioTrack->muted;
                bool const trackLocked  = audioTrack->locked;
                bool const mouseInRow =
                    allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);
                auto const    selectedAudioClip = ctx.selection.selectedAudioClip();
                float const   rounding          = 4.0f * uiScale;
                float const   midY              = (blockTop + blockBot) * 0.5f;
                float const   waveHalfH         = std::max(2.0f, (blockBot - blockTop) * 0.5f - 2.0f * uiScale);
                float const   stepX             = std::max(2.0f, 2.0f * uiScale);
                float const   barW              = std::max(1.0f, stepX - 0.5f * uiScale);
                ImFont* const font              = ImGui::GetFont();

                state::editing::model::AudioClip const*                   hoveredAudioClip = nullptr;
                float                                                     hoveredAudioX0   = 0.0f;
                float                                                     hoveredAudioX1   = 0.0f;
                std::shared_ptr<audio::external::DecodedAudioAsset const> hoveredAsset;
                double                                                    hoveredEffDur   = 0.0;
                double                                                    hoveredStartSec = 0.0;
                double                                                    hoveredEndSec   = 0.0;

                for (auto const& clip : audioTrack->clips) {
                    bool const draggingThis =
                        mDraggingAudioTrackId == audioTrack->id && mDraggingAudioClipId == clip.id;
                    int const    drawnStartTick = draggingThis ? mDraggingAudioDraftTick : clip.startTick;
                    auto const   asset          = audio::external::getOrRequestAudioAsset(clip.filePath);
                    double const effectiveDur =
                        std::max(0.0, audio::external::effectiveAudioClipDurationSeconds(clip, asset.get()));
                    double const startOutSec =
                        audioTimeMapping
                            ? (audioTimeMapping->unscaledTicksAt(static_cast<long double>(drawnStartTick)) / 20.0)
                            : (static_cast<double>(drawnStartTick) / 20.0);
                    double const endOutSec = startOutSec + effectiveDur;
                    double const endReplayTick =
                        audioTimeMapping
                            ? audioTimeMapping->replayTickForUnscaled(static_cast<long double>(endOutSec * 20.0))
                            : (static_cast<double>(drawnStartTick) + effectiveDur * 20.0);

                    float const rawX0 = scale.xAt(static_cast<float>(drawnStartTick));
                    float const rawX1 = std::max(rawX0 + 24.0f * uiScale, scale.xAt(static_cast<float>(endReplayTick)));
                    float const x0    = std::max(layout.canvasLeft, rawX0);
                    float const x1    = std::min(layout.fullMax.x, rawX1);
                    if (x1 <= x0) continue;

                    bool const clipSelected = selectedAudioClip && selectedAudioClip->trackId == audioTrack->id
                                           && selectedAudioClip->clipId == clip.id;
                    bool const isFailed   = asset && asset->state == audio::external::AudioLoadState::Failed;
                    bool const isPending  = !asset || asset->state == audio::external::AudioLoadState::Pending;
                    bool const isReady    = asset && asset->state == audio::external::AudioLoadState::Ready;
                    bool const activeClip = trackEnabled && !trackMuted && !clip.muted;

                    ImU32 const baseTint  = activeClip ? kAudioAccent : theme::kTrackDisabled;
                    int const   fillAlpha = activeClip ? (clipSelected ? 0x5c : 0x42) : (clipSelected ? 0x48 : 0x2e);
                    drawList->AddRectFilled(
                        {x0, blockTop},
                        {x1, blockBot},
                        theme::withAlpha(baseTint, fillAlpha),
                        rounding
                    );

                    if (isReady && !asset->peakEnvelope.empty() && effectiveDur > 0.0) {
                        drawList->AddLine(
                            {x0 + 1.0f, midY},
                            {x1 - 1.0f, midY},
                            theme::withAlpha(baseTint, activeClip ? 0x55 : 0x30),
                            1.0f
                        );
                        float const waveX0 = std::max(x0 + 1.0f, layout.canvasLeft);
                        float const waveX1 = std::min(x1 - 1.0f, layout.fullMax.x);
                        float const trackVol =
                            std::isfinite(audioTrack->volume) ? std::max(0.0f, audioTrack->volume) : 1.0f;
                        float const  clipVol  = std::isfinite(clip.volume) ? std::max(0.0f, clip.volume) : 1.0f;
                        float const  volScale = std::clamp(clipVol * trackVol, 0.25f, 1.5f);
                        double const trimStart =
                            std::isfinite(clip.trimStartSeconds) ? std::max(0.0, clip.trimStartSeconds) : 0.0;
                        ImU32 const waveCol     = theme::withAlpha(baseTint, activeClip ? 0xb8 : 0x50);
                        float const maxBarHalfH = waveHalfH * 0.92f;
                        for (float bx = waveX0; bx < waveX1; bx += stepX) {
                            float const  bxEnd = std::min(bx + barW, waveX1);
                            double const t0    = static_cast<double>(scale.tickAtExact(bx));
                            double const t1    = static_cast<double>(scale.tickAtExact(bx + stepX));
                            double const outSec0 =
                                audioTimeMapping ? (audioTimeMapping->unscaledTicksAt(t0) / 20.0) : (t0 / 20.0);
                            double const outSec1 =
                                audioTimeMapping ? (audioTimeMapping->unscaledTicksAt(t1) / 20.0) : (t1 / 20.0);
                            if (outSec1 <= startOutSec || outSec0 >= endOutSec) continue;
                            double const  s0   = trimStart + std::max(0.0, outSec0 - startOutSec);
                            double const  s1   = trimStart + std::max(0.0, std::min(endOutSec, outSec1) - startOutSec);
                            uint8_t const peak = asset->samplePeakRange(s0, std::max(s0, s1));
                            float const   amp  = std::clamp((static_cast<float>(peak) / 255.0f) * volScale, 0.0f, 1.0f);
                            float const   barH = std::max(1.0f, amp * maxBarHalfH);
                            drawList->AddRectFilled({bx, midY - barH}, {bxEnd, midY + barH}, waveCol);
                        }
                    }

                    ImU32 const borderCol = isFailed
                                              ? kAudioErrorEdge
                                              : (clipSelected ? theme::kKeyframeSelected
                                                              : theme::withAlpha(baseTint, activeClip ? 0xc0 : 0x6c));
                    drawList->AddRect(
                        {x0, blockTop},
                        {x1, blockBot},
                        borderCol,
                        rounding,
                        0,
                        (clipSelected || isFailed) ? 1.5f : 1.0f
                    );

                    float rightEdge = x1 - 4.0f * uiScale;
                    if (isFailed && rightEdge - (x0 + 6.0f * uiScale) >= eyeIconSize + 6.0f * uiScale) {
                        drawCompactIconAtCentre(
                            drawList,
                            ICON_WARNING,
                            {rightEdge - eyeIconSize * 0.5f, cornerCenterY},
                            eyeIconSize,
                            kAudioErrorText
                        );
                        rightEdge -= eyeIconSize + 2.0f * uiScale;
                    }
                    if ((clip.muted || trackMuted)
                        && rightEdge - (x0 + 6.0f * uiScale) >= eyeIconSize + 6.0f * uiScale) {
                        drawCompactIconAtCentre(
                            drawList,
                            ICON_MUTE,
                            {rightEdge - eyeIconSize * 0.5f, cornerCenterY},
                            eyeIconSize,
                            theme::kWarning
                        );
                        rightEdge -= eyeIconSize + 2.0f * uiScale;
                    }

                    std::string displayName = clip.name;
                    if (displayName.empty() && !clip.filePath.empty()) {
                        auto const sep = clip.filePath.find_last_of("/\\");
                        displayName    = sep == std::string::npos ? clip.filePath : clip.filePath.substr(sep + 1);
                    }
                    if (isPending) {
                        displayName += " (" + "playback.refactorEditor.audio.statusLoading"_tr() + ")";
                    } else if (std::isfinite(clip.volume) && std::abs(clip.volume - 1.0f) >= 0.01f) {
                        char volBuf[24]{};
                        std::snprintf(
                            volBuf,
                            sizeof(volBuf),
                            " (%d%%)",
                            static_cast<int>(std::lround(clip.volume * 100.0f))
                        );
                        displayName += volBuf;
                    }
                    float const textX = x0 + 5.0f * uiScale;
                    if (!displayName.empty() && rightEdge > textX + 4.0f * uiScale) {
                        std::string const elided =
                            elideTextToWidth(font, cornerFontSize, displayName, rightEdge - textX);
                        if (!elided.empty()) {
                            float const  textY = std::floor(cornerCenterY - cornerFontSize * 0.5f + 0.5f);
                            ImVec4 const labelClip{textX, blockTop, rightEdge, blockBot};
                            ImU32 const  labelCol =
                                isFailed ? kAudioErrorText : (activeClip ? theme::kText : theme::kTextDim);
                            drawList->AddText(
                                font,
                                cornerFontSize,
                                {textX, textY},
                                labelCol,
                                elided.c_str(),
                                nullptr,
                                0.0f,
                                &labelClip
                            );
                        }
                    }

                    if (mouseInRow && contains({x0, blockTop}, {x1, blockBot}, mouse)) {
                        hoveredAudioClip = &clip;
                        hoveredAudioX0   = x0;
                        hoveredAudioX1   = x1;
                        hoveredAsset     = asset;
                        hoveredEffDur    = effectiveDur;
                        hoveredStartSec  = startOutSec;
                        hoveredEndSec    = endOutSec;
                    }
                }

                bool const altBoxSelect = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;
                if (hoveredAudioClip) {
                    if (mDraggingAudioClipId.empty()) {
                        if (!trackLocked && mTool != TimelineTool::BoxSelect) ImGui::SetMouseCursor(theme::kCursorGrab);
                        std::string baseName = hoveredAudioClip->filePath;
                        if (auto const sep = baseName.find_last_of("/\\"); sep != std::string::npos) {
                            baseName = baseName.substr(sep + 1);
                        }
                        std::string tip = hoveredAudioClip->name.empty() ? baseName : hoveredAudioClip->name;
                        if (!baseName.empty() && baseName != tip) tip += "\n" + baseName;
                        char timeBuf[128]{};
                        std::snprintf(
                            timeBuf,
                            sizeof(timeBuf),
                            "%s  (%.2fs -> %.2fs, %.2fs)",
                            formatTick(hoveredAudioClip->startTick).c_str(),
                            hoveredStartSec,
                            hoveredEndSec,
                            hoveredEffDur
                        );
                        tip += "\n" + std::string(timeBuf);
                        char volBuf[64]{};
                        std::snprintf(
                            volBuf,
                            sizeof(volBuf),
                            "%s: %d%%",
                            "playback.refactorEditor.audio.volume"_tr().c_str(),
                            static_cast<int>(std::lround(hoveredAudioClip->volume * 100.0f))
                        );
                        tip += "\n" + std::string(volBuf);
                        if (hoveredAudioClip->muted || trackMuted) {
                            tip += "  [" + "playback.refactorEditor.audio.muted"_tr() + "]";
                        }
                        if (!hoveredAsset || hoveredAsset->state == audio::external::AudioLoadState::Pending) {
                            tip += "\n" + "playback.refactorEditor.audio.statusLoading"_tr();
                        } else if (hoveredAsset->state == audio::external::AudioLoadState::Ready) {
                            tip += "\n" + "playback.refactorEditor.audio.statusReady"_tr(hoveredAsset->durationSeconds);
                        } else {
                            tip += "\n" + "playback.refactorEditor.audio.statusFailed"_tr();
                            if (!hoveredAsset->errorMessage.empty()) tip += ": " + hoveredAsset->errorMessage;
                        }
                        widgets::hoverTooltip(tip.c_str());
                    }

                    if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        clickConsumed = true;
                        ctx.selection.selectAudioClip(audioTrack->id, hoveredAudioClip->id);
                        mAudioClipMenuTrackId = audioTrack->id;
                        mAudioClipMenuClipId  = hoveredAudioClip->id;
                        widgets::openAnchoredPopup(
                            "##timeline-audio-clip-menu",
                            {hoveredAudioX0, blockTop},
                            {hoveredAudioX1, blockBot}
                        );
                    } else if (!clickConsumed && mTool != TimelineTool::BoxSelect && !altBoxSelect
                               && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        clickConsumed = true;
                        ctx.selection.selectAudioClip(audioTrack->id, hoveredAudioClip->id);
                        if (!trackLocked) {
                            mDraggingAudioTrackId     = audioTrack->id;
                            mDraggingAudioClipId      = hoveredAudioClip->id;
                            mDraggingAudioStartMouseX = mouse.x;
                            mDraggingAudioOriginTick  = hoveredAudioClip->startTick;
                            mDraggingAudioDraftTick   = hoveredAudioClip->startTick;
                            mDraggingAudioMoved       = false;
                        } else {
                            submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                        }
                    }
                } else if (mouseInRow && !clickConsumed) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        clickConsumed = true;
                        int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                        if (mSnapEnabled) tick = snapTick(tick);
                        mAudioTrackMenuId   = audioTrack->id;
                        mAudioLaneMenuTick  = tick;
                        float const anchorX = scale.xAt(static_cast<float>(tick));
                        ctx.selection.selectAudioTrack(audioTrack->id);
                        widgets::openAnchoredPopup("##timeline-audio-menu", {anchorX, blockTop}, {anchorX, blockBot});
                    } else if (mTool != TimelineTool::BoxSelect && !altBoxSelect
                               && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        clickConsumed = true;
                        ctx.selection.selectAudioTrack(audioTrack->id);
                        submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                    }
                }
            }
        } else if (row.kind == TrackRowKind::Camera && row.cameraIndex >= 0
                   && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera     = project->cameras[row.cameraIndex];
            ImU32 const trackColor = theme::trackColor(row.cameraIndex);
            bool const  mouseInRow =
                allowInput && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse);

            auto const drawnTickForKey = [&](int keyTick, bool selected) {
                bool const dragging = mDraggingKeyframeCameraId == camera.id && mDraggingKeyframeStartTick == keyTick;
                if (mGroupDrag && selected) return keyTick + mGroupDragDelta;
                if (!mGroupDrag && dragging) return mDraggingKeyframeTick;
                return keyTick;
            };
            auto const startBoxPending = [&](std::string const& cameraId, bool penAdd, int penTick) {
                mBoxPending          = true;
                mBoxSelecting        = false;
                mBoxAdditive         = ImGui::GetIO().KeyCtrl;
                mBoxStart            = mouse;
                mBoxClickCameraId    = cameraId;
                mBoxClickClipId.clear();
                mBoxClickKeyTick     = -1;
                mBoxClickTimeOfDay   = false;
                mBoxClickWeather     = false;
                mPenPendingAdd       = penAdd;
                mPenPendingTimeOfDay = false;
                mPenPendingWeather   = false;
                mPenPendingTick      = penTick;
            };

            // Priority 1: existing keyframe hit-testing (checked first so keyframes always win over clips).
            std::optional<int> hoveredKeyTick;
            float              hoveredKeyX        = 0.0f;
            float              hoveredKeyRadius   = keyRadius;
            bool               hoveredKeySelected = false;
            if (mouseInRow) {
                float bestDist = std::numeric_limits<float>::max();
                for (auto const& [keyTick, key] : camera.keysByTick) {
                    bool const  selected  = ctx.selection.isKeyframeSelected(camera.id, keyTick);
                    int const   drawnTick = drawnTickForKey(keyTick, selected);
                    float const x         = scale.xAt(static_cast<float>(drawnTick));
                    if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                    float const radius = selected ? keyRadius + 1.0f : keyRadius;
                    float const distX  = std::abs(mouse.x - x);
                    if (distX <= radius + 4.0f && std::abs(mouse.y - keyCenterY) <= radius + 5.0f && distX < bestDist) {
                        bestDist           = distX;
                        hoveredKeyTick     = keyTick;
                        hoveredKeyX        = x;
                        hoveredKeyRadius   = radius;
                        hoveredKeySelected = selected;
                    }
                }
            }
            bool const overAnyKeyOnRow = hoveredKeyTick.has_value() && mTool != TimelineTool::Razor;
            if (hoveredKeyTick.has_value()) {
                if (mTool != TimelineTool::Razor && mTool != TimelineTool::BoxSelect) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed         = true;
                    int const keyTick     = *hoveredKeyTick;
                    mKeyframeMenuCameraId = camera.id;
                    mKeyframeMenuTick     = keyTick;
                    mKeyframeMenuTickEdit = keyTick;
                    // Right-clicking inside a multi-selection keeps it so the menu can act on the whole set.
                    mKeyframeMenuGroup = hoveredKeySelected && ctx.selection.keyframeCount() > 1;
                    if (!mKeyframeMenuGroup) {
                        ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    }
                    widgets::openAnchoredPopup(
                        "##timeline-keyframe-menu",
                        {hoveredKeyX - hoveredKeyRadius, keyCenterY - hoveredKeyRadius},
                        {hoveredKeyX + hoveredKeyRadius, keyCenterY + hoveredKeyRadius}
                    );
                } else if (!clickConsumed && mTool != TimelineTool::Razor && mTool != TimelineTool::BoxSelect
                           && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed     = true;
                    int const keyTick = *hoveredKeyTick;
                    if (ImGui::GetIO().KeyCtrl && !camera.locked) {
                        ctx.selection.toggleKeyframe({camera.id, keyTick});
                    } else if (hoveredKeySelected && ctx.selection.keyframeCount() > 1) {
                        // Grabbing a member of the set moves the set; a click without movement narrows to this key.
                        mGroupDrag                   = true;
                        mGroupDragDelta              = 0;
                        mDraggingKeyframeCameraId    = camera.id;
                        mDraggingKeyframeStartMouseX = mouse.x;
                        mDraggingKeyframeStartTick   = keyTick;
                        mDraggingKeyframeTick        = keyTick;
                        mDraggingKeyframeMoved       = false;
                    } else {
                        ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                        if (ctx.commands.isAutoPreviewEnabled()) {
                            EditorAction previewAction{EditorActionType::SetPreviewCamera};
                            previewAction.id = camera.id;
                            ctx.submitAction(std::move(previewAction));
                        }
                        if (camera.locked || !camera.enabled) {
                            submitSeek(ctx, keyTick);
                        } else {
                            mDraggingKeyframeCameraId    = camera.id;
                            mDraggingKeyframeStartMouseX = mouse.x;
                            mDraggingKeyframeStartTick   = keyTick;
                            mDraggingKeyframeTick        = keyTick;
                            mDraggingKeyframeMoved       = false;
                        }
                    }
                }
            }

            bool const penMode       = mTool == TimelineTool::Pen;
            bool const ctrlSelectPen = mTool == TimelineTool::Select && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt
                                    && std::abs(mouse.y - keyCenterY) <= keyRadius + 6.0f;
            bool const altBoxSelect = mTool == TimelineTool::Select && ImGui::GetIO().KeyAlt;

            std::optional<int> penPreviewTick;
            if (mouseInRow && !overAnyKeyOnRow && !camera.locked && (penMode || ctrlSelectPen)) {
                int penTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                if (shiftSnap) {
                    penTick = snapToEditPoints(ctx, penTick, scale.pixelsPerTick, true, true, {}, {});
                } else if (mSnapEnabled) {
                    penTick = snapTick(penTick);
                }
                penTick        = std::clamp(penTick, 0, state.totalTicks);
                penPreviewTick = penTick;
            }

            // 1) Uncovered gaps across [0, state.totalTicks].
            if (state.totalTicks > 0) {
                std::vector<std::pair<int, int>> clipIntervals;
                clipIntervals.reserve(camera.clips.size());
                for (auto const& clip : camera.clips) {
                    bool const draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                    int const  cIn  = std::clamp(draggingThis ? mClipDragInTick : clip.inTick, 0, state.totalTicks);
                    int const  cOut = std::clamp(draggingThis ? mClipDragOutTick : clip.outTick, cIn, state.totalTicks);
                    if (cIn < cOut) clipIntervals.emplace_back(cIn, cOut);
                }
                std::ranges::sort(clipIntervals);

                auto const shadeGap = [&](int gapStart, int gapEnd) {
                    if (gapEnd <= gapStart) return;
                    float const gx0 = std::max(layout.canvasLeft, scale.xAt(static_cast<float>(gapStart)));
                    float const gx1 = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(gapEnd)));
                    if (gx1 <= gx0) return;
                    drawList->AddRectFilled({gx0, blockTop}, {gx1, blockBot}, IM_COL32(0, 0, 0, 0x1c));
                };
                int cursor = 0;
                for (auto const& [cIn, cOut] : clipIntervals) {
                    shadeGap(cursor, cIn);
                    cursor = std::max(cursor, cOut);
                }
                shadeGap(cursor, state.totalTicks);
            }

            // 2) Uniform single-piece rounded clip pills.
            state::editing::model::Clip const* hoveredClipPtr       = nullptr;
            float                              hoveredClipX0        = 0.0f;
            float                              hoveredClipX1        = 0.0f;
            bool                               hoveredClipEye       = false;
            bool                               hoveredClipLeftEdge  = false;
            bool                               hoveredClipRightEdge = false;
            {
                float const   rounding = 5.0f * uiScale;
                float const   eyeBox   = cornerH + 2.0f * uiScale;
                ImFont* const font     = ImGui::GetFont();
                for (auto const& clip : camera.clips) {
                    bool const  draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                    int const   drawnIn      = draggingThis ? mClipDragInTick : clip.inTick;
                    int const   drawnOut     = draggingThis ? mClipDragOutTick : clip.outTick;
                    float const rawX0        = scale.xAt(static_cast<float>(drawnIn));
                    float const rawX1        = scale.xAt(static_cast<float>(drawnOut));
                    float const x0           = std::max(layout.canvasLeft, rawX0);
                    float const x1           = std::min(layout.fullMax.x, rawX1);
                    if (x1 <= x0) continue;

                    bool const clipSelected = selectedClip && selectedClip->clipId == clip.id;
                    bool const overClip =
                        mouseInRow && !overAnyKeyOnRow && contains({x0, blockTop}, {x1, blockBot}, mouse);
                    bool const exportOff = !clip.exportEnabled;
                    bool const showEye =
                        (x1 - x0) >= 44.0f * uiScale || (overClip && (x1 - x0) >= eyeBox + 6.0f * uiScale);
                    float const eyeX = x1 - eyeBox - 4.0f * uiScale;
                    bool const  overEye =
                        showEye && overClip && mTool == TimelineTool::Select && !penPreviewTick.has_value()
                        && !altBoxSelect
                        && contains({eyeX - 2.0f, blockTop}, {x1, blockTop + eyeBox + 3.0f * uiScale}, mouse);
                    float const handleReach  = std::min(6.0f * uiScale, std::max(2.0f, (rawX1 - rawX0) * 0.25f));
                    bool const  overLeftEdge = mTool == TimelineTool::Select && overClip && !overEye
                                           && !penPreviewTick.has_value() && !altBoxSelect
                                           && std::abs(mouse.x - rawX0) <= handleReach;
                    bool const overRightEdge = mTool == TimelineTool::Select && overClip && !overEye
                                            && !penPreviewTick.has_value() && !altBoxSelect && !overLeftEdge
                                            && std::abs(mouse.x - rawX1) <= handleReach;

                    if (overClip && !hoveredClipPtr) {
                        hoveredClipPtr       = &clip;
                        hoveredClipX0        = x0;
                        hoveredClipX1        = x1;
                        hoveredClipEye       = overEye;
                        hoveredClipLeftEdge  = overLeftEdge;
                        hoveredClipRightEdge = overRightEdge;
                    }

                    bool const  activeClip = camera.enabled && clip.exportEnabled;
                    ImU32 const swatch     = activeClip ? trackColor : theme::kTrackDisabled;
                    int const   fillAlpha  = activeClip ? (clipSelected ? 0x82 : 0x64) : (clipSelected ? 0x64 : 0x46);
                    drawList
                        ->AddRectFilled({x0, blockTop}, {x1, blockBot}, theme::withAlpha(swatch, fillAlpha), rounding);
                    if (exportOff && x1 - x0 > 4.0f && blockBot - blockTop > 4.0f) {
                        drawDiagonalHatch(
                            drawList,
                            {x0 + 2.0f, blockTop + 2.0f},
                            {x1 - 2.0f, blockBot - 2.0f},
                            theme::withAlpha(theme::kTextDim, 0x30),
                            7.0f * uiScale
                        );
                    }
                    if (activeClip && x1 - x0 > rounding + 4.0f) {
                        drawList->AddLine(
                            {x0 + rounding * 0.5f, blockTop + 1.0f},
                            {x1 - rounding * 0.5f, blockTop + 1.0f},
                            IM_COL32(255, 255, 255, 0x1c),
                            1.0f
                        );
                    }
                    drawList->AddRect(
                        {x0, blockTop},
                        {x1, blockBot},
                        clipSelected ? theme::kSelected : theme::withAlpha(swatch, exportOff ? 0x68 : 0xc0),
                        rounding,
                        0,
                        clipSelected ? 1.75f : 1.0f
                    );

                    bool const showTrimGrips = (overClip || clipSelected || draggingThis)
                                            && mTool == TimelineTool::Select && rawX1 - rawX0 >= 12.0f * uiScale
                                            && !camera.locked && !clip.locked;
                    if (showTrimGrips) {
                        float const gripW     = 2.75f * uiScale;
                        float const gripTop   = blockTop + 5.0f * uiScale;
                        float const gripBot   = blockBot - 5.0f * uiScale;
                        float const gripInset = 2.5f * uiScale;
                        float const gripR     = 1.5f * uiScale;
                        if (gripBot > gripTop) {
                            if (rawX0 >= layout.canvasLeft - 1.0f) {
                                ImU32 const leftCol = (overLeftEdge || (draggingThis && mClipDragMode == 1))
                                                        ? theme::kSelected
                                                        : theme::withAlpha(theme::kText, 0x70);
                                drawList->AddRectFilled(
                                    {rawX0 + gripInset, gripTop},
                                    {rawX0 + gripInset + gripW, gripBot},
                                    leftCol,
                                    gripR
                                );
                            }
                            if (rawX1 <= layout.fullMax.x + 1.0f) {
                                ImU32 const rightCol = (overRightEdge || (draggingThis && mClipDragMode == 2))
                                                         ? theme::kSelected
                                                         : theme::withAlpha(theme::kText, 0x70);
                                drawList->AddRectFilled(
                                    {rawX1 - gripInset - gripW, gripTop},
                                    {rawX1 - gripInset, gripBot},
                                    rightCol,
                                    gripR
                                );
                            }
                        }
                    }

                    if (showEye) {
                        ImU32 const eyeCol = overEye ? theme::kIconHighlight
                                                     : (clip.exportEnabled ? theme::kIconActive : theme::kTextDim);
                        drawCompactIconAtCentre(
                            drawList,
                            clip.exportEnabled ? ICON_EYE : ICON_HIDE,
                            {eyeX + eyeBox * 0.5f, cornerCenterY},
                            eyeIconSize,
                            eyeCol
                        );
                        if (overEye) {
                            widgets::hoverTooltip("playback.refactorEditor.timeline.toggleClipExport"_tr().c_str());
                        }
                    }

                    float rightEdge = showEye ? (eyeX - 3.0f * uiScale) : (x1 - 5.0f * uiScale);
                    if (clip.locked && rightEdge - (x0 + 6.0f * uiScale) >= eyeIconSize + 10.0f * uiScale) {
                        drawCompactIconAtCentre(
                            drawList,
                            ICON_LOCK,
                            {rightEdge - eyeIconSize * 0.5f, cornerCenterY},
                            eyeIconSize,
                            theme::kWarning
                        );
                        rightEdge -= eyeIconSize + 2.0f * uiScale;
                    }

                    float const borderInset      = clipSelected ? 1.5f : 0.75f;
                    bool const  shiftForLeftGrip = showTrimGrips && rawX0 >= layout.canvasLeft - 1.0f;
                    float const contentLeft      = shiftForLeftGrip ? (x0 + 6.5f * uiScale) : (x0 + borderInset);

                    if (exportOff && (x1 - x0) >= 84.0f * uiScale) {
                        std::string const skippedLabel = "playback.refactorEditor.timeline.clipExportSkipped"_tr();
                        float const       tagFontSize  = std::max(9.5f, std::round(cornerFontSize * 0.90f));
                        ImVec2 const      tagTextSize =
                            font->CalcTextSizeA(tagFontSize, FLT_MAX, 0.0f, skippedLabel.c_str());
                        float const tagPadX = 4.0f * uiScale;
                        float const tagW    = tagTextSize.x + tagPadX * 2.0f;
                        float const tagH    = cornerFontSize + 1.0f * uiScale;
                        if (rightEdge - contentLeft >= tagW + 6.0f * uiScale) {
                            float const tagX0  = rightEdge - tagW;
                            float const tagTop = cornerCenterY - tagH * 0.5f;
                            drawList->AddRectFilled(
                                {tagX0, tagTop},
                                {rightEdge, tagTop + tagH},
                                IM_COL32(0x16, 0x18, 0x1e, 0xd8),
                                3.0f * uiScale
                            );
                            drawList->AddRect(
                                {tagX0, tagTop},
                                {rightEdge, tagTop + tagH},
                                theme::withAlpha(theme::kTextDim, 0x60),
                                3.0f * uiScale,
                                0,
                                1.0f
                            );
                            drawList->AddText(
                                font,
                                tagFontSize,
                                {tagX0 + tagPadX, std::floor(cornerCenterY - tagTextSize.y * 0.5f + 0.5f)},
                                theme::kTextDim,
                                skippedLabel.c_str()
                            );
                        }
                    }
                }
            }

            // 2b) Clip cut seams and gaps: hit-test during marquee box selection and highlight selected cuts.
            if (state.totalTicks > 0) {
                auto const cuts = state::editing::ClipOps::collectCameraClipCuts(camera, state.totalTicks);
                for (auto const& cut : cuts) {
                    bool inBox = false;
                    if (mBoxSelecting && !camera.locked && blockBot >= boxMin.y && blockTop <= boxMax.y) {
                        if (cut.isSeam()) {
                            float const seamX = scale.xAt(static_cast<float>(cut.startTick));
                            inBox = seamX + 4.0f * uiScale >= boxMin.x && seamX - 4.0f * uiScale <= boxMax.x;
                        } else if (cut.isGap()) {
                            float const gx0 = scale.xAt(static_cast<float>(cut.startTick));
                            float const gx1 = scale.xAt(static_cast<float>(cut.endTick));
                            inBox           = gx1 >= boxMin.x && gx0 <= boxMax.x;
                        }
                    }
                    if (inBox) cutBoxHits.push_back(cut);
                    if (inBox || ctx.selection.isClipCutSelected(camera.id, cut.startTick, cut.endTick)) {
                        if (cut.isSeam()) {
                            float const seamX = scale.xAt(static_cast<float>(cut.startTick));
                            if (seamX >= layout.canvasLeft - 6.0f * uiScale
                                && seamX <= layout.fullMax.x + 6.0f * uiScale) {
                                drawList->AddLine(
                                    {seamX, blockTop - 1.0f},
                                    {seamX, blockBot + 1.0f},
                                    theme::kSelected,
                                    2.5f * uiScale
                                );
                                drawKeyframeDiamond(
                                    drawList,
                                    seamX,
                                    (blockTop + blockBot) * 0.5f,
                                    4.0f * uiScale,
                                    theme::kSelected,
                                    theme::kIconHighlight
                                );
                            }
                        } else if (cut.isGap()) {
                            float const gx0 = std::max(layout.canvasLeft, scale.xAt(static_cast<float>(cut.startTick)));
                            float const gx1 = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(cut.endTick)));
                            if (gx1 > gx0) {
                                float const gapRounding = 4.0f * uiScale;
                                drawList->AddRectFilled(
                                    {gx0, blockTop},
                                    {gx1, blockBot},
                                    theme::withAlpha(theme::kSelected, 0x38),
                                    gapRounding
                                );
                                drawList->AddRect(
                                    {gx0, blockTop},
                                    {gx1, blockBot},
                                    theme::kSelected,
                                    gapRounding,
                                    0,
                                    1.75f
                                );
                            }
                        }
                    }
                }
            }

            // 3) 50% centered keyframe baseline & interpolation spans strictly clipped inside clips.
            {
                bool const  rowHoveredForKeyAdd = mouseInRow && !camera.locked && (penMode || ImGui::GetIO().KeyCtrl);
                ImU32 const baselineColor       = IM_COL32(255, 255, 255, rowHoveredForKeyAdd ? 0x6e : 0x2a);
                for (auto const& clip : camera.clips) {
                    bool const  draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                    float const x0           = std::max(
                        layout.canvasLeft,
                        scale.xAt(static_cast<float>(draggingThis ? mClipDragInTick : clip.inTick))
                    );
                    float const x1 = std::min(
                        layout.fullMax.x,
                        scale.xAt(static_cast<float>(draggingThis ? mClipDragOutTick : clip.outTick))
                    );
                    if (x1 - x0 > 10.0f) {
                        drawList->AddLine({x0 + 5.0f, keyCenterY}, {x1 - 5.0f, keyCenterY}, baselineColor, 1.0f);
                    }
                }

                using Mode = state::editing::model::CameraKeyframeMode;
                for (auto it = camera.keysByTick.begin(); it != camera.keysByTick.end(); ++it) {
                    int const  t0      = it->first;
                    auto const nextIt  = std::next(it);
                    bool const hasNext = nextIt != camera.keysByTick.end();

                    ImU32 spanColor = trackColor;
                    int   spanEnd   = state.totalTicks;
                    bool  heldSpan  = false;

                    if (it->second.mode == Mode::Interpolated) {
                        int const dimEnd    = intervalEndForTick(t0);
                        spanEnd             = hasNext ? std::min(nextIt->first, dimEnd) : dimEnd;
                        bool const nextSame = hasNext && nextIt->second.mode == Mode::Interpolated
                                           && intervalEndForTick(nextIt->first) == dimEnd;
                        heldSpan = !nextSame
                                || it->second.interpolationType == state::editing::model::CameraInterpolationType::Hold;
                    } else if (it->second.mode == Mode::ThirdPerson && it->second.thirdPersonCustom) {
                        int const dimEnd    = intervalEndForTick(t0);
                        spanEnd             = hasNext ? nextIt->first : state.totalTicks;
                        spanColor           = theme::kKeyframeThirdPerson;
                        bool const nextSame = hasNext && nextIt->second.mode == Mode::ThirdPerson
                                           && nextIt->second.thirdPersonCustom
                                           && nextIt->second.subject == it->second.subject
                                           && nextIt->second.thirdPersonFollowRotation
                                                  == it->second.thirdPersonFollowRotation
                                           && intervalEndForTick(nextIt->first) == dimEnd;
                        heldSpan = !nextSame
                                || it->second.interpolationType == state::editing::model::CameraInterpolationType::Hold;
                    } else {
                        spanEnd   = hasNext ? nextIt->first : state.totalTicks;
                        spanColor = it->second.mode == Mode::FirstPerson ? theme::kKeyframeFirstPerson
                                                                         : theme::kKeyframeThirdPerson;
                        heldSpan  = it->second.mode == Mode::ThirdPerson;
                    }
                    if (spanEnd <= t0) continue;

                    float const rawSpanX0 = scale.xAt(static_cast<float>(t0));
                    float const rawSpanX1 = scale.xAt(static_cast<float>(spanEnd));
                    ImU32 const spanLineColor =
                        theme::withAlpha(spanColor, !camera.enabled ? 0x48 : (heldSpan ? 0x7c : 0xe0));
                    float const lineThick = heldSpan ? 1.25f : 2.0f;

                    for (auto const& clip : camera.clips) {
                        bool const  draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                        float const clipX0 =
                            scale.xAt(static_cast<float>(draggingThis ? mClipDragInTick : clip.inTick));
                        float const clipX1 =
                            scale.xAt(static_cast<float>(draggingThis ? mClipDragOutTick : clip.outTick));
                        float const segX0 = std::max({layout.canvasLeft, rawSpanX0, clipX0 + 4.0f});
                        float const segX1 = std::min({layout.fullMax.x, rawSpanX1, clipX1 - 4.0f});
                        if (segX1 <= segX0) continue;

                        drawList->AddLine({segX0, keyCenterY}, {segX1, keyCenterY}, spanLineColor, lineThick);
                    }
                }
            }

            // 4) Keyframe diamonds / squares / circles & below-keyframe perspective labels topmost on row.
            {
                struct BelowKeyLabel {
                    int         tick;
                    float       x0;
                    float       x1;
                    char const* text;
                    ImU32       color;
                    int         priority;
                    bool        lit;
                    bool        inActiveClip;
                };
                std::vector<BelowKeyLabel> belowLabels;
                ImFont* const              belowFont = ImGui::GetFont();
                using Mode                           = state::editing::model::CameraKeyframeMode;

                for (auto it = camera.keysByTick.begin(); it != camera.keysByTick.end(); ++it) {
                    int const   keyTick   = it->first;
                    auto const& key       = it->second;
                    bool const  selected  = ctx.selection.isKeyframeSelected(camera.id, keyTick);
                    int const   drawnTick = drawnTickForKey(keyTick, selected);
                    float const x         = scale.xAt(static_cast<float>(drawnTick));
                    if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                    bool const inBox = mBoxSelecting && !camera.locked && boxContains(x, keyCenterY);
                    if (inBox) boxHits.push_back({camera.id, keyTick});
                    bool const hold = key.interpolationType == state::editing::model::CameraInterpolationType::Hold;

                    bool const  lit       = selected || inBox;
                    float const radius    = lit ? keyRadius + 1.0f : keyRadius;
                    ImU32 const modeColor = key.mode == Mode::FirstPerson ? theme::kKeyframeFirstPerson
                                          : key.mode == Mode::ThirdPerson ? theme::kKeyframeThirdPerson
                                                                          : theme::kKeyframe;
                    bool const  inActiveClip =
                        state::editing::ClipOps::cameraHasActiveClipAt(camera, drawnTick, state.totalTicks, false);
                    ImU32 const baseColor = !camera.enabled ? theme::kKeyframeDisabled : modeColor;
                    ImU32       fill      = lit ? theme::kKeyframeSelected : baseColor;
                    ImU32       outline   = lit ? theme::kIconHighlight : theme::kKeyframeOutline;
                    if (!inActiveClip && !lit) {
                        fill    = theme::withAlpha(baseColor, 0x36);
                        outline = theme::withAlpha(baseColor, 0x8c);
                    }
                    if (key.mode == Mode::Interpolated) {
                        if (hold) drawKeyframeSquare(drawList, x, keyCenterY, radius, fill, outline);
                        else drawKeyframeDiamond(drawList, x, keyCenterY, radius, fill, outline);
                    } else {
                        drawKeyframeCircle(
                            drawList,
                            x,
                            keyCenterY,
                            radius,
                            fill,
                            outline,
                            key.mode == Mode::ThirdPerson,
                            key.mode == Mode::ThirdPerson && key.thirdPersonCustom,
                            hold
                        );
                    }

                    char const* belowText     = nullptr;
                    ImU32       belowColor    = theme::kText;
                    int         labelPriority = 0;
                    if (key.mode == Mode::FirstPerson) {
                        belowText     = "1P";
                        belowColor    = theme::kKeyframeFirstPerson;
                        labelPriority = 2;
                    } else if (key.mode == Mode::ThirdPerson) {
                        belowText     = key.thirdPersonCustom ? "3P+" : "3P";
                        belowColor    = theme::kKeyframeThirdPerson;
                        labelPriority = 2;
                    } else if (key.lookAtEnabled) {
                        bool const samePrevAim = it != camera.keysByTick.begin()
                                              && std::prev(it)->second.mode == Mode::Interpolated
                                              && std::prev(it)->second.lookAtEnabled
                                              && std::prev(it)->second.lookAtTarget == key.lookAtTarget;
                        if (!samePrevAim) {
                            belowText     = "Aim";
                            belowColor    = theme::kKeyframe;
                            labelPriority = 2;
                        }
                    } else if (it != camera.keysByTick.begin() && std::prev(it)->second.mode != Mode::Interpolated) {
                        belowText     = "Free";
                        belowColor    = theme::kKeyframe;
                        labelPriority = 1;
                    }

                    if (belowText != nullptr) {
                        if (lit || (hoveredKeyTick && *hoveredKeyTick == keyTick)) {
                            labelPriority = 3;
                        }
                        ImVec2 const sz       = belowFont->CalcTextSizeA(belowFontSize, FLT_MAX, 0.0f, belowText);
                        float        lx0      = std::round(x - sz.x * 0.5f);
                        bool         fitsClip = true;
                        for (auto const& clip : camera.clips) {
                            bool const draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                            int const  drawnIn      = draggingThis ? mClipDragInTick : clip.inTick;
                            int const  drawnOut     = draggingThis ? mClipDragOutTick : clip.outTick;
                            if (drawnIn <= drawnTick && drawnTick <= drawnOut) {
                                float const cx0 = scale.xAt(static_cast<float>(drawnIn));
                                float const cx1 = scale.xAt(static_cast<float>(drawnOut));
                                float const pad = 6.0f * uiScale;
                                if ((cx1 - cx0) < sz.x + pad * 2.0f) {
                                    fitsClip = false;
                                } else {
                                    lx0 = std::clamp(lx0, cx0 + pad, cx1 - pad - sz.x);
                                }
                                break;
                            }
                        }
                        float const lx1 = lx0 + sz.x;
                        if (fitsClip && lx0 >= layout.canvasLeft + 2.0f && lx1 <= layout.fullMax.x - 2.0f
                            && std::abs((lx0 + lx1) * 0.5f - x) <= sz.x + 6.0f * uiScale) {
                            belowLabels.push_back(
                                {keyTick, lx0, lx1, belowText, belowColor, labelPriority, lit, inActiveClip}
                            );
                        }
                    }
                }

                std::ranges::sort(belowLabels, [](BelowKeyLabel const& a, BelowKeyLabel const& b) {
                    return a.priority != b.priority ? a.priority > b.priority : a.tick < b.tick;
                });
                float const minGap     = 4.0f * uiScale;
                float const belowTextY = std::min(
                    blockBot - belowFontSize - 1.75f * uiScale,
                    std::floor(keyCenterY + keyRadius + 0.5f * uiScale)
                );
                std::vector<std::pair<float, float>> placed;
                placed.reserve(belowLabels.size());
                for (auto const& lbl : belowLabels) {
                    bool collides = false;
                    for (auto const& [px0, px1] : placed) {
                        if (!(lbl.x1 + minGap <= px0 || lbl.x0 >= px1 + minGap)) {
                            collides = true;
                            break;
                        }
                    }
                    if (collides) continue;
                    placed.emplace_back(lbl.x0, lbl.x1);
                    ImU32 const shadowCol = IM_COL32(0x10, 0x12, 0x18, (lbl.inActiveClip || lbl.lit) ? 0xd8 : 0x60);
                    ImU32 const textCol =
                        lbl.lit
                            ? theme::kKeyframeSelected
                            : (!camera.enabled ? theme::kTextDim
                                               : (lbl.inActiveClip ? lbl.color : theme::withAlpha(lbl.color, 0x68)));
                    drawList
                        ->AddText(belowFont, belowFontSize, {lbl.x0 + 0.75f, belowTextY + 0.75f}, shadowCol, lbl.text);
                    drawList->AddText(belowFont, belowFontSize, {lbl.x0, belowTextY}, textCol, lbl.text);
                }
            }

            // Priority 2: Razor tool cuts unlocked clips across the full row height.
            if (mouseInRow && !overAnyKeyOnRow && mTool == TimelineTool::Razor && hoveredClipPtr && !camera.locked
                && !hoveredClipPtr->locked && hoveredClipPtr->outTick - hoveredClipPtr->inTick >= 2) {
                int cutTick = scale.tickAt(mouse.x);
                if (shiftSnap) {
                    cutTick =
                        snapToEditPoints(ctx, cutTick, scale.pixelsPerTick, true, true, {}, {}, hoveredClipPtr->id);
                } else if (mSnapEnabled) {
                    cutTick = snapTick(cutTick);
                }
                cutTick          = std::clamp(cutTick, hoveredClipPtr->inTick + 1, hoveredClipPtr->outTick - 1);
                float const cutX = scale.xAt(static_cast<float>(cutTick));
                drawList->AddLine({cutX, blockTop}, {cutX, blockBot}, theme::kWarning, 2.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedClip{hoveredClipPtr->id});
                    EditorAction action{EditorActionType::SplitClip};
                    action.id   = hoveredClipPtr->id;
                    action.tick = cutTick;
                    ctx.submitAction(std::move(action));
                }
            }

            // Priority 3: Pen tool (click adds keyframe, drag marquee-selects) or Ctrl+Click on keyframe line in
            // Select.
            if (penPreviewTick.has_value()) {
                float const penX = scale.xAt(static_cast<float>(*penPreviewTick));
                drawKeyframeDiamond(
                    drawList,
                    penX,
                    keyCenterY,
                    keyRadius,
                    theme::withAlpha(theme::kKeyframe, 0x88),
                    theme::withAlpha(theme::kKeyframeOutline, 0xa0)
                );
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (penMode) {
                        startBoxPending(camera.id, true, *penPreviewTick);
                    } else {
                        EditorAction action{EditorActionType::AddCameraKeyframe};
                        action.id   = camera.id;
                        action.tick = *penPreviewTick;
                        ctx.submitAction(std::move(action));
                        queueCameraKeyframeSelection(camera.id, *penPreviewTick);
                        ctx.commands.showSelectionInspector();
                    }
                }
            } else if (mouseInRow && !overAnyKeyOnRow && penMode && !clickConsumed
                       && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                clickConsumed = true;
                startBoxPending(camera.id, false, -1);
            }

            // Priority 4: Alt+Left-Drag in Select mode marquee-selects keyframes right over clips.
            if (mouseInRow && !overAnyKeyOnRow && !clickConsumed && altBoxSelect
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                clickConsumed = true;
                startBoxPending(camera.id, false, -1);
            }

            // Clip right-click works across tools when not over a keyframe.
            if (hoveredClipPtr && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                clickConsumed = true;
                ctx.selection.select(state::editing::model::SelectedClip{hoveredClipPtr->id});
                mClipMenuClipId = hoveredClipPtr->id;
                mClipMenuTick   = std::clamp(scale.tickAt(mouse.x), hoveredClipPtr->inTick, hoveredClipPtr->outTick);
                mClipRenameId.clear();
                mFocusClipRename = false;
                widgets::openAnchoredPopup(
                    "##timeline-clip-menu",
                    {hoveredClipX0, blockTop},
                    {hoveredClipX1, blockBot}
                );
            }

            // BoxSelect tool: left-drag anywhere on the camera row starts marquee selection.
            if (mouseInRow && !clickConsumed && mTool == TimelineTool::BoxSelect
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                clickConsumed = true;
                startBoxPending(camera.id, false, -1);
                if (hoveredKeyTick.has_value()) mBoxClickKeyTick = *hoveredKeyTick;
            }

            // Priority 5 & 6: inline export eye, trim edges, and clip body in Select mode.
            if (hoveredClipPtr && mTool == TimelineTool::Select && !overAnyKeyOnRow && !penPreviewTick.has_value()
                && !altBoxSelect) {
                if (hoveredClipEye && !camera.locked) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        clickConsumed = true;
                        EditorAction action{EditorActionType::SetClipExportEnabled};
                        action.id    = hoveredClipPtr->id;
                        action.value = !hoveredClipPtr->exportEnabled;
                        ctx.submitAction(std::move(action));
                    }
                } else {
                    bool const clipEditable = !camera.locked && !hoveredClipPtr->locked;
                    bool       canSlideClip = false;
                    if (clipEditable && !hoveredClipLeftEdge && !hoveredClipRightEdge) {
                        auto const [minIn, maxOut] = clipBoundsOnCamera(camera, *hoveredClipPtr, state.totalTicks);
                        canSlideClip               = hoveredClipPtr->inTick > minIn || hoveredClipPtr->outTick < maxOut;
                    }
                    if (clipEditable && mClipDragMode == 0
                        && (hoveredClipLeftEdge || hoveredClipRightEdge || canSlideClip)) {
                        ImGui::SetMouseCursor(
                            (hoveredClipLeftEdge || hoveredClipRightEdge) ? ImGuiMouseCursor_ResizeEW
                                                                          : theme::kCursorGrab
                        );
                    }
                    if (!clickConsumed && !hoveredClipLeftEdge && !hoveredClipRightEdge
                        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        clickConsumed = true;
                        mClipDragMode = 0;
                        mClipDragCameraId.clear();
                        mClipDragId.clear();
                        mClipDragMoved = false;
                        ctx.selection.select(state::editing::model::SelectedClip{hoveredClipPtr->id});
                        mClipMenuClipId = hoveredClipPtr->id;
                        mClipMenuTick =
                            std::clamp(scale.tickAt(mouse.x), hoveredClipPtr->inTick, hoveredClipPtr->outTick);
                        mClipRenameId.clear();
                        mFocusClipRename = true;
                        widgets::openAnchoredPopup(
                            "##timeline-clip-menu",
                            {hoveredClipX0, blockTop},
                            {hoveredClipX1, blockBot}
                        );
                    } else if (!clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        clickConsumed = true;
                        ctx.selection.select(state::editing::model::SelectedClip{hoveredClipPtr->id});
                        if (ctx.commands.isAutoPreviewEnabled()) {
                            EditorAction previewAction{EditorActionType::SetPreviewCamera};
                            previewAction.id = camera.id;
                            ctx.submitAction(std::move(previewAction));
                        }
                        if (clipEditable) {
                            if (hoveredClipLeftEdge || hoveredClipRightEdge || canSlideClip) {
                                mClipDragMode        = hoveredClipLeftEdge ? 1 : (hoveredClipRightEdge ? 2 : 3);
                                mClipDragCameraId    = camera.id;
                                mClipDragId          = hoveredClipPtr->id;
                                mClipDragStartMouseX = mouse.x;
                                mClipDragOrigInTick  = hoveredClipPtr->inTick;
                                mClipDragOrigOutTick = hoveredClipPtr->outTick;
                                mClipDragInTick      = hoveredClipPtr->inTick;
                                mClipDragOutTick     = hoveredClipPtr->outTick;
                                mClipDragMoved       = false;
                            } else {
                                startBoxPending(camera.id, false, -1);
                                mBoxClickClipId = hoveredClipPtr->id;
                            }
                        } else {
                            submitSeek(
                                ctx,
                                std::clamp(scale.tickAt(mouse.x), hoveredClipPtr->inTick, hoveredClipPtr->outTick)
                            );
                        }
                    }
                }
            }

            // Priority 7: empty gap on row (outside any clip).
            if (mouseInRow && !overAnyKeyOnRow && !clickConsumed) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    int tick      = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                    if (mSnapEnabled) {
                        int const  snapped = snapTick(tick);
                        bool const inClip  = std::ranges::any_of(camera.clips, [&](auto const& c) {
                            return c.inTick <= snapped && snapped < c.outTick;
                        });
                        if (!inClip) tick = snapped;
                    }
                    mLaneMenuCameraId   = camera.id;
                    mLaneMenuTick       = tick;
                    float const anchorX = scale.xAt(static_cast<float>(tick));
                    widgets::openAnchoredPopup("##timeline-lane-menu", {anchorX, blockTop}, {anchorX, blockBot});
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    if (mTool == TimelineTool::Select) {
                        startBoxPending(camera.id, false, -1);
                    } else {
                        ctx.selection.select(state::editing::model::SelectedCamera{camera.id});
                        submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
                    }
                }
            }
        }
        y = rowBottom;
    }
    if (state.totalTicks > 0 && layout.bodyBottom > layout.camerasTop) {
        std::vector<std::pair<int, int>> activeIntervals;
        for (auto const& camera : project->cameras) {
            if (!camera.enabled) continue;
            for (auto const& clip : camera.clips) {
                if (!clip.exportEnabled) continue;
                bool const draggingThis = mClipDragMode != 0 && mClipDragId == clip.id;
                int const  cIn          = std::clamp(draggingThis ? mClipDragInTick : clip.inTick, 0, state.totalTicks);
                int const  cOut = std::clamp(draggingThis ? mClipDragOutTick : clip.outTick, cIn, state.totalTicks);
                if (cIn < cOut) activeIntervals.emplace_back(cIn, cOut);
            }
        }
        std::ranges::sort(activeIntervals);

        ImU32 const overlayColor = IM_COL32(0, 0, 0, 0x18);
        auto const  shadeDim     = [&](int startTick, int endTick) {
            if (endTick <= startTick) return;
            float const x0 = std::max(layout.canvasLeft, scale.xAt(static_cast<float>(startTick)));
            float const x1 = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(endTick)));
            if (x1 > x0) {
                drawList->AddRectFilled({x0, layout.camerasTop}, {x1, layout.bodyBottom}, overlayColor);
            }
        };
        int cursor = 0;
        for (auto const& [cIn, cOut] : activeIntervals) {
            shadeDim(cursor, cIn);
            cursor = std::max(cursor, cOut);
        }
        shadeDim(cursor, state.totalTicks);
    }
    drawList->PopClipRect();

    drawTrackGrid(ctx, layout, scale);
    drawExportRange(ctx, layout, scale, allowInput);
    drawContextMenus(ctx);

    // Shift snaps a drag onto nearby edit points; otherwise the whole-second grid applies when enabled.
    auto const dragTarget = [&](std::function<bool(std::string const&, int)> const& skipKey) {
        int const raw = tickFromMouse();
        return shiftSnap ? snapToEditPoints(ctx, raw, scale.pixelsPerTick, true, true, skipKey, {}) : snapTick(raw);
    };

    if (allowInput && mGroupDrag) {
        if (ImGui::GetMousePos().x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
        auto const& keys = ctx.selection.keyframes();
        int         delta = 0;
        if (mDraggingKeyframeMoved && !keys.empty()) {
            int const target =
                dragTarget([&](std::string const& id, int tick) { return ctx.selection.isKeyframeSelected(id, tick); });
            delta = target - mDraggingKeyframeStartTick;
            // Clamp the whole set into the replay, then refuse deltas that would land on a key outside the set.
            int minTick = std::numeric_limits<int>::max();
            int maxTick = std::numeric_limits<int>::min();
            for (auto const& key : keys) {
                minTick = std::min(minTick, key.tick);
                maxTick = std::max(maxTick, key.tick);
            }
            delta = std::clamp(delta, -minTick, state.totalTicks - maxTick);
            bool const collides = std::ranges::any_of(keys, [&](auto const& key) {
                auto const camera =
                    std::ranges::find(project->cameras, key.trackId, &state::editing::model::CameraEntity::id);
                if (camera == project->cameras.end()) return false;
                int const moved = key.tick + delta;
                return camera->keysByTick.contains(moved) && !ctx.selection.isKeyframeSelected(key.trackId, moved);
            });
            if (collides) delta = mGroupDragDelta;
        }
        mGroupDragDelta     = delta;
        float const markerX = scale.xAt(static_cast<float>(mDraggingKeyframeStartTick + delta));
        drawList->AddLine(
            {markerX, layout.bodyTop},
            {markerX, layout.bodyBottom},
            theme::withAlpha(theme::kKeyframeSelected, 180),
            1.0f
        );
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved && delta != 0) {
                EditorAction action{EditorActionType::MoveCameraKeyframes};
                action.keyframeRefs = selectedKeyframeRefs(ctx);
                action.tick         = delta;
                ctx.submitAction(std::move(action));
                std::vector<state::editing::model::SelectedKeyframe> moved;
                for (auto const& key : keys) moved.push_back({key.trackId, key.tick + delta});
                queueKeyframeSelection(std::move(moved));
            } else if (!mDraggingKeyframeMoved) {
                ctx.selection.select(
                    state::editing::model::SelectedKeyframe{mDraggingKeyframeCameraId, mDraggingKeyframeStartTick}
                );
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mGroupDrag      = false;
            mGroupDragDelta = 0;
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    } else if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        // Any horizontal movement latches into a drag, so a plain click only seeks.
        if (ImGui::GetMousePos().x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
        int const candidateTick =
            mDraggingKeyframeMoved
                ? boundedKeyframeTick(dragTarget([&](std::string const& id, int tick) {
                      return id == mDraggingKeyframeCameraId && tick == mDraggingKeyframeStartTick;
                  }))
                : mDraggingKeyframeStartTick;
        mDraggingKeyframeTick = candidateTick;
        float const markerX   = scale.xAt(static_cast<float>(candidateTick));
        drawList->AddLine(
            {markerX, layout.bodyTop},
            {markerX, layout.bodyBottom},
            theme::withAlpha(theme::kKeyframeSelected, 180),
            1.0f
        );
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved) {
                if (candidateTick != mDraggingKeyframeStartTick) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = mDraggingKeyframeCameraId;
                    action.tick          = mDraggingKeyframeStartTick;
                    action.secondaryTick = candidateTick;
                    ctx.submitAction(std::move(action));
                    queueKeyframeSelection({{mDraggingKeyframeCameraId, candidateTick}});
                }
            } else {
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    } else if (allowInput && mDraggingSpeedKeyframeOriginTick >= 0) {
        auto const& keys  = project->speedTrack.keysByTick;
        auto const  keyIt = keys.find(mDraggingSpeedKeyframeOriginTick);
        if (keyIt == keys.end() || project->speedTrack.locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingSpeedKeyframeOriginTick = -1;
            mDraggingSpeedKeyframeTick       = -1;
            mDraggingSpeedKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingSpeedKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingSpeedKeyframeMoved = true;
            }
            int minimum = 0;
            int maximum = state.totalTicks;
            if (keyIt != keys.begin()) minimum = std::min(state.totalTicks, std::prev(keyIt)->first + 1);
            auto const next = std::next(keyIt);
            if (next != keys.end()) maximum = std::max(0, next->first - 1);

            int candidateTick = mDraggingSpeedKeyframeOriginTick;
            if (mDraggingSpeedKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap ? snapToEditPoints(
                                                   ctx,
                                                   raw,
                                                   scale.pixelsPerTick,
                                                   true,
                                                   true,
                                                   {},
                                                   {},
                                                   {},
                                                   mDraggingSpeedKeyframeOriginTick
                                               )
                                             : snapTick(raw);
                if (minimum <= maximum) {
                    candidateTick = std::clamp(target, minimum, maximum);
                }
            }
            mDraggingSpeedKeyframeTick = candidateTick;
            float const markerX        = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingSpeedKeyframeMoved) {
                    if (candidateTick != mDraggingSpeedKeyframeOriginTick) {
                        EditorAction action{EditorActionType::MoveSpeedKeyframe};
                        action.tick          = mDraggingSpeedKeyframeOriginTick;
                        action.secondaryTick = candidateTick;
                        ctx.submitAction(std::move(action));
                        queueSpeedKeyframeSelection(candidateTick);
                    }
                } else {
                    submitSeek(ctx, mDraggingSpeedKeyframeOriginTick);
                }
                mDraggingSpeedKeyframeOriginTick = -1;
                mDraggingSpeedKeyframeTick       = -1;
                mDraggingSpeedKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mDraggingFreezeKeyframeOriginTick >= 0) {
        auto const& keys  = project->freezeTrack.keysByTick;
        auto const  keyIt = keys.find(mDraggingFreezeKeyframeOriginTick);
        if (keyIt == keys.end() || project->freezeTrack.locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingFreezeKeyframeOriginTick = -1;
            mDraggingFreezeKeyframeTick       = -1;
            mDraggingFreezeKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingFreezeKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingFreezeKeyframeMoved = true;
            }
            int minimum = 0;
            int maximum = state.totalTicks;
            if (keyIt != keys.begin()) minimum = std::min(state.totalTicks, std::prev(keyIt)->first + 1);
            auto const next = std::next(keyIt);
            if (next != keys.end()) maximum = std::max(0, next->first - 1);

            int candidateTick = mDraggingFreezeKeyframeOriginTick;
            if (mDraggingFreezeKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap ? snapToEditPoints(
                                                   ctx,
                                                   raw,
                                                   scale.pixelsPerTick,
                                                   true,
                                                   true,
                                                   {},
                                                   {},
                                                   {},
                                                   std::nullopt,
                                                   mDraggingFreezeKeyframeOriginTick
                                               )
                                             : snapTick(raw);
                if (minimum <= maximum) {
                    candidateTick = std::clamp(target, minimum, maximum);
                }
            }
            mDraggingFreezeKeyframeTick = candidateTick;
            float const markerX         = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingFreezeKeyframeMoved) {
                    if (candidateTick != mDraggingFreezeKeyframeOriginTick) {
                        EditorAction action{EditorActionType::MoveFreezeKeyframe};
                        action.tick          = mDraggingFreezeKeyframeOriginTick;
                        action.secondaryTick = candidateTick;
                        ctx.submitAction(std::move(action));
                        queueFreezeKeyframeSelection(candidateTick);
                    }
                } else {
                    submitSeek(ctx, mDraggingFreezeKeyframeOriginTick);
                }
                mDraggingFreezeKeyframeOriginTick = -1;
                mDraggingFreezeKeyframeTick       = -1;
                mDraggingFreezeKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mDraggingTimelapseKeyframeOriginTick >= 0) {
        auto const& keys  = project->timelapseTrack.keysByTick;
        auto const  keyIt = keys.find(mDraggingTimelapseKeyframeOriginTick);
        if (keyIt == keys.end() || project->timelapseTrack.locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingTimelapseKeyframeOriginTick = -1;
            mDraggingTimelapseKeyframeTick       = -1;
            mDraggingTimelapseKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingTimelapseKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingTimelapseKeyframeMoved = true;
            }
            int minimum = 0;
            int maximum = state.totalTicks;
            if (keyIt != keys.begin()) minimum = std::min(state.totalTicks, std::prev(keyIt)->first + 1);
            auto const next = std::next(keyIt);
            if (next != keys.end()) maximum = std::max(0, next->first - 1);

            int candidateTick = mDraggingTimelapseKeyframeOriginTick;
            if (mDraggingTimelapseKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap ? snapToEditPoints(
                                                   ctx,
                                                   raw,
                                                   scale.pixelsPerTick,
                                                   true,
                                                   true,
                                                   {},
                                                   {},
                                                   {},
                                                   std::nullopt,
                                                   std::nullopt,
                                                   mDraggingTimelapseKeyframeOriginTick
                                               )
                                             : snapTick(raw);
                if (minimum <= maximum) {
                    candidateTick = std::clamp(target, minimum, maximum);
                }
            }
            mDraggingTimelapseKeyframeTick = candidateTick;
            float const markerX            = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingTimelapseKeyframeMoved) {
                    if (candidateTick != mDraggingTimelapseKeyframeOriginTick) {
                        EditorAction action{EditorActionType::MoveTimelapseKeyframe};
                        action.tick          = mDraggingTimelapseKeyframeOriginTick;
                        action.secondaryTick = candidateTick;
                        ctx.submitAction(std::move(action));
                        queueTimelapseKeyframeSelection(candidateTick);
                    }
                } else {
                    submitSeek(ctx, mDraggingTimelapseKeyframeOriginTick);
                }
                mDraggingTimelapseKeyframeOriginTick = -1;
                mDraggingTimelapseKeyframeTick       = -1;
                mDraggingTimelapseKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mTimeOfDayGroupDrag) {
        auto const& keys  = project->timeOfDayTrack.keysByTick;
        auto const  ticks = ctx.selection.selectedTimeOfDayKeyframes();
        if (project->timeOfDayTrack.locked || ticks.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mTimeOfDayGroupDrag                  = false;
            mTimeOfDayGroupDragDelta             = 0;
            mDraggingTimeOfDayKeyframeOriginTick = -1;
            mDraggingTimeOfDayKeyframeTick       = -1;
            mDraggingTimeOfDayKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingTimeOfDayKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingTimeOfDayKeyframeMoved = true;
            }
            int delta = 0;
            if (mDraggingTimeOfDayKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap
                                     ? snapToEditPoints(
                                           ctx,
                                           raw,
                                           scale.pixelsPerTick,
                                           true,
                                           true,
                                           {},
                                           {},
                                           {},
                                           std::nullopt,
                                           std::nullopt,
                                           std::nullopt,
                                           {},
                                           [&](int tick) { return ctx.selection.isTimeOfDayKeyframeSelected(tick); }
                                       )
                                     : snapTick(raw);
                delta            = target - mDraggingTimeOfDayKeyframeOriginTick;
                int minTick      = std::numeric_limits<int>::max();
                int maxTick      = std::numeric_limits<int>::min();
                for (int const tick : ticks) {
                    minTick = std::min(minTick, tick);
                    maxTick = std::max(maxTick, tick);
                }
                delta               = std::clamp(delta, -minTick, state.totalTicks - maxTick);
                bool const collides = std::ranges::any_of(ticks, [&](int tick) {
                    int const moved = tick + delta;
                    return keys.contains(moved) && !ctx.selection.isTimeOfDayKeyframeSelected(moved);
                });
                if (collides) delta = mTimeOfDayGroupDragDelta;
            }
            mTimeOfDayGroupDragDelta = delta;
            float const markerX      = scale.xAt(static_cast<float>(mDraggingTimeOfDayKeyframeOriginTick + delta));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingTimeOfDayKeyframeMoved && delta != 0) {
                    EditorAction action{EditorActionType::MoveTimeOfDayKeyframes};
                    action.ticks         = ticks;
                    action.secondaryTick = delta;
                    ctx.submitAction(std::move(action));
                    std::vector<int> moved;
                    moved.reserve(ticks.size());
                    for (int const tick : ticks) moved.push_back(tick + delta);
                    queueTimeOfDayKeyframesSelection(std::move(moved));
                } else if (!mDraggingTimeOfDayKeyframeMoved) {
                    ctx.selection.selectTimeOfDayKeyframe(mDraggingTimeOfDayKeyframeOriginTick);
                    submitSeek(ctx, mDraggingTimeOfDayKeyframeOriginTick);
                }
                mTimeOfDayGroupDrag                  = false;
                mTimeOfDayGroupDragDelta             = 0;
                mDraggingTimeOfDayKeyframeOriginTick = -1;
                mDraggingTimeOfDayKeyframeTick       = -1;
                mDraggingTimeOfDayKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mDraggingTimeOfDayKeyframeOriginTick >= 0) {
        auto const& keys  = project->timeOfDayTrack.keysByTick;
        auto const  keyIt = keys.find(mDraggingTimeOfDayKeyframeOriginTick);
        if (keyIt == keys.end() || project->timeOfDayTrack.locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingTimeOfDayKeyframeOriginTick = -1;
            mDraggingTimeOfDayKeyframeTick       = -1;
            mDraggingTimeOfDayKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingTimeOfDayKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingTimeOfDayKeyframeMoved = true;
            }
            int minimum = 0;
            int maximum = state.totalTicks;
            if (keyIt != keys.begin()) minimum = std::min(state.totalTicks, std::prev(keyIt)->first + 1);
            auto const next = std::next(keyIt);
            if (next != keys.end()) maximum = std::max(0, next->first - 1);

            int candidateTick = mDraggingTimeOfDayKeyframeOriginTick;
            if (mDraggingTimeOfDayKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap
                                     ? snapToEditPoints(
                                           ctx,
                                           raw,
                                           scale.pixelsPerTick,
                                           true,
                                           true,
                                           {},
                                           {},
                                           {},
                                           std::nullopt,
                                           std::nullopt,
                                           std::nullopt,
                                           {},
                                           [&](int tick) { return tick == mDraggingTimeOfDayKeyframeOriginTick; }
                                       )
                                     : snapTick(raw);
                if (minimum <= maximum) {
                    candidateTick = std::clamp(target, minimum, maximum);
                }
            }
            mDraggingTimeOfDayKeyframeTick = candidateTick;
            float const markerX            = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingTimeOfDayKeyframeMoved) {
                    if (candidateTick != mDraggingTimeOfDayKeyframeOriginTick) {
                        EditorAction action{EditorActionType::MoveTimeOfDayKeyframe};
                        action.tick          = mDraggingTimeOfDayKeyframeOriginTick;
                        action.secondaryTick = candidateTick;
                        ctx.submitAction(std::move(action));
                        queueTimeOfDayKeyframeSelection(candidateTick);
                    }
                } else {
                    submitSeek(ctx, mDraggingTimeOfDayKeyframeOriginTick);
                }
                mDraggingTimeOfDayKeyframeOriginTick = -1;
                mDraggingTimeOfDayKeyframeTick       = -1;
                mDraggingTimeOfDayKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mWeatherGroupDrag) {
        auto const& keys  = project->weatherTrack.keysByTick;
        auto const  ticks = ctx.selection.selectedWeatherKeyframes();
        if (project->weatherTrack.locked || ticks.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mWeatherGroupDrag                  = false;
            mWeatherGroupDragDelta             = 0;
            mDraggingWeatherKeyframeOriginTick = -1;
            mDraggingWeatherKeyframeTick       = -1;
            mDraggingWeatherKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingWeatherKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingWeatherKeyframeMoved = true;
            }
            int delta = 0;
            if (mDraggingWeatherKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap
                                     ? snapToEditPoints(
                                           ctx,
                                           raw,
                                           scale.pixelsPerTick,
                                           true,
                                           true,
                                           {},
                                           {},
                                           {},
                                           std::nullopt,
                                           std::nullopt,
                                           std::nullopt,
                                           {},
                                           {},
                                           [&](int tick) { return ctx.selection.isWeatherKeyframeSelected(tick); }
                                       )
                                     : snapTick(raw);
                delta            = target - mDraggingWeatherKeyframeOriginTick;
                int minTick      = std::numeric_limits<int>::max();
                int maxTick      = std::numeric_limits<int>::min();
                for (int const tick : ticks) {
                    minTick = std::min(minTick, tick);
                    maxTick = std::max(maxTick, tick);
                }
                delta               = std::clamp(delta, -minTick, state.totalTicks - maxTick);
                bool const collides = std::ranges::any_of(ticks, [&](int tick) {
                    int const moved = tick + delta;
                    return keys.contains(moved) && !ctx.selection.isWeatherKeyframeSelected(moved);
                });
                if (collides) delta = mWeatherGroupDragDelta;
            }
            mWeatherGroupDragDelta = delta;
            float const markerX    = scale.xAt(static_cast<float>(mDraggingWeatherKeyframeOriginTick + delta));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingWeatherKeyframeMoved && delta != 0) {
                    EditorAction action{EditorActionType::MoveWeatherKeyframes};
                    action.ticks         = ticks;
                    action.secondaryTick = delta;
                    ctx.submitAction(std::move(action));
                    std::vector<int> moved;
                    moved.reserve(ticks.size());
                    for (int const tick : ticks) moved.push_back(tick + delta);
                    queueWeatherKeyframesSelection(std::move(moved));
                } else if (!mDraggingWeatherKeyframeMoved) {
                    ctx.selection.selectWeatherKeyframe(mDraggingWeatherKeyframeOriginTick);
                    submitSeek(ctx, mDraggingWeatherKeyframeOriginTick);
                }
                mWeatherGroupDrag                  = false;
                mWeatherGroupDragDelta             = 0;
                mDraggingWeatherKeyframeOriginTick = -1;
                mDraggingWeatherKeyframeTick       = -1;
                mDraggingWeatherKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mDraggingWeatherKeyframeOriginTick >= 0) {
        auto const& keys  = project->weatherTrack.keysByTick;
        auto const  keyIt = keys.find(mDraggingWeatherKeyframeOriginTick);
        if (keyIt == keys.end() || project->weatherTrack.locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingWeatherKeyframeOriginTick = -1;
            mDraggingWeatherKeyframeTick       = -1;
            mDraggingWeatherKeyframeMoved      = false;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (std::abs(ImGui::GetMousePos().x - mDraggingWeatherKeyframeStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingWeatherKeyframeMoved = true;
            }
            int minimum = 0;
            int maximum = state.totalTicks;
            if (keyIt != keys.begin()) minimum = std::min(state.totalTicks, std::prev(keyIt)->first + 1);
            auto const next = std::next(keyIt);
            if (next != keys.end()) maximum = std::max(0, next->first - 1);

            int candidateTick = mDraggingWeatherKeyframeOriginTick;
            if (mDraggingWeatherKeyframeMoved) {
                int const raw    = tickFromMouse();
                int const target = shiftSnap ? snapToEditPoints(
                                                   ctx,
                                                   raw,
                                                   scale.pixelsPerTick,
                                                   true,
                                                   true,
                                                   {},
                                                   {},
                                                   {},
                                                   std::nullopt,
                                                   std::nullopt,
                                                   std::nullopt,
                                                   {},
                                                   {},
                                                   [&](int tick) { return tick == mDraggingWeatherKeyframeOriginTick; }
                                               )
                                             : snapTick(raw);
                if (minimum <= maximum) {
                    candidateTick = std::clamp(target, minimum, maximum);
                }
            }
            mDraggingWeatherKeyframeTick = candidateTick;
            float const markerX          = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingWeatherKeyframeMoved) {
                    if (candidateTick != mDraggingWeatherKeyframeOriginTick) {
                        EditorAction action{EditorActionType::MoveWeatherKeyframe};
                        action.tick          = mDraggingWeatherKeyframeOriginTick;
                        action.secondaryTick = candidateTick;
                        ctx.submitAction(std::move(action));
                        queueWeatherKeyframeSelection(candidateTick);
                    }
                } else {
                    submitSeek(ctx, mDraggingWeatherKeyframeOriginTick);
                }
                mDraggingWeatherKeyframeOriginTick = -1;
                mDraggingWeatherKeyframeTick       = -1;
                mDraggingWeatherKeyframeMoved      = false;
            }
        }
    } else if (allowInput && mClipDragMode != 0) {
        auto const hit = state::editing::ClipOps::findClipWithCamera(*project, mClipDragId);
        if (!hit || hit.camera->locked || hit.clip->locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mClipDragMode = 0;
            mClipDragCameraId.clear();
            mClipDragId.clear();
            mClipDragMoved = false;
        } else {
            auto const* clip = hit.clip;
            if (std::abs(ImGui::GetMousePos().x - mClipDragStartMouseX) >= 2.0f * metrics::scale()) {
                mClipDragMoved = true;
            }
            ImGui::SetMouseCursor(
                (mClipDragMode == 1 || mClipDragMode == 2) ? ImGuiMouseCursor_ResizeEW : theme::kCursorGrabbing
            );
            auto const [minIn, maxOut] = clipBoundsOnCamera(*hit.camera, *clip, state.totalTicks);

            if (mClipDragMoved) {
                int const rawDelta = static_cast<int>(
                    std::round((ImGui::GetMousePos().x - mClipDragStartMouseX) / std::max(0.0001f, scale.pixelsPerTick))
                );
                if (mClipDragMode == 1) {
                    int targetIn = mClipDragOrigInTick + rawDelta;
                    if (shiftSnap) {
                        targetIn = snapToEditPoints(ctx, targetIn, scale.pixelsPerTick, true, true, {}, {}, clip->id);
                    } else if (mSnapEnabled) {
                        targetIn = snapTick(targetIn);
                    }
                    mClipDragInTick  = std::clamp(targetIn, minIn, mClipDragOrigOutTick - 1);
                    mClipDragOutTick = mClipDragOrigOutTick;
                } else if (mClipDragMode == 2) {
                    int targetOut = mClipDragOrigOutTick + rawDelta;
                    if (shiftSnap) {
                        targetOut = snapToEditPoints(ctx, targetOut, scale.pixelsPerTick, true, true, {}, {}, clip->id);
                    } else if (mSnapEnabled) {
                        targetOut = snapTick(targetOut);
                    }
                    mClipDragInTick  = mClipDragOrigInTick;
                    mClipDragOutTick = std::clamp(targetOut, mClipDragOrigInTick + 1, maxOut);
                } else {
                    int const len      = std::max(1, mClipDragOrigOutTick - mClipDragOrigInTick);
                    int       targetIn = mClipDragOrigInTick + rawDelta;
                    if (shiftSnap) {
                        int const snappedIn =
                            snapToEditPoints(ctx, targetIn, scale.pixelsPerTick, true, true, {}, {}, clip->id);
                        int const guideAfterIn = mSnapGuideTick;
                        mSnapGuideTick         = -1;
                        int const snappedOutTick =
                            snapToEditPoints(ctx, targetIn + len, scale.pixelsPerTick, true, true, {}, {}, clip->id);
                        int const  guideAfterOut = mSnapGuideTick;
                        int const  snappedOut    = snappedOutTick - len;
                        bool const hasIn         = snappedIn != targetIn;
                        bool const hasOut        = snappedOut != targetIn;
                        if (hasIn && (!hasOut || std::abs(snappedIn - targetIn) <= std::abs(snappedOut - targetIn))) {
                            targetIn       = snappedIn;
                            mSnapGuideTick = guideAfterIn;
                        } else if (hasOut) {
                            targetIn       = snappedOut;
                            mSnapGuideTick = guideAfterOut;
                        } else {
                            mSnapGuideTick = -1;
                        }
                    } else if (mSnapEnabled) {
                        targetIn = snapTick(targetIn);
                    }
                    targetIn         = std::clamp(targetIn, minIn, std::max(minIn, maxOut - len));
                    mClipDragInTick  = targetIn;
                    mClipDragOutTick = targetIn + len;
                }
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mClipDragMoved
                    && (mClipDragInTick != mClipDragOrigInTick || mClipDragOutTick != mClipDragOrigOutTick)) {
                    EditorAction action{EditorActionType::MoveAndTrimClip};
                    action.id            = clip->id;
                    action.tick          = mClipDragInTick;
                    action.secondaryTick = mClipDragOutTick;
                    ctx.submitAction(std::move(action));
                } else if (!mClipDragMoved && mClipDragMode == 3) {
                    submitSeek(ctx, std::clamp(scale.tickAt(mClipDragStartMouseX), clip->inTick, clip->outTick));
                }
                mClipDragMode = 0;
                mClipDragCameraId.clear();
                mClipDragId.clear();
                mClipDragMoved = false;
            }
        }
    } else if (allowInput && !mDraggingAudioTrackId.empty() && !mDraggingAudioClipId.empty()) {
        auto const* track = state::editing::ClipOps::findAudioTrack(*project, mDraggingAudioTrackId);
        auto const* clip  = track ? state::editing::ClipOps::findAudioClip(*track, mDraggingAudioClipId) : nullptr;
        if (!track || !clip || track->locked || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            mDraggingAudioTrackId.clear();
            mDraggingAudioClipId.clear();
            mDraggingAudioOriginTick = -1;
            mDraggingAudioDraftTick  = -1;
            mDraggingAudioMoved      = false;
        } else {
            ImGui::SetMouseCursor(theme::kCursorGrabbing);
            if (std::abs(ImGui::GetMousePos().x - mDraggingAudioStartMouseX) >= 2.0f * metrics::scale()) {
                mDraggingAudioMoved = true;
            }
            int candidateTick = mDraggingAudioOriginTick;
            if (mDraggingAudioMoved) {
                int const rawDelta = static_cast<int>(std::round(
                    (ImGui::GetMousePos().x - mDraggingAudioStartMouseX) / std::max(0.0001f, scale.pixelsPerTick)
                ));
                int       target   = mDraggingAudioOriginTick + rawDelta;
                if (shiftSnap) {
                    target = snapToEditPoints(
                        ctx,
                        target,
                        scale.pixelsPerTick,
                        true,
                        true,
                        {},
                        {},
                        {},
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        mDraggingAudioClipId
                    );
                } else if (mSnapEnabled) {
                    target = snapTick(target);
                }
                candidateTick = state.totalTicks > 0 ? std::clamp(target, 0, state.totalTicks) : std::max(0, target);
            }
            mDraggingAudioDraftTick = candidateTick;
            float const markerX     = scale.xAt(static_cast<float>(candidateTick));
            drawList->AddLine(
                {markerX, layout.bodyTop},
                {markerX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (mDraggingAudioMoved) {
                    if (candidateTick != mDraggingAudioOriginTick) {
                        EditorAction action{EditorActionType::MoveAudioClip};
                        action.id          = mDraggingAudioTrackId;
                        action.secondaryId = mDraggingAudioClipId;
                        action.tick        = candidateTick;
                        ctx.submitAction(std::move(action));
                    }
                } else {
                    submitSeek(ctx, std::clamp(scale.tickAt(mDraggingAudioStartMouseX), 0, state.totalTicks));
                }
                mDraggingAudioTrackId.clear();
                mDraggingAudioClipId.clear();
                mDraggingAudioOriginTick = -1;
                mDraggingAudioDraftTick  = -1;
                mDraggingAudioMoved      = false;
            }
        }
    }

    float const playheadX = std::clamp(scale.xAt(static_cast<float>(displayTick)), layout.canvasLeft, layout.fullMax.x);
    if (allowInput && !clickConsumed && mMarkerDragId.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && std::abs(ImGui::GetMousePos().x - playheadX) <= 6.0f && ImGui::GetMousePos().y >= layout.rulerTop
        && ImGui::GetMousePos().y < layout.bodyBottom) {
        mDraggingPlayhead = true;
        clickConsumed     = true;
    }
    if (allowInput && mDraggingPlayhead) {
        displayTick = tickFromMouse();
        if (shiftSnap) displayTick = snapToEditPoints(ctx, displayTick, scale.pixelsPerTick, false, true, {}, {});
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            submitSeek(ctx, displayTick);
            mDraggingPlayhead = false;
        }
    }
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && ImGui::GetMousePos().x >= layout.canvasLeft && ImGui::GetMousePos().x <= layout.fullMax.x
        && ImGui::GetMousePos().y >= layout.camerasTop && ImGui::GetMousePos().y < layout.bodyBottom) {
        if (mTool == TimelineTool::Select || mTool == TimelineTool::BoxSelect) {
            mBoxPending          = true;
            mBoxSelecting        = false;
            mBoxAdditive         = ImGui::GetIO().KeyCtrl;
            mBoxStart            = ImGui::GetMousePos();
            mBoxClickTimeOfDay   = false;
            mBoxClickWeather     = false;
            mPenPendingAdd       = false;
            mPenPendingTimeOfDay = false;
            mPenPendingWeather   = false;
            mPenPendingTick      = -1;
            mBoxClickCameraId.clear();
            mBoxClickClipId.clear();
            mBoxClickKeyTick = -1;
        } else {
            submitSeek(ctx, std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks));
        }
    }
    if (allowInput && mBoxPending) {
        if (mBoxSelecting) {
            drawList->AddRectFilled(boxMin, boxMax, theme::withAlpha(theme::kAccent, 0x30));
            drawList->AddRect(boxMin, boxMax, theme::withAlpha(theme::kAccent, 0xc0));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mBoxSelecting) {
                auto const selectTimeOfDayHits = [&](std::vector<int> ticks) {
                    if (ticks.empty()) return;
                    if (ticks.size() == 1) ctx.selection.selectTimeOfDayKeyframe(ticks.front());
                    else ctx.selection.selectTimeOfDayKeyframes(std::move(ticks));
                };
                auto const selectWeatherHits = [&](std::vector<int> ticks) {
                    if (ticks.empty()) return;
                    if (ticks.size() == 1) ctx.selection.selectWeatherKeyframe(ticks.front());
                    else ctx.selection.selectWeatherKeyframes(std::move(ticks));
                };
                if (mBoxAdditive) {
                    if (!weatherBoxHits.empty()
                        && (mBoxClickWeather || (boxHits.empty() && cutBoxHits.empty() && timeOfDayBoxHits.empty()))) {
                        std::vector<int> merged = ctx.selection.selectedWeatherKeyframes();
                        for (int const hit : weatherBoxHits) {
                            if (!ctx.selection.isWeatherKeyframeSelected(hit)) merged.push_back(hit);
                        }
                        selectWeatherHits(std::move(merged));
                    } else if (!timeOfDayBoxHits.empty()
                               && (mBoxClickTimeOfDay || (boxHits.empty() && cutBoxHits.empty()))) {
                        std::vector<int> merged = ctx.selection.selectedTimeOfDayKeyframes();
                        for (int const hit : timeOfDayBoxHits) {
                            if (!ctx.selection.isTimeOfDayKeyframeSelected(hit)) merged.push_back(hit);
                        }
                        selectTimeOfDayHits(std::move(merged));
                    } else if (!boxHits.empty() || !cutBoxHits.empty() || !ctx.selection.keyframes().empty()
                               || !ctx.selection.clipCuts().empty()) {
                        std::vector<state::editing::model::SelectedKeyframe> mergedKeys = ctx.selection.keyframes();
                        for (auto const& hit : boxHits) {
                            if (!ctx.selection.isKeyframeSelected(hit.trackId, hit.tick)) mergedKeys.push_back(hit);
                        }
                        std::vector<state::editing::model::SelectedClipCut> mergedCuts = ctx.selection.clipCuts();
                        for (auto const& hit : cutBoxHits) {
                            if (!ctx.selection.isClipCutSelected(hit.cameraId, hit.startTick, hit.endTick)) {
                                mergedCuts.push_back(hit);
                            }
                        }
                        ctx.selection.setBoxSelection(std::move(mergedKeys), std::move(mergedCuts));
                        if (!boxHits.empty() || !cutBoxHits.empty()) {
                            ctx.commands.showSelectionInspector();
                        }
                    }
                } else if (mBoxClickWeather && !weatherBoxHits.empty()) {
                    selectWeatherHits(std::move(weatherBoxHits));
                } else if (mBoxClickTimeOfDay && !timeOfDayBoxHits.empty()) {
                    selectTimeOfDayHits(std::move(timeOfDayBoxHits));
                } else if (!boxHits.empty() || !cutBoxHits.empty()) {
                    ctx.selection.setBoxSelection(std::move(boxHits), std::move(cutBoxHits));
                    ctx.commands.showSelectionInspector();
                } else if (!timeOfDayBoxHits.empty()) {
                    selectTimeOfDayHits(std::move(timeOfDayBoxHits));
                } else if (!weatherBoxHits.empty()) {
                    selectWeatherHits(std::move(weatherBoxHits));
                } else {
                    ctx.selection.clear();
                }
            } else if (mPenPendingAdd && mPenPendingTimeOfDay && mPenPendingTick >= 0) {
                addTimeOfDayKeyframeAt(ctx, mPenPendingTick);
            } else if (mPenPendingAdd && mPenPendingWeather && mPenPendingTick >= 0) {
                addWeatherKeyframeAt(ctx, mPenPendingTick);
            } else if (mPenPendingAdd && !mBoxClickCameraId.empty() && mPenPendingTick >= 0) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = mBoxClickCameraId;
                action.tick = mPenPendingTick;
                ctx.submitAction(std::move(action));
                queueCameraKeyframeSelection(mBoxClickCameraId, mPenPendingTick);
                ctx.commands.showSelectionInspector();
            } else if (!mBoxClickClipId.empty()) {
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            } else if (mBoxAdditive) {
                // A Ctrl+click that never became a box leaves the selection alone.
            } else if (mBoxClickWeather) {
                ctx.selection.selectWeatherTrack();
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            } else if (mBoxClickTimeOfDay) {
                ctx.selection.selectTimeOfDayTrack();
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            } else if (mTool == TimelineTool::BoxSelect && !mBoxClickCameraId.empty()) {
                bool handled = false;
                if (mBoxClickKeyTick >= 0) {
                    ctx.selection.select(state::editing::model::SelectedKeyframe{mBoxClickCameraId, mBoxClickKeyTick});
                    ctx.commands.showSelectionInspector();
                    handled = true;
                } else if (state.totalTicks > 0) {
                    auto const camIt = std::ranges::find(
                        project->cameras,
                        mBoxClickCameraId,
                        &state::editing::model::CameraEntity::id
                    );
                    if (camIt != project->cameras.end() && !camIt->locked) {
                        auto const  cuts = state::editing::ClipOps::collectCameraClipCuts(*camIt, state.totalTicks);
                        float const seamThreshold = 6.0f * uiScale;
                        float       bestSeamDist  = std::numeric_limits<float>::max();
                        std::optional<state::editing::model::SelectedClipCut> hitCut;
                        for (auto const& cut : cuts) {
                            if (cut.isSeam()) {
                                float const seamX = scale.xAt(static_cast<float>(cut.startTick));
                                float const dist  = std::abs(mBoxStart.x - seamX);
                                if (dist <= seamThreshold && dist < bestSeamDist) {
                                    bestSeamDist = dist;
                                    hitCut       = cut;
                                }
                            }
                        }
                        if (!hitCut) {
                            for (auto const& cut : cuts) {
                                if (cut.isGap()) {
                                    float const gx0 = scale.xAt(static_cast<float>(cut.startTick));
                                    float const gx1 = scale.xAt(static_cast<float>(cut.endTick));
                                    if (mBoxStart.x >= gx0 && mBoxStart.x <= gx1) {
                                        hitCut = cut;
                                        break;
                                    }
                                }
                            }
                        }
                        if (hitCut) {
                            ctx.selection.setClipCuts({*hitCut});
                            ctx.commands.showSelectionInspector();
                            handled = true;
                        }
                    }
                }
                if (!handled) {
                    submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
                }
            } else if (!mBoxClickCameraId.empty()) {
                ctx.selection.select(state::editing::model::SelectedCamera{mBoxClickCameraId});
                if (ctx.commands.isAutoPreviewEnabled()) {
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = mBoxClickCameraId;
                    ctx.submitAction(std::move(action));
                }
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            } else {
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            }
            mBoxPending          = false;
            mBoxSelecting        = false;
            mBoxClickTimeOfDay   = false;
            mBoxClickWeather     = false;
            mPenPendingAdd       = false;
            mPenPendingTimeOfDay = false;
            mPenPendingWeather   = false;
            mPenPendingTick      = -1;
            mBoxClickCameraId.clear();
            mBoxClickClipId.clear();
            mBoxClickKeyTick = -1;
        }
    }

    drawMarkers(ctx, layout, scale);
    if (mSnapGuideTick >= 0) {
        float const guideX = scale.xAt(static_cast<float>(mSnapGuideTick));
        drawList->AddLine({guideX, layout.rulerTop}, {guideX, layout.bodyBottom}, theme::kIconHighlight, 1.5f);
    }

    auto const badge = computePlayheadBadge(
        layout.canvasLeft,
        layout.fullMax.x,
        layout.rulerTop,
        layout.headerHeight,
        scale,
        displayTick
    );
    drawList->AddLine({badge.playheadX, layout.bodyTop}, {badge.playheadX, layout.bodyBottom}, theme::kPlayhead, 1.5f);
    {
        ImFont* const font     = ImGui::GetFont();
        float const   rounding = 3.0f * uiScale;
        drawList->AddRectFilled(
            {badge.badgeX, badge.badgeY},
            {badge.badgeX + badge.badgeW, badge.badgeY + badge.badgeH},
            theme::kPlayhead,
            rounding
        );
        drawList->AddText(
            font,
            badge.fontSize,
            {badge.badgeX + (badge.badgeW - badge.textSize.x) * 0.5f,
             std::floor(badge.badgeY + (badge.badgeH - badge.textSize.y) * 0.5f + 0.5f)},
            theme::kPlayheadLabel,
            badge.text.c_str()
        );
        float const halfW  = 4.0f * uiScale;
        float const capTop = badge.badgeY + badge.badgeH - 0.5f;
        float const capTip = layout.bodyTop + 1.5f * uiScale;
        drawList->AddTriangleFilled(
            {badge.playheadX - halfW, capTop},
            {badge.playheadX + halfW, capTop},
            {badge.playheadX, capTip},
            theme::kPlayhead
        );
    }
    drawList->PopClipRect();

    if (maxScrollY > 0.0f) {
        float const trackHeight = std::max(1.0f, visibleTrackHeight);
        float const thumbHeight = std::max(20.0f, trackHeight * trackHeight / std::max(1.0f, trackContentHeight));
        float const thumbTop    = layout.camerasTop + (trackHeight - thumbHeight) * (mScrollY / maxScrollY);
        drawList->AddRectFilled(
            {layout.fullMax.x - 5.0f, thumbTop},
            {layout.fullMax.x - 2.0f, thumbTop + thumbHeight},
            theme::withAlpha(theme::kScrollThumb, 200),
            2.0f
        );
    }

    ImVec2 const wheelMouse = ImGui::GetMousePos();
    bool const   canvasHovered =
        contains({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, wheelMouse);
    bool const trackListHovered = wheelMouse.x >= layout.listLeft && wheelMouse.x < layout.canvasLeft
                               && wheelMouse.y >= layout.camerasTop && wheelMouse.y < layout.bodyBottom;
    if (allowInput && (canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyAlt) {
            float const anchorX    = std::clamp(wheelMouse.x - layout.canvasLeft, 0.0f, layout.canvasWidth);
            float const anchorTick = scale.tickAtExact(layout.canvasLeft + anchorX);
            mZoomScale =
                std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll     = std::max(0.0f, state.totalTicks * nextPixelsPerTick - layout.canvasWidth);
            mScrollX                      = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (canvasHovered && maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, maxScrollY);
        }
    }

    drawRangeBar(ctx, layout, allowInput);
}

} // namespace playback::editor::ui

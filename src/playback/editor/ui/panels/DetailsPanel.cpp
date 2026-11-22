#include "DetailsPanel.h"

#include "playback/audio/ExternalAudioCache.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/integration/OptiPistonBridge.h"
#include "playback/keyframe/SpeedTimelineEvaluator.h"
#include "playback/keyframe/TimelapseTimelineEvaluator.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/utils/ShellUtils.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <span>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

using widgets::formatTick;

std::string keyframeModeName(state::editing::model::CameraKeyframeMode mode) {
    using Mode = state::editing::model::CameraKeyframeMode;
    switch (mode) {
    case Mode::FirstPerson:
        return "playback.refactorEditor.details.lensModeType.firstPerson"_tr();
    case Mode::ThirdPerson:
        return "playback.refactorEditor.details.lensModeType.thirdPerson"_tr();
    case Mode::Interpolated:
    default:
        return "playback.refactorEditor.details.lensModeType.interpolated"_tr();
    }
}

std::string interpolationName(state::editing::model::CameraInterpolationType interpolation) {
    using Interpolation = state::editing::model::CameraInterpolationType;
    switch (interpolation) {
    case Interpolation::Linear:
        return "playback.refactorEditor.details.interpolationType.linear"_tr();
    case Interpolation::EaseIn:
        return "playback.refactorEditor.details.interpolationType.easeIn"_tr();
    case Interpolation::EaseOut:
        return "playback.refactorEditor.details.interpolationType.easeOut"_tr();
    case Interpolation::EaseInOut:
        return "playback.refactorEditor.details.interpolationType.easeInOut"_tr();
    case Interpolation::Hold:
        return "playback.refactorEditor.details.interpolationType.hold"_tr();
    case Interpolation::Hermite:
        return "playback.refactorEditor.details.interpolationType.hermite"_tr();
    case Interpolation::CubicBezier:
        return "playback.refactorEditor.details.interpolationType.cubicBezier"_tr();
    case Interpolation::Smooth:
    default:
        return "playback.refactorEditor.details.interpolationType.smooth"_tr();
    }
}

float measureLabelColumn(std::initializer_list<std::string> labels, float minWidth = 68.0f) {
    float const scale = metrics::scale();
    float       width = minWidth * scale;
    for (auto const& label : labels) {
        width = std::max(width, ImGui::CalcTextSize(label.c_str()).x + 10.0f * scale);
    }
    float const maxWidth = std::max(minWidth * scale, ImGui::GetContentRegionAvail().x * 0.44f);
    return std::min(width, maxWidth);
}

float keyframeLabelWidth() {
    return measureLabelColumn({
        "playback.refactorEditor.details.tick"_tr(),
        "playback.refactorEditor.details.lensMode"_tr(),
        "playback.refactorEditor.details.position"_tr(),
        "playback.refactorEditor.details.rotation"_tr(),
        "playback.refactorEditor.details.fov"_tr(),
        "playback.refactorEditor.details.interpolation"_tr(),
        "playback.refactorEditor.details.thirdPersonDistance"_tr(),
        "playback.refactorEditor.details.thirdPersonOffset"_tr(),
        "playback.refactorEditor.details.lookAtOffsetY"_tr(),
        "playback.refactorEditor.details.bezierControl1"_tr(),
        "playback.refactorEditor.details.bezierControl2"_tr(),
    });
}

bool vectorInput(
    char const* id,
    float (&values)[3],
    std::array<char const*, 3> const& labels,
    std::array<ImVec4, 3> const*      colors,
    char const*                       format
) {
    bool        edited = false;
    float const scale  = metrics::scale();
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {2.5f * scale, 1.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.0f * scale, 2.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3.0f * scale, 3.5f * scale});
    if (ImGui::BeginTable("##axes", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableNextRow();
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            if (colors) ImGui::TextColored((*colors)[axis], "%s", labels[axis]);
            else ImGui::TextDisabled("%s", labels[axis]);
        }
        ImGui::TableNextRow();
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(axis);
            ImGui::InputFloat("##value", &values[axis], 0.0f, 0.0f, format);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    return edited;
}

bool vectorInput2(char const* id, float (&values)[2], std::array<char const*, 2> const& labels, char const* format) {
    bool        edited = false;
    float const scale  = metrics::scale();
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {2.5f * scale, 1.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.0f * scale, 2.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3.0f * scale, 3.5f * scale});
    if (ImGui::BeginTable("##axes2", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableNextRow();
        for (int axis = 0; axis < 2; ++axis) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", labels[axis]);
        }
        ImGui::TableNextRow();
        for (int axis = 0; axis < 2; ++axis) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(axis);
            ImGui::InputFloat("##value", &values[axis], 0.0f, 0.0f, format);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    return edited;
}

[[nodiscard]] float speedToSliderT(float speed, float minSpeed = 0.25f, float maxSpeed = 8.0f) {
    float const safe = std::max(speed, minSpeed);
    if (safe <= 1.0f) {
        float const negSpan = std::max(0.001f, -std::log2(minSpeed));
        return 0.5f * (1.0f + std::clamp(std::log2(safe) / negSpan, -1.0f, 0.0f));
    }
    float const posSpan = std::max(0.001f, std::log2(maxSpeed));
    return 0.5f + 0.5f * std::clamp(std::log2(safe) / posSpan, 0.0f, 1.0f);
}

[[nodiscard]] float sliderTToSpeed(float t, float minSpeed = 0.25f, float maxSpeed = 8.0f) {
    float const clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped <= 0.5f) {
        float const negSpan = std::max(0.001f, -std::log2(minSpeed));
        return std::exp2((2.0f * clamped - 1.0f) * negSpan);
    }
    float const posSpan = std::max(0.001f, std::log2(maxSpeed));
    return std::exp2((2.0f * clamped - 1.0f) * posSpan);
}

[[nodiscard]] ImU32 speedAccentColor(float speed) {
    if (speed > 1.001f) return theme::kSpeedFast;
    if (speed < 0.999f) return theme::kSpeedSlow;
    return theme::kAccent;
}

[[nodiscard]] ImU32 brightenColor(ImU32 color, float amount) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.x      = c.x + (1.0f - c.x) * amount;
    c.y      = c.y + (1.0f - c.y) * amount;
    c.z      = c.z + (1.0f - c.z) * amount;
    return ImGui::ColorConvertFloat4ToU32(c);
}

[[nodiscard]] ImU32 scaleAlpha(ImU32 color, float alphaScale) {
    int const alpha =
        static_cast<int>(std::lround(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xff) * alphaScale));
    return theme::withAlpha(color, std::clamp(alpha, 0, 255));
}

template <typename DrawInputFn>
void drawInputWithHint(std::string const& hintText, DrawInputFn&& drawInput) {
    float const scale  = metrics::scale();
    float const availW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const gap    = 6.0f * scale;
    float const hintW  = ImGui::CalcTextSize(hintText.c_str()).x;
    float const inputW = availW - hintW - gap;
    if (inputW >= 58.0f * scale) {
        ImGui::SetNextItemWidth(inputW);
        drawInput();
        ImGui::SameLine(0.0f, gap);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", hintText.c_str());
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        drawInput();
        ImGui::TextDisabled("%s", hintText.c_str());
    }
}

template <typename SubmitSpeedFn, typename FormatTooltipFn>
void drawLogSpeedSlider(
    char const*            id,
    bool                   editable,
    float                  committedSpeed,
    float                  minSliderSpeed,
    float                  maxSliderSpeed,
    float                  minClampSpeed,
    float                  maxClampSpeed,
    std::span<float const> snapPresets,
    bool&                  dragging,
    float&                 dragValue,
    SubmitSpeedFn&&        onCommit,
    FormatTooltipFn&&      formatTooltip
) {
    float const  scale      = metrics::scale();
    float const  availW     = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const  sliderH    = std::round(18.0f * scale);
    float const  thumbR     = 5.5f * scale;
    float const  railH      = 4.0f * scale;
    float const  alphaScale = editable ? 1.0f : ImGui::GetStyle().Alpha;
    ImVec2 const min        = ImGui::GetCursorScreenPos();
    ImVec2 const max        = {min.x + availW, min.y + sliderH};

    ImGui::InvisibleButton(id, {availW, sliderH});
    bool const    hovered    = ImGui::IsItemHovered();
    bool const    active     = ImGui::IsItemActive();
    ImGuiID const sliderId   = ImGui::GetItemID();
    ImGuiStorage* storage    = ImGui::GetStateStorage();
    bool          ignoreDrag = storage->GetBool(sliderId, false);
    if (!active) {
        ignoreDrag = false;
        storage->SetBool(sliderId, false);
    }

    float const trackMinX = min.x + thumbR;
    float const trackMaxX = std::max(trackMinX + 1.0f, max.x - thumbR);
    float const trackW    = trackMaxX - trackMinX;
    float const centerX   = min.x + availW * 0.5f;
    float const centerY   = (min.y + max.y) * 0.5f;

    if (editable) {
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ignoreDrag = true;
            storage->SetBool(sliderId, true);
            dragging = false;
            onCommit(1.0f);
        } else if (active && !ignoreDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float const rawT     = std::clamp((ImGui::GetMousePos().x - trackMinX) / trackW, 0.0f, 1.0f);
            float       newSpeed = sliderTToSpeed(rawT, minSliderSpeed, maxSliderSpeed);
            if (ImGui::GetIO().KeyShift && !snapPresets.empty()) {
                float bestDist   = std::abs(rawT - speedToSliderT(snapPresets[0], minSliderSpeed, maxSliderSpeed));
                float bestPreset = snapPresets[0];
                for (size_t i = 1; i < snapPresets.size(); ++i) {
                    float const dist = std::abs(rawT - speedToSliderT(snapPresets[i], minSliderSpeed, maxSliderSpeed));
                    if (dist < bestDist) {
                        bestDist   = dist;
                        bestPreset = snapPresets[i];
                    }
                }
                newSpeed = bestPreset;
            }
            dragging  = true;
            dragValue = std::clamp(newSpeed, minClampSpeed, maxClampSpeed);
        } else if (dragging && (!active || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            float const finalSpeed = dragValue;
            dragging               = false;
            onCommit(finalSpeed);
        }
    } else if (dragging) {
        dragging = false;
    }

    float const sliderSpeed = dragging ? dragValue : committedSpeed;
    float const t           = speedToSliderT(sliderSpeed, minSliderSpeed, maxSliderSpeed);
    float const thumbX      = trackMinX + t * trackW;
    float const railTop     = centerY - railH * 0.5f;
    float const railBottom  = centerY + railH * 0.5f;
    float const railRound   = railH * 0.5f;
    auto* const dl          = ImGui::GetWindowDrawList();

    dl->AddRectFilled(
        {trackMinX, railTop},
        {trackMaxX, railBottom},
        scaleAlpha(theme::kInputBg, alphaScale),
        railRound
    );
    dl->AddRect(
        {trackMinX, railTop},
        {trackMaxX, railBottom},
        scaleAlpha(theme::kBorder, alphaScale),
        railRound,
        0,
        1.0f
    );

    // Center 1.0x reference tick.
    dl->AddLine(
        {centerX, railTop - 3.0f * scale},
        {centerX, railBottom + 3.0f * scale},
        scaleAlpha(theme::kLine, alphaScale),
        std::max(1.0f, 1.25f * scale)
    );

    ImU32 const accent = speedAccentColor(sliderSpeed);
    if (sliderSpeed > 1.001f || sliderSpeed < 0.999f) {
        float const fillLeft  = std::min(centerX, thumbX);
        float const fillRight = std::max(centerX, thumbX);
        if (fillRight - fillLeft > 0.5f) {
            dl->AddRectFilled({fillLeft, railTop}, {fillRight, railBottom}, scaleAlpha(accent, alphaScale), railRound);
        }
    }

    ImU32 const thumbCol = (editable && (hovered || active)) ? brightenColor(accent, active ? 0.30f : 0.18f) : accent;
    dl->AddCircleFilled({thumbX, centerY}, thumbR, scaleAlpha(thumbCol, alphaScale));
    dl->AddCircle({thumbX, centerY}, thumbR, scaleAlpha(theme::kKeyframeOutline, alphaScale), 0, 1.0f * scale);

    std::string const tip = formatTooltip(sliderSpeed);
    widgets::itemTooltip(tip.c_str());
}

template <typename T, typename FormatLabelFn, typename IsActiveFn, typename AccentFn, typename OnSelectFn>
void drawPresetChips(
    std::span<T const> presets,
    FormatLabelFn&&    formatLabel,
    IsActiveFn&&       isActive,
    AccentFn&&         accentFor,
    OnSelectFn&&       onSelect
) {
    if (presets.empty()) return;
    float const  scale    = metrics::scale();
    size_t const count    = presets.size();
    float const  spacing  = 3.0f * scale;
    float const  availW   = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float const  totalGap = spacing * static_cast<float>(count - 1);
    float const  chipH    = std::round(ImGui::GetFontSize() + 8.0f * scale);

    std::vector<std::string> labels(count);
    std::vector<float>       textWidths(count);
    float                    sumTextW = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        labels[i]        = formatLabel(presets[i]);
        float const tw   = ImGui::CalcTextSize(labels[i].c_str()).x;
        textWidths[i]   = tw;
        sumTextW       += tw;
    }
    float const usableW = std::max(1.0f, availW - totalGap);
    float const extra   = std::max(0.0f, usableW - sumTextW) / static_cast<float>(count);
    float const shrink  = (sumTextW > usableW && sumTextW > 1.0f) ? (usableW / sumTextW) : 1.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing, spacing});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kButtonRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        auto const& preset       = presets[i];
        bool const  active       = isActive(preset);
        ImU32 const presetAccent = accentFor(preset);
        float const targetW      = textWidths[i] * shrink + extra;
        float const w = (i == count - 1) ? std::max(1.0f, ImGui::GetContentRegionAvail().x) : std::round(targetW);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(presetAccent, 0x44));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(presetAccent, 0x5c));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(presetAccent, 0x70));
            ImGui::PushStyleColor(ImGuiCol_Border, presetAccent);
            ImGui::PushStyleColor(ImGuiCol_Text, presetAccent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kButtonSubtle);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
            ImGui::PushStyleColor(ImGuiCol_Border, theme::kButtonSubtleBorder);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(labels[i].c_str(), {w, chipH})) {
            onSelect(preset);
        }
        ImGui::PopID();
        ImGui::PopStyleColor(5);
    }
    ImGui::PopStyleVar(4);
}

template <typename SubmitSpeedFn>
void drawSpeedPresetChips(float displayedSpeed, std::span<float const> presets, SubmitSpeedFn&& onSelect) {
    drawPresetChips(
        presets,
        [](float preset) {
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "%.2gx", static_cast<double>(preset));
            return std::string(buf);
        },
        [displayedSpeed](float preset) { return std::abs(displayedSpeed - preset) < 0.001f; },
        [](float preset) { return speedAccentColor(preset); },
        std::forward<SubmitSpeedFn>(onSelect)
    );
}

template <typename CommitBezierFn>
void drawBezierCurveRows(
    char const*                  idPrefix,
    bool                         locked,
    state::editing::model::Vec2  keyCtrl1,
    state::editing::model::Vec2  keyCtrl2,
    int&                         dragHandle,
    state::editing::model::Vec2& draftCtrl1,
    state::editing::model::Vec2& draftCtrl2,
    CommitBezierFn&&             onCommit
) {
    auto ctrl1 = dragHandle != 0 ? draftCtrl1 : keyCtrl1;
    auto ctrl2 = dragHandle != 0 ? draftCtrl2 : keyCtrl2;
    ctrl1.x    = std::clamp(ctrl1.x, 0.0f, 1.0f);
    ctrl1.y    = std::clamp(ctrl1.y, 0.0f, 1.0f);
    ctrl2.x    = std::clamp(ctrl2.x, 0.0f, 1.0f);
    ctrl2.y    = std::clamp(ctrl2.y, 0.0f, 1.0f);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.bezierCurve"_tr().c_str());
    ImGui::TableNextColumn();
    {
        float const  uiScale  = metrics::scale();
        float const  availW   = std::max(64.0f, ImGui::GetContentRegionAvail().x);
        float const  boxH     = std::clamp(availW, 96.0f * uiScale, 140.0f * uiScale);
        float const  pad      = 8.0f * uiScale;
        ImVec2 const boxMin   = ImGui::GetCursorScreenPos();
        ImVec2 const boxMax   = {boxMin.x + availW, boxMin.y + boxH};
        ImVec2 const innerMin = {boxMin.x + pad, boxMin.y + pad};
        ImVec2 const innerMax = {boxMax.x - pad, boxMax.y - pad};
        float const  innerW   = std::max(1.0f, innerMax.x - innerMin.x);
        float const  innerH   = std::max(1.0f, innerMax.y - innerMin.y);

        auto const toScreen = [&](state::editing::model::Vec2 p) -> ImVec2 {
            return {innerMin.x + p.x * innerW, innerMax.y - p.y * innerH};
        };
        auto const fromScreen = [&](ImVec2 s) -> state::editing::model::Vec2 {
            return {
                std::clamp((s.x - innerMin.x) / innerW, 0.0f, 1.0f),
                std::clamp((innerMax.y - s.y) / innerH, 0.0f, 1.0f),
            };
        };

        std::string const canvasId = std::string("##") + idPrefix + "-canvas";
        ImGui::InvisibleButton(canvasId.c_str(), {availW, boxH});
        bool const   hovered = ImGui::IsItemHovered();
        ImVec2 const mouse   = ImGui::GetMousePos();
        ImVec2 const p0      = toScreen({0.0f, 0.0f});
        ImVec2       p1      = toScreen(ctrl1);
        ImVec2       p2      = toScreen(ctrl2);
        ImVec2 const p3      = toScreen({1.0f, 1.0f});

        auto const distSq = [](ImVec2 a, ImVec2 b) {
            float const dx = a.x - b.x;
            float const dy = a.y - b.y;
            return dx * dx + dy * dy;
        };
        float const hitR2    = (9.0f * uiScale) * (9.0f * uiScale);
        float const d1       = distSq(mouse, p1);
        float const d2       = distSq(mouse, p2);
        int         hoveredH = 0;
        if (hovered && !locked) {
            if (d1 <= hitR2 && (d1 <= d2 || d2 > hitR2)) hoveredH = 1;
            else if (d2 <= hitR2) hoveredH = 2;
        }
        if (hoveredH != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragHandle = hoveredH;
            draftCtrl1 = ctrl1;
            draftCtrl2 = ctrl2;
        }
        if (dragHandle != 0) {
            if (!locked && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (dragHandle == 1) draftCtrl1 = fromScreen(mouse);
                else draftCtrl2 = fromScreen(mouse);
                ctrl1 = draftCtrl1;
                ctrl2 = draftCtrl2;
                p1    = toScreen(ctrl1);
                p2    = toScreen(ctrl2);
            } else {
                dragHandle = 0;
                if (!locked) onCommit(draftCtrl1, draftCtrl2);
            }
        }
        if (hoveredH != 0 || dragHandle != 0) {
            ImGui::SetMouseCursor(dragHandle != 0 ? theme::kCursorGrabbing : theme::kCursorGrab);
        }

        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(boxMin, boxMax, theme::kInputBg, 4.0f * uiScale);
        dl->AddRect(boxMin, boxMax, theme::kBorder, 4.0f * uiScale);
        dl->AddLine(p0, p3, theme::kFineDivider, 1.0f);
        dl->AddLine(p0, p1, theme::withAlpha(theme::kTextDim, 0xb0), 1.2f * uiScale);
        dl->AddLine(p3, p2, theme::withAlpha(theme::kTextDim, 0xb0), 1.2f * uiScale);
        dl->AddBezierCubic(p0, p1, p2, p3, theme::kAccent, 2.0f * uiScale, 32);
        dl->AddCircleFilled(p0, 2.5f * uiScale, theme::kTextDim);
        dl->AddCircleFilled(p3, 2.5f * uiScale, theme::kTextDim);

        float const handleR = 4.5f * uiScale;
        ImU32 const c1Color = (dragHandle == 1 || hoveredH == 1) ? theme::kSelected : theme::kKeyframe;
        ImU32 const c2Color = (dragHandle == 2 || hoveredH == 2) ? theme::kSelected : theme::kKeyframe;
        dl->AddCircleFilled(p1, handleR, c1Color);
        dl->AddCircle(p1, handleR, theme::kKeyframeOutline, 0, 1.2f * uiScale);
        dl->AddCircleFilled(p2, handleR, c2Color);
        dl->AddCircle(p2, handleR, theme::kKeyframeOutline, 0, 1.2f * uiScale);
    }

    static constexpr std::array xyLabels{"X", "Y"};
    std::string const           c1Id  = std::string(idPrefix) + "-c1";
    float                       c1[2] = {ctrl1.x, ctrl1.y};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.bezierControl1"_tr().c_str());
    ImGui::TableNextColumn();
    if (vectorInput2(c1Id.c_str(), c1, xyLabels, "%.2f") && std::isfinite(c1[0]) && std::isfinite(c1[1])) {
        onCommit({std::clamp(c1[0], 0.0f, 1.0f), std::clamp(c1[1], 0.0f, 1.0f)}, ctrl2);
    }

    std::string const c2Id  = std::string(idPrefix) + "-c2";
    float             c2[2] = {ctrl2.x, ctrl2.y};
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.bezierControl2"_tr().c_str());
    ImGui::TableNextColumn();
    if (vectorInput2(c2Id.c_str(), c2, xyLabels, "%.2f") && std::isfinite(c2[0]) && std::isfinite(c2[1])) {
        onCommit(ctrl1, {std::clamp(c2[0], 0.0f, 1.0f), std::clamp(c2[1], 0.0f, 1.0f)});
    }
}

[[nodiscard]] std::filesystem::path utf8ToPath(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
}

[[nodiscard]] std::string pathToUtf8(std::filesystem::path const& path) {
    auto const u8 = path.u8string();
    return {reinterpret_cast<char const*>(u8.data()), u8.size()};
}

template <typename SubmitVolumeFn>
void drawAudioVolumeControl(
    char const*        idPrefix,
    bool               editable,
    float              committedVolume,
    std::string const& targetId,
    std::string&       dragId,
    bool&              dragging,
    float&             dragValue,
    SubmitVolumeFn&&   onCommit
) {
    if (dragging && dragId != targetId) {
        dragging = false;
        dragId.clear();
    }
    float const committed = state::editing::model::clampAudioVolume(committedVolume);
    float const displayed = (dragging && dragId == targetId) ? dragValue : committed;
    float       volumePct = displayed * 100.0f;

    ImGui::TextDisabled("%s", "playback.refactorEditor.audio.volume"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    std::string const sliderId = std::string("##") + idPrefix + "-volume";
    ImGui::SliderFloat(sliderId.c_str(), &volumePct, 0.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    if (editable) {
        if (ImGui::IsItemActive()) {
            dragging  = true;
            dragId    = targetId;
            dragValue = state::editing::model::clampAudioVolume(volumePct / 100.0f);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            float const finalVolume = state::editing::model::clampAudioVolume(volumePct / 100.0f);
            dragging                = false;
            dragId.clear();
            if (std::abs(finalVolume - committed) > 1e-4f) {
                onCommit(finalVolume);
            }
        } else if (!ImGui::IsItemActive() && dragging && dragId == targetId) {
            dragging = false;
            dragId.clear();
        }
    } else if (dragging) {
        dragging = false;
        dragId.clear();
    }

    static constexpr float kVolumePresets[] = {0.5f, 1.0f, 1.5f};
    ImGui::PushID(idPrefix);
    drawPresetChips<float>(
        kVolumePresets,
        [](float preset) {
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(preset * 100.0f));
            return std::string(buf);
        },
        [displayed](float preset) { return std::abs(displayed - preset) < 0.005f; },
        [](float) { return theme::kAccent; },
        [&](float preset) {
            dragging = false;
            dragId.clear();
            if (std::abs(preset - committed) > 1e-4f) {
                onCommit(preset);
            }
        }
    );
    ImGui::PopID();
}

} // namespace

void DetailsPanel::resetAudioDrafts() {
    mAudioTrackNameId.clear();
    mAudioTrackNameBuffer.fill('\0');
    mAudioTrackNameEditing = false;
    mAudioTrackVolumeDragId.clear();
    mAudioTrackVolumeDragging  = false;
    mAudioTrackVolumeDragValue = 1.0f;
    mAudioClipPathId.clear();
    mAudioClipPathBuffer.fill('\0');
    mAudioClipPathEditing = false;
    mAudioClipVolumeDragId.clear();
    mAudioClipVolumeDragging  = false;
    mAudioClipVolumeDragValue = 1.0f;
}

void DetailsPanel::draw(PanelContext const& ctx) {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImVec2 const avail  = ImGui::GetContentRegionAvail();
    float const  railW  = metrics::rail();
    float const  scale  = metrics::scale();
    drawRail(railW, avail.y);

    ImGui::SetCursorScreenPos({origin.x + railW, origin.y});
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        {theme::kInspectorPaddingX * scale, theme::kInspectorPaddingY * scale}
    );
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f * scale, 5.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * scale, 4.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::kButtonSubtleBorder);
    ImGui::BeginChild(
        "##InspectorBody",
        {std::max(1.0f, avail.x - railW), avail.y},
        ImGuiChildFlags_AlwaysUseWindowPadding
    );
    if (!ctx.state.project) {
        mSpeedKeyDragging      = false;
        mSpeedKeyDragTick      = -1;
        mSpeedBezierDragHandle = 0;
        mSpeedBezierDragTick   = -1;
        mFreezeDelayDragging   = false;
        mFreezeDelayDragTick   = -1;
        mTimelapseTextTick.reset();
        mTimelapseTextBuffer.fill('\0');
        mTimelapseTextEditing = false;
        resetAudioDrafts();
        widgets::noActiveProjectPlaceholder();
    } else {
        if (mPage != InspectorPage::Selection) {
            mSpeedKeyDragging      = false;
            mSpeedKeyDragTick      = -1;
            mSpeedBezierDragHandle = 0;
            mSpeedBezierDragTick   = -1;
            mFreezeDelayDragging   = false;
            mFreezeDelayDragTick   = -1;
            mTimelapseTextTick.reset();
            mTimelapseTextBuffer.fill('\0');
            mTimelapseTextEditing = false;
            resetAudioDrafts();
        }
        switch (mPage) {
        case InspectorPage::Selection:
            drawSelectionPage(ctx);
            break;
        case InspectorPage::Export:
            drawExportPage(ctx);
            break;
        case InspectorPage::Preview:
            drawPreviewPage(ctx);
            break;
        case InspectorPage::Settings:
            drawSettingsPage(ctx);
            break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(5);
}

void DetailsPanel::drawRail(float width, float height) {
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    auto*        drawList = ImGui::GetWindowDrawList();
    float const  scale    = metrics::scale();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme::kRailBg);
    drawList->AddLine({origin.x + width, origin.y}, {origin.x + width, origin.y + height}, theme::kBorder);

    struct Entry {
        InspectorPage page;
        char const*   icon;
        std::string   tooltip;
        bool          enabled;
    };
    std::array const entries{
        Entry{InspectorPage::Selection, ICON_PANEL_LEFT, "playback.refactorEditor.inspector.selection"_tr(), true},
        Entry{InspectorPage::Export,    ICON_EXPORT,     "playback.refactorEditor.inspector.export"_tr(),    true},
        Entry{InspectorPage::Preview,   ICON_EYE,        "playback.refactorEditor.inspector.preview"_tr(),   true},
        Entry{InspectorPage::Settings,  ICON_SETTINGS,   "playback.refactorEditor.inspector.settings"_tr(),  true},
    };

    float const button = metrics::iconButton();
    float const gap    = 4.0f * scale;
    float const barW   = std::max(2.0f, 2.5f * scale);
    float const insetY = 4.0f * scale;
    float       y      = origin.y + 6.0f * scale;
    for (auto const& entry : entries) {
        bool const active = mPage == entry.page;
        ImGui::SetCursorScreenPos({origin.x + (width - button) * 0.5f, y});
        ImGui::PushID(static_cast<int>(entry.page));
        if (widgets::iconToggle("rail", entry.icon, entry.tooltip.c_str(), active, entry.enabled)) mPage = entry.page;
        ImGui::PopID();
        if (active) {
            drawList->AddRectFilled(
                {origin.x + 1.0f, y + insetY},
                {origin.x + 1.0f + barW, y + button - insetY},
                theme::kRailActive,
                barW * 0.5f
            );
        }
        y += button + gap;
    }
}

void DetailsPanel::drawSelectionPage(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    std::string subject   = "playback.refactorEditor.details.noSelection"_tr();
    if (auto const* selectedClip = selection.getAs<state::editing::model::SelectedClip>()) {
        auto const hit = state::editing::ClipOps::findClipWithCamera(*project, selectedClip->clipId);
        subject        = hit ? ("playback.refactorEditor.details.clip"_tr() + " (" + hit.camera->name + ")")
                             : "playback.refactorEditor.details.clip"_tr();
    } else if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        subject            = camera ? camera->name : "playback.refactorEditor.details.camera"_tr();
    } else if (selection.keyframeCount() > 1) {
        subject = "playback.refactorEditor.details.keyframesSelected"_tr(selection.keyframeCount());
    } else if (selection.getAs<state::editing::model::SelectedKeyframe>()) {
        subject = "playback.refactorEditor.details.cameraKeyframe"_tr();
    } else if (selection.getAs<state::editing::model::SelectedMarker>()) {
        subject = "playback.refactorEditor.details.marker"_tr();
    } else if (selection.selectedSpeedKeyframe()) {
        subject = "playback.refactorEditor.speed.keyframeDetailsTitle"_tr();
    } else if (selection.isSpeedTrackSelected()) {
        subject = "playback.refactorEditor.speed.trackDetailsTitle"_tr();
    } else if (selection.selectedFreezeKeyframe()) {
        subject = "playback.refactorEditor.freeze.keyframeDetailsTitle"_tr();
    } else if (selection.isFreezeTrackSelected()) {
        subject = "playback.refactorEditor.freeze.trackDetailsTitle"_tr();
    } else if (selection.selectedTimelapseKeyframe()) {
        subject = "playback.refactorEditor.timelapse.keyframeDetailsTitle"_tr();
    } else if (selection.isTimelapseTrackSelected()) {
        subject = "playback.refactorEditor.timelapse.trackDetailsTitle"_tr();
    } else if (auto const selectedAudioClip = selection.selectedAudioClip()) {
        auto const* track = state::editing::ClipOps::findAudioTrack(*project, selectedAudioClip->trackId);
        auto const* clip  = track ? state::editing::ClipOps::findAudioClip(*track, selectedAudioClip->clipId) : nullptr;
        subject = (clip && !clip->name.empty()) ? clip->name : "playback.refactorEditor.audio.clipDetailsTitle"_tr();
    } else if (auto const selectedAudioTrackId = selection.selectedAudioTrackId()) {
        auto const* track = state::editing::ClipOps::findAudioTrack(*project, *selectedAudioTrackId);
        subject           = track ? track->name : "playback.refactorEditor.audio.trackDetailsTitle"_tr();
    }
    property::beginInspector("playback.refactorEditor.inspector.selection"_tr(), subject);

    if (drawClip(ctx)) return;
    if (drawCamera(ctx)) return;
    if (drawKeyframes(ctx)) return;
    if (drawKeyframe(ctx)) return;
    if (drawMarker(ctx)) return;
    if (drawSpeedKeyframe(ctx)) return;
    if (drawSpeedTrack(ctx)) return;
    if (drawFreezeKeyframe(ctx)) return;
    if (drawFreezeTrack(ctx)) return;
    if (drawTimelapseKeyframe(ctx)) return;
    if (drawTimelapseTrack(ctx)) return;
    if (drawAudioClip(ctx)) return;
    if (drawAudioTrack(ctx)) return;
    drawOverview(ctx);
}

bool DetailsPanel::drawClip(PanelContext const& ctx) {
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedClip>();
    if (!selected) return false;

    auto const project = ctx.state.project;
    auto const hit     = state::editing::ClipOps::findClipWithCamera(*project, selected->clipId);
    if (!hit) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.clipMissing"_tr().c_str());
        return true;
    }
    auto const* camera = hit.camera;
    auto const* clip   = hit.clip;

    if (property::beginSection("playback.refactorEditor.details.clip"_tr().c_str())) {
        property::textRow("playback.refactorEditor.details.clipLane"_tr().c_str(), camera->name.c_str());
        property::separator();
        bool const  editable = !camera->locked && !clip->locked;
        float const scale    = metrics::scale();

        int minIn  = 0;
        int maxOut = std::max(1, project->totalTicks);
        for (auto const& other : camera->clips) {
            if (other.id == clip->id) continue;
            if (other.outTick <= clip->inTick) minIn = std::max(minIn, other.outTick);
            if (other.inTick >= clip->outTick) maxOut = std::min(maxOut, other.inTick);
        }
        minIn  = std::clamp(minIn, 0, project->totalTicks);
        maxOut = std::clamp(maxOut, minIn, project->totalTicks);

        int const sourceTicks       = std::max(0, clip->outTick - clip->inTick);
        double    outputDurationSec = static_cast<double>(sourceTicks) / static_cast<double>(widgets::kTicksPerSecond);
        if (sourceTicks > 0 && keyframe::hasActiveSpeedCurve(*project)) {
            keyframe::SpeedTimeMapping const mapping(*project, clip->outTick);
            outputDurationSec = mapping.outputSecondsBetween(clip->inTick, clip->outTick);
        }

        float const clipLabelW = measureLabelColumn({
            "playback.refactorEditor.details.start"_tr(),
            "playback.refactorEditor.details.end"_tr(),
            "playback.refactorEditor.details.duration"_tr(),
            "playback.refactorEditor.details.outputDuration"_tr(),
        });

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 3.0f * scale});
        if (ImGui::BeginTable(
                "##clip-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, clipLabelW);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int inTick = clip->inTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.start"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(clip->inTick), [&] {
                ImGui::InputInt("##clip-in", &inTick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    inTick = std::clamp(inTick, minIn, std::max(minIn, clip->outTick - 1));
                    if (inTick != clip->inTick) {
                        EditorAction action{EditorActionType::MoveAndTrimClip};
                        action.id            = clip->id;
                        action.tick          = inTick;
                        action.secondaryTick = clip->outTick;
                        ctx.submitAction(std::move(action));
                    }
                }
            });

            int outTick = clip->outTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.end"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(clip->outTick), [&] {
                ImGui::InputInt("##clip-out", &outTick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    outTick = std::clamp(outTick, std::min(maxOut, clip->inTick + 1), maxOut);
                    if (outTick != clip->outTick) {
                        EditorAction action{EditorActionType::MoveAndTrimClip};
                        action.id            = clip->id;
                        action.tick          = clip->inTick;
                        action.secondaryTick = outTick;
                        ctx.submitAction(std::move(action));
                    }
                }
            });

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.duration"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(formatTick(sourceTicks).c_str());

            char outDurBuf[32]{};
            std::snprintf(outDurBuf, sizeof(outDurBuf), "%.2f s", outputDurationSec);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.outputDuration"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(outDurBuf);

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();

        ImGui::Dummy({0.0f, 1.0f * scale});
        ImGui::BeginDisabled(camera->locked);
        bool locked = clip->locked;
        if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetClipLocked};
            action.id    = clip->id;
            action.value = locked;
            ctx.submitAction(std::move(action));
        }

        bool exportEnabled = clip->exportEnabled;
        if (ImGui::Checkbox("playback.refactorEditor.details.exportEnabled"_tr().c_str(), &exportEnabled)) {
            EditorAction action{EditorActionType::SetClipExportEnabled};
            action.id    = clip->id;
            action.value = exportEnabled;
            ctx.submitAction(std::move(action));
        }
        ImGui::EndDisabled();

        property::separator();
        int const  playhead = ctx.state.currentTick;
        bool const canSplit = editable && clip->inTick < playhead && playhead < clip->outTick;
        if (property::actionButton(
                "playback.refactorEditor.details.splitClipAtPlayhead"_tr().c_str(),
                canSplit,
                property::ButtonVariant::Secondary,
                ICON_SPLIT
            )) {
            EditorAction action{EditorActionType::SplitClip};
            action.id   = clip->id;
            action.tick = playhead;
            ctx.submitAction(std::move(action));
        }
        bool const canTrimIn = editable && minIn <= playhead && playhead < clip->outTick && playhead != clip->inTick;
        bool const canTrimOut = editable && clip->inTick < playhead && playhead <= maxOut && playhead != clip->outTick;
        std::string const trimInLabel  = "playback.refactorEditor.details.trimClipInToPlayhead"_tr();
        std::string const trimOutLabel = "playback.refactorEditor.details.trimClipOutToPlayhead"_tr();
        int const         trimChoice   = property::actionButtonPair(
            {trimInLabel.c_str(), ICON_CHEVRONS_RIGHT, property::ButtonVariant::Secondary, canTrimIn},
            {trimOutLabel.c_str(), ICON_CHEVRONS_LEFT, property::ButtonVariant::Secondary, canTrimOut}
        );
        if (trimChoice == 0) {
            EditorAction action{EditorActionType::MoveAndTrimClip};
            action.id            = clip->id;
            action.tick          = playhead;
            action.secondaryTick = clip->outTick;
            ctx.submitAction(std::move(action));
        } else if (trimChoice == 1) {
            EditorAction action{EditorActionType::MoveAndTrimClip};
            action.id            = clip->id;
            action.tick          = clip->inTick;
            action.secondaryTick = playhead;
            ctx.submitAction(std::move(action));
        }
        property::separator();
        bool const canExport = ctx.state.capabilities.videoExport
                            && !exporting::isExportActive(ctx.state.exportStatus.state) && project->totalTicks > 0;
        if (property::actionButton(
                "playback.refactorEditor.details.exportClip"_tr().c_str(),
                canExport,
                property::ButtonVariant::Primary,
                ICON_EXPORT
            )) {
            ctx.commands.openExportSingleClip(clip->id);
        }
        if (property::actionButton(
                "playback.refactorEditor.details.deleteClip"_tr().c_str(),
                editable,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            EditorAction action{EditorActionType::DeleteClip};
            action.id = clip->id;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        if (camera->locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
            ImGui::PopTextWrapPos();
        } else if (clip->locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.clipLocked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    }
    return true;
}

bool DetailsPanel::drawCamera(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.camera"_tr().c_str())) {
            property::textRow("playback.refactorEditor.details.nameLabel"_tr().c_str(), camera->name.c_str());
            std::string const keyframeCount = std::to_string(camera->keysByTick.size());
            property::textRow("playback.refactorEditor.details.keyframesLabel"_tr().c_str(), keyframeCount.c_str());
            size_t enabledClips = 0;
            for (auto const& clip : camera->clips) {
                if (clip.exportEnabled) ++enabledClips;
            }
            property::formattedRow("playback.refactorEditor.details.clipsSummary"_tr(camera->clips.size(), enabledClips));
            property::separator();
            float const scale     = metrics::scale();
            bool const  pairFlags = ImGui::GetContentRegionAvail().x >= 168.0f * scale;
            if (pairFlags) {
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f * scale, 1.0f * scale});
                if (ImGui::BeginTable(
                        "##camera-flags",
                        2,
                        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX
                    )) {
                    ImGui::TableNextColumn();
                    bool       enabled      = camera->enabled;
                    auto const enabledLabel = "playback.refactorEditor.details.enabled"_tr();
                    if (ImGui::Checkbox(enabledLabel.c_str(), &enabled)) {
                        EditorAction action{EditorActionType::SetCameraEnabled};
                        action.id    = camera->id;
                        action.value = enabled;
                        submit(std::move(action));
                    }
                    ImGui::TableNextColumn();
                    bool locked = camera->locked;
                    if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
                        EditorAction action{EditorActionType::SetCameraLocked};
                        action.id    = camera->id;
                        action.value = locked;
                        submit(std::move(action));
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            } else {
                bool       enabled      = camera->enabled;
                auto const enabledLabel = "playback.refactorEditor.details.enabled"_tr();
                if (ImGui::Checkbox(enabledLabel.c_str(), &enabled)) {
                    EditorAction action{EditorActionType::SetCameraEnabled};
                    action.id    = camera->id;
                    action.value = enabled;
                    submit(std::move(action));
                }
                bool locked = camera->locked;
                if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
                    EditorAction action{EditorActionType::SetCameraLocked};
                    action.id    = camera->id;
                    action.value = locked;
                    submit(std::move(action));
                }
            }
            ImGui::BeginDisabled(camera->locked);
            property::separator();
            bool hasShake = camera->shake.has_value();
            if (ImGui::Checkbox("playback.refactorEditor.details.cameraShake"_tr().c_str(), &hasShake)) {
                EditorAction action{EditorActionType::SetCameraShake};
                action.id = camera->id;
                if (hasShake) {
                    int startTick = std::clamp(ctx.state.currentTick, 0, project->totalTicks);
                    int endTick   = std::clamp(startTick + 40, startTick, project->totalTicks);
                    if (camera->keysByTick.size() >= 2) {
                        startTick = camera->keysByTick.begin()->first;
                        endTick   = camera->keysByTick.rbegin()->first;
                    }
                    action.shake = state::editing::model::CameraShake{
                        startTick,
                        endTick,
                        0.0f,
                        1.0f,
                        1.0f,
                    };
                }
                submit(std::move(action));
            }
            if (camera->shake) {
                auto        shake      = *camera->shake;
                bool        edited     = false;
                float const scale      = metrics::scale();
                float const labelWidth = measureLabelColumn({
                    "playback.refactorEditor.details.start"_tr(),
                    "playback.refactorEditor.details.end"_tr(),
                    "playback.refactorEditor.details.shakePositionAmplitude"_tr(),
                    "playback.refactorEditor.details.shakeRotationAmplitude"_tr(),
                    "playback.refactorEditor.details.shakeFrequency"_tr(),
                });
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
                if (ImGui::BeginTable(
                        "##camera-shake-properties",
                        2,
                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                    )) {
                    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.start"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputInt("##shake-start", &shake.startTick, 0, 0);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        shake.startTick = std::clamp(shake.startTick, 0, project->totalTicks);
                        shake.endTick   = std::max(shake.endTick, shake.startTick);
                        edited          = true;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.end"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputInt("##shake-end", &shake.endTick, 0, 0);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        shake.endTick = std::clamp(shake.endTick, shake.startTick, project->totalTicks);
                        edited        = true;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakePositionAmplitude"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##shake-pos", &shake.positionAmplitude, 0.0f, 0.0f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(shake.positionAmplitude)) {
                        shake.positionAmplitude = std::max(0.0f, shake.positionAmplitude);
                        edited                  = true;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeRotationAmplitude"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##shake-rot", &shake.rotationAmplitude, 0.0f, 0.0f, "%.2f°");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(shake.rotationAmplitude)) {
                        shake.rotationAmplitude = std::max(0.0f, shake.rotationAmplitude);
                        edited                  = true;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeFrequency"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##shake-freq", &shake.frequency, 0.0f, 0.0f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(shake.frequency)) {
                        shake.frequency = std::clamp(shake.frequency, 0.01f, 100.0f);
                        edited          = true;
                    }

                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
                if (edited) {
                    EditorAction action{EditorActionType::SetCameraShake};
                    action.id    = camera->id;
                    action.shake = shake;
                    submit(std::move(action));
                }
            }
            property::separator();
            if (property::actionButton(
                    "playback.refactorEditor.details.addKeyframeAtPlayhead"_tr().c_str(),
                    true,
                    property::ButtonVariant::Primary,
                    ICON_ADD_KEYFRAME
                )) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = ctx.state.currentTick;
                submit(std::move(action));
            }
            bool const canResetClips = !camera->locked && project->totalTicks > 0
                                    && std::ranges::none_of(camera->clips, &state::editing::model::Clip::locked);
            if (property::actionButton(
                    "playback.refactorEditor.details.resetTrackClipsToFull"_tr().c_str(),
                    canResetClips,
                    property::ButtonVariant::Subtle,
                    ICON_RESET
                )) {
                EditorAction action{EditorActionType::ResetClipsToFullRange};
                action.id = camera->id;
                submit(std::move(action));
            }
            property::separator();
            if (property::actionButton(
                    "playback.refactorEditor.details.deleteCamera"_tr().c_str(),
                    true,
                    property::ButtonVariant::Danger,
                    ICON_TRASH
                )) {
                EditorAction action{EditorActionType::DeleteCamera};
                action.id = camera->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
                ImGui::PopTextWrapPos();
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawKeyframes(PanelContext const& ctx) {
    auto const& selection = ctx.selection;
    if (selection.keyframeCount() <= 1) return false;
    auto const project = ctx.state.project;

    std::vector<state::editing::model::KeyframeRef> refs;
    std::optional<int>                              shared;
    bool                                            mixed = false;
    std::optional<int>                              sharedMode;
    bool                                            mixedMode              = false;
    bool                                            anyInterpolated        = false;
    bool                                            anyPerson              = false;
    bool                                            anyThirdPerson         = false;
    bool                                            anyCustomThirdPerson   = false;
    std::optional<bool>                             sharedThirdPersonCustom;
    bool                                            mixedThirdPersonCustom = false;
    bool                                            anyLookAtEnabled       = false;
    std::optional<bool>                             sharedLookAtEnabled;
    bool                                            mixedLookAtEnabled = false;
    std::optional<std::string>                      sharedLookAtTarget;
    bool                                            mixedLookAtTarget = false;
    std::optional<std::string>                      sharedSubject;
    bool                                            mixedSubject = false;
    std::optional<bool>                             sharedIgnore;
    bool                                            mixedIgnore = false;
    int                                             tracks      = 0;
    std::string                                     lastTrack;
    for (auto const& entry : selection.keyframes()) {
        refs.push_back({entry.trackId, entry.tick, std::nullopt});
        if (entry.trackId != lastTrack) {
            ++tracks;
            lastTrack = entry.trackId;
        }
        auto const* camera = findById(project->cameras, entry.trackId);
        if (!camera) continue;
        auto const key = camera->keysByTick.find(entry.tick);
        if (key == camera->keysByTick.end()) continue;
        int const mode = static_cast<int>(key->second.mode);
        if (!sharedMode) sharedMode = mode;
        else if (*sharedMode != mode) mixedMode = true;
        bool isEvaluator = false;
        if (key->second.mode == state::editing::model::CameraKeyframeMode::Interpolated) {
            anyInterpolated = true;
            isEvaluator     = true;
            if (!sharedLookAtEnabled) sharedLookAtEnabled = key->second.lookAtEnabled;
            else if (*sharedLookAtEnabled != key->second.lookAtEnabled) mixedLookAtEnabled = true;
            if (key->second.lookAtEnabled) {
                anyLookAtEnabled = true;
                if (!sharedLookAtTarget) sharedLookAtTarget = key->second.lookAtTarget;
                else if (*sharedLookAtTarget != key->second.lookAtTarget) mixedLookAtTarget = true;
            }
        } else {
            anyPerson = true;
            if (!sharedSubject) sharedSubject = key->second.subject;
            else if (*sharedSubject != key->second.subject) mixedSubject = true;
            if (key->second.mode == state::editing::model::CameraKeyframeMode::ThirdPerson) {
                anyThirdPerson = true;
                if (!sharedThirdPersonCustom) sharedThirdPersonCustom = key->second.thirdPersonCustom;
                else if (*sharedThirdPersonCustom != key->second.thirdPersonCustom) mixedThirdPersonCustom = true;
                if (key->second.thirdPersonCustom) {
                    anyCustomThirdPerson = true;
                    isEvaluator          = true;
                }
            }
        }
        if (isEvaluator) {
            int const type = static_cast<int>(key->second.interpolationType);
            if (!shared) shared = type;
            else if (*shared != type) mixed = true;
            if (!sharedIgnore) sharedIgnore = key->second.ignoreClipSpeed;
            else if (*sharedIgnore != key->second.ignoreClipSpeed) mixedIgnore = true;
        }
    }

    if (property::beginSection("playback.refactorEditor.details.multiKeyframe"_tr().c_str())) {
        property::textRow(
            "playback.refactorEditor.details.keyframeCount"_tr().c_str(),
            std::to_string(selection.keyframeCount()).c_str()
        );
        property::textRow("playback.refactorEditor.details.trackCount"_tr().c_str(), std::to_string(tracks).c_str());
        property::separator();
        {
            float const scale      = metrics::scale();
            float const labelWidth = measureLabelColumn({
                "playback.refactorEditor.details.lensMode"_tr(),
                "playback.refactorEditor.details.interpolation"_tr(),
            });
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
            if (ImGui::BeginTable(
                    "##batch-keyframe-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.lensMode"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                std::string const modePreview =
                    mixedMode || !sharedMode
                        ? "playback.refactorEditor.details.mixed"_tr()
                        : keyframeModeName(static_cast<state::editing::model::CameraKeyframeMode>(*sharedMode));
                if (widgets::beginCombo("##batch-lens-mode", modePreview.c_str())) {
                    for (int index = 0; index < 3; ++index) {
                        auto const value = static_cast<state::editing::model::CameraKeyframeMode>(index);
                        if (ImGui::Selectable(keyframeModeName(value).c_str(), !mixedMode && sharedMode == index)) {
                            EditorAction action{EditorActionType::SetKeyframesMode};
                            action.keyframeRefs = refs;
                            action.kind         = index;
                            ctx.submitAction(std::move(action));
                        }
                    }
                    widgets::endCombo();
                }

                if (anyInterpolated || anyCustomThirdPerson) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    std::string const preview =
                        mixed || !shared
                            ? "playback.refactorEditor.details.mixed"_tr()
                            : interpolationName(static_cast<state::editing::model::CameraInterpolationType>(*shared));
                    if (widgets::beginCombo("##batch-interpolation", preview.c_str())) {
                        for (int index = 0; index < 8; ++index) {
                            auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                            if (ImGui::Selectable(interpolationName(value).c_str(), !mixed && shared == index)) {
                                EditorAction action{EditorActionType::SetKeyframesInterpolation};
                                action.keyframeRefs = refs;
                                action.kind         = index;
                                ctx.submitAction(std::move(action));
                            }
                        }
                        widgets::endCombo();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
        if (anyThirdPerson) {
            property::separator();
            bool              customValue = !mixedThirdPersonCustom && sharedThirdPersonCustom.value_or(false);
            std::string const customLabel =
                mixedThirdPersonCustom
                    ? ("playback.refactorEditor.details.thirdPersonCustom"_tr() + " ("
                       + "playback.refactorEditor.details.mixed"_tr() + ")##batch-3p-custom")
                    : ("playback.refactorEditor.details.thirdPersonCustom"_tr() + "##batch-3p-custom");
            if (ImGui::Checkbox(customLabel.c_str(), &customValue)) {
                EditorAction action{EditorActionType::SetKeyframesThirdPersonCustom};
                action.keyframeRefs = refs;
                action.value        = customValue;
                ctx.submitAction(std::move(action));
            }
        }
        if (anyPerson) {
            property::separator();
            std::string const current =
                mixedSubject || !sharedSubject
                    ? "playback.refactorEditor.details.mixed"_tr()
                    : playerSubjectLabel(ctx.state.replayPlayers, ctx.state.replayEntities, *sharedSubject);
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeSubject"_tr().c_str());
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(current.c_str());
            ImGui::PopTextWrapPos();
            PlayerPickerResult picked;
            if (drawPlayerList(
                    "##keyframes-subject-list",
                    PlayerPickerMode::SegmentSubject,
                    ctx.state.replayPlayers,
                    ctx.state.replayEntities,
                    sharedSubject.value_or(std::string{}),
                    false,
                    picked,
                    mixedSubject
                )
                && (mixedSubject || !sharedSubject || picked.uuid != *sharedSubject)) {
                EditorAction action{EditorActionType::SetKeyframesSubject};
                action.keyframeRefs = refs;
                action.secondaryId  = std::move(picked.uuid);
                ctx.submitAction(std::move(action));
            }
        }
        if (anyInterpolated) {
            property::separator();
            bool              lookAtValue = !mixedLookAtEnabled && sharedLookAtEnabled.value_or(false);
            std::string const lookAtLabel =
                mixedLookAtEnabled
                    ? ("playback.refactorEditor.details.lookAtEntity"_tr() + " ("
                       + "playback.refactorEditor.details.mixed"_tr() + ")##batch-lookat-enabled")
                    : ("playback.refactorEditor.details.lookAtEntity"_tr() + "##batch-lookat-enabled");
            if (ImGui::Checkbox(lookAtLabel.c_str(), &lookAtValue)) {
                EditorAction action{EditorActionType::SetKeyframesLookAt};
                action.keyframeRefs  = refs;
                action.value         = lookAtValue;
                action.secondaryId   = sharedLookAtTarget.value_or(std::string{});
                action.secondaryTick = 1;
                ctx.submitAction(std::move(action));
            }
            if (anyLookAtEnabled) {
                bool const        targetMixed = mixedLookAtEnabled || mixedLookAtTarget || !sharedLookAtTarget;
                std::string const currentTarget =
                    targetMixed
                        ? "playback.refactorEditor.details.mixed"_tr()
                        : playerSubjectLabel(ctx.state.replayPlayers, ctx.state.replayEntities, *sharedLookAtTarget);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.lookAtTarget"_tr().c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(currentTarget.c_str());
                ImGui::PopTextWrapPos();
                PlayerPickerResult picked;
                if (drawPlayerList(
                        "##keyframes-lookat-list",
                        PlayerPickerMode::SegmentSubject,
                        ctx.state.replayPlayers,
                        ctx.state.replayEntities,
                        sharedLookAtTarget.value_or(std::string{}),
                        false,
                        picked,
                        targetMixed
                    )
                    && (targetMixed || picked.uuid != *sharedLookAtTarget)) {
                    EditorAction action{EditorActionType::SetKeyframesLookAt};
                    action.keyframeRefs  = refs;
                    action.value         = true;
                    action.secondaryId   = std::move(picked.uuid);
                    action.secondaryTick = 1;
                    ctx.submitAction(std::move(action));
                }
            }
        }
        if (anyInterpolated || anyCustomThirdPerson) {
            property::separator();
            bool              ignoreValue = !mixedIgnore && sharedIgnore.value_or(false);
            std::string const ignoreLabel =
                mixedIgnore ? ("playback.refactorEditor.details.ignoreClipSpeed"_tr() + " ("
                               + "playback.refactorEditor.details.mixed"_tr() + ")##batch-ignore-clip-speed")
                            : ("playback.refactorEditor.details.ignoreClipSpeed"_tr() + "##batch-ignore-clip-speed");
            if (ImGui::Checkbox(ignoreLabel.c_str(), &ignoreValue)) {
                EditorAction action{EditorActionType::SetKeyframesIgnoreClipSpeed};
                action.keyframeRefs = refs;
                action.value        = ignoreValue;
                ctx.submitAction(std::move(action));
            }
            widgets::itemTooltip("playback.refactorEditor.details.ignoreClipSpeedHint"_tr().c_str());
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.details.deleteKeyframes"_tr(selection.keyframeCount()).c_str(),
                true,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            EditorAction action{EditorActionType::DeleteCameraKeyframes};
            action.keyframeRefs = std::move(refs);
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.multiKeyframeHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        property::endSection();
    }
    return true;
}

void DetailsPanel::drawKeyframeInterpolationRows(
    PanelContext const&                          ctx,
    state::editing::model::CameraEntity const&   camera,
    int                                          originalTick,
    state::editing::model::CameraKeyframe const& keyframe
) {
    auto const submit        = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };
    int const  interpolation = static_cast<int>(keyframe.interpolationType);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    if (widgets::beginCombo("##interpolation", interpolationName(keyframe.interpolationType).c_str())) {
        for (int index = 0; index < 8; ++index) {
            auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
            if (ImGui::Selectable(interpolationName(value).c_str(), index == interpolation)) {
                EditorAction action{EditorActionType::SetKeyframeInterpolation};
                action.id   = camera.id;
                action.tick = originalTick;
                action.kind = index;
                submit(std::move(action));
            }
        }
        widgets::endCombo();
    }

    if (keyframe.interpolationType == state::editing::model::CameraInterpolationType::CubicBezier) {
        if (mBezierDragHandle != 0 && (mBezierDragCameraId != camera.id || mBezierDragTick != originalTick)) {
            mBezierDragHandle = 0;
        }
        int const prevHandle = mBezierDragHandle;
        drawBezierCurveRows(
            "bezier",
            camera.locked,
            keyframe.bezierCtrl1,
            keyframe.bezierCtrl2,
            mBezierDragHandle,
            mBezierDraftCtrl1,
            mBezierDraftCtrl2,
            [&](state::editing::model::Vec2 c1, state::editing::model::Vec2 c2) {
                EditorAction action{EditorActionType::SetKeyframeBezierControls};
                action.id          = camera.id;
                action.tick        = originalTick;
                action.bezierCtrl1 = c1;
                action.bezierCtrl2 = c2;
                submit(std::move(action));
            }
        );
        if (prevHandle == 0 && mBezierDragHandle != 0) {
            mBezierDragCameraId = camera.id;
            mBezierDragTick     = originalTick;
        }
    }
}

bool DetailsPanel::drawKeyframe(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selected = selection.getAs<state::editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return true;
        }
        auto const key = camera->keysByTick.find(selected->tick);
        if (key == camera->keysByTick.end()) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.cameraKeyframe"_tr().c_str())) {
            property::textRow("playback.refactorEditor.details.camera"_tr().c_str(), camera->name.c_str());
            property::separator();
            int const  originalTick  = key->first;
            bool const isFree        = key->second.mode == state::editing::model::CameraKeyframeMode::Interpolated;
            bool const isThirdPerson = key->second.mode == state::editing::model::CameraKeyframeMode::ThirdPerson;
            bool const isCustomThird = isThirdPerson && key->second.thirdPersonCustom;
            bool const isPerson      = !isFree;
            if (ctx.state.replayVisible && (isFree || isCustomThird)) {
                std::string const captureLabel = "playback.refactorEditor.details.captureFromView"_tr();
                std::string const jumpLabel    = "playback.refactorEditor.details.jumpToKeyframe"_tr();
                int const         viewChoice   = property::actionButtonPair(
                    {captureLabel.c_str(), ICON_CAMERA, property::ButtonVariant::Secondary, !camera->locked},
                    {jumpLabel.c_str(), ICON_EYE, property::ButtonVariant::Secondary, true}
                );
                if (viewChoice == 0) {
                    EditorAction action{EditorActionType::CaptureKeyframeFromView};
                    action.id   = camera->id;
                    action.tick = originalTick;
                    submit(std::move(action));
                } else if (viewChoice == 1) {
                    EditorAction action{EditorActionType::MoveViewToKeyframe};
                    action.id   = camera->id;
                    action.tick = originalTick;
                    submit(std::move(action));
                }
                property::separator();
            }
            ImGui::BeginDisabled(camera->locked);
            int         tick       = originalTick;
            float const scale      = metrics::scale();
            float const labelWidth = keyframeLabelWidth();
            static constexpr std::array axisLabels{"X", "Y", "Z"};
            static std::array const     axisColors{
                ImGui::ColorConvertU32ToFloat4(theme::kAxisX),
                ImGui::ColorConvertU32ToFloat4(theme::kAxisY),
                ImGui::ColorConvertU32ToFloat4(theme::kAxisZ),
            };
            auto const       yawLabel   = "playback.refactorEditor.details.yaw"_tr();
            auto const       pitchLabel = "playback.refactorEditor.details.pitch"_tr();
            auto const       rollLabel  = "playback.refactorEditor.details.roll"_tr();
            std::array const rotationLabels{yawLabel.c_str(), pitchLabel.c_str(), rollLabel.c_str()};

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
            if (ImGui::BeginTable(
                    "##keyframe-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("##tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = camera->id;
                    action.tick          = originalTick;
                    action.secondaryTick = tick;
                    submit(std::move(action));
                }

                int const currentMode = static_cast<int>(key->second.mode);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.lensMode"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (widgets::beginCombo("##lens-mode", keyframeModeName(key->second.mode).c_str())) {
                    for (int index = 0; index < 3; ++index) {
                        auto const value = static_cast<state::editing::model::CameraKeyframeMode>(index);
                        if (ImGui::Selectable(keyframeModeName(value).c_str(), index == currentMode)) {
                            EditorAction action{EditorActionType::SetKeyframeMode};
                            action.id   = camera->id;
                            action.tick = originalTick;
                            action.kind = index;
                            submit(std::move(action));
                        }
                    }
                    widgets::endCombo();
                }

                // Interpolated keyframes use explicit world pose and curve controls.
                if (isFree) {
                    float position[3] = {key->second.position.x, key->second.position.y, key->second.position.z};
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.position"_tr().c_str());
                    ImGui::TableNextColumn();
                    if (vectorInput("position", position, axisLabels, &axisColors, "%.3f")
                        && std::ranges::all_of(position, [](float value) { return std::isfinite(value); })) {
                        EditorAction action{EditorActionType::SetKeyframePosition};
                        action.id       = camera->id;
                        action.tick     = originalTick;
                        action.position = {position[0], position[1], position[2]};
                        submit(std::move(action));
                    }

                    float rotation[3] = {key->second.yaw, key->second.pitch, key->second.roll};
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.rotation"_tr().c_str());
                    ImGui::TableNextColumn();
                    if (vectorInput("rotation", rotation, rotationLabels, nullptr, "%.2f")
                        && std::ranges::all_of(rotation, [](float value) { return std::isfinite(value); })) {
                        EditorAction action{EditorActionType::SetKeyframeRotation};
                        action.id       = camera->id;
                        action.tick     = originalTick;
                        action.position = {rotation[0], rotation[1], rotation[2]};
                        submit(std::move(action));
                    }

                    float fov = key->second.fov;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.fov"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##fov", &fov, 0.0f, 0.0f, "%.2f°");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(fov)) {
                        EditorAction action{EditorActionType::SetKeyframeFov};
                        action.id    = camera->id;
                        action.tick  = originalTick;
                        action.speed = std::clamp(fov, 1.0f, 179.0f);
                        submit(std::move(action));
                    }

                    drawKeyframeInterpolationRows(ctx, *camera, originalTick, key->second);
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            if (isFree) {
                property::separator();
                bool lookAtEnabled = key->second.lookAtEnabled;
                if (ImGui::Checkbox(
                        ("playback.refactorEditor.details.lookAtEntity"_tr() + "##keyframe-lookat-enabled").c_str(),
                        &lookAtEnabled
                    )) {
                    EditorAction action{EditorActionType::SetKeyframeLookAt};
                    action.id          = camera->id;
                    action.tick        = originalTick;
                    action.value       = lookAtEnabled;
                    action.secondaryId = key->second.lookAtTarget;
                    action.speed       = key->second.lookAtOffsetY;
                    submit(std::move(action));
                }
                if (key->second.lookAtEnabled) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.lookAtRotationHint"_tr().c_str());
                    ImGui::PopTextWrapPos();

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
                    if (ImGui::BeginTable(
                            "##keyframe-lookat-properties",
                            2,
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                        )) {
                        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                        float offsetY = key->second.lookAtOffsetY;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.lookAtOffsetY"_tr().c_str());
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1.0f);
                        bool const offsetChanged =
                            ImGui::InputFloat("##keyframe-lookat-offsety", &offsetY, 0.1f, 0.5f, "%.2f");
                        if ((ImGui::IsItemDeactivatedAfterEdit() || (offsetChanged && !ImGui::IsItemActive()))
                            && std::isfinite(offsetY) && std::abs(offsetY - key->second.lookAtOffsetY) > 1e-5f) {
                            EditorAction action{EditorActionType::SetKeyframeLookAt};
                            action.id          = camera->id;
                            action.tick        = originalTick;
                            action.value       = true;
                            action.secondaryId = key->second.lookAtTarget;
                            action.speed       = offsetY;
                            submit(std::move(action));
                        }
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();

                    std::string const currentTarget =
                        playerSubjectLabel(ctx.state.replayPlayers, ctx.state.replayEntities, key->second.lookAtTarget);
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.lookAtTarget"_tr().c_str());
                    ImGui::SameLine();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(currentTarget.c_str());
                    ImGui::PopTextWrapPos();
                    PlayerPickerResult picked;
                    if (drawPlayerList(
                            "##keyframe-lookat-list",
                            PlayerPickerMode::SegmentSubject,
                            ctx.state.replayPlayers,
                            ctx.state.replayEntities,
                            key->second.lookAtTarget,
                            false,
                            picked
                        )
                        && picked.uuid != key->second.lookAtTarget) {
                        EditorAction action{EditorActionType::SetKeyframeLookAt};
                        action.id          = camera->id;
                        action.tick        = originalTick;
                        action.value       = true;
                        action.secondaryId = std::move(picked.uuid);
                        action.speed       = key->second.lookAtOffsetY;
                        submit(std::move(action));
                    }
                }
            }

            if (isPerson) {
                property::separator();
                if (isThirdPerson) {
                    bool customThird = key->second.thirdPersonCustom;
                    if (ImGui::Checkbox(
                            ("playback.refactorEditor.details.thirdPersonCustom"_tr() + "##keyframe-3p-custom").c_str(),
                            &customThird
                        )) {
                        EditorAction action{EditorActionType::SetKeyframeThirdPersonSettings};
                        action.id                          = camera->id;
                        action.tick                        = originalTick;
                        action.keyframe                    = key->second;
                        action.keyframe->thirdPersonCustom = customThird;
                        submit(std::move(action));
                    }
                }
                if (!isCustomThird) {
                    // The subject holds from this key until the next key on the same track (or end of replay).
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.personApplyHint"_tr().c_str());
                    ImGui::PopTextWrapPos();
                }
                std::string const current =
                    playerSubjectLabel(ctx.state.replayPlayers, ctx.state.replayEntities, key->second.subject);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeSubject"_tr().c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(current.c_str());
                ImGui::PopTextWrapPos();
                PlayerPickerResult picked;
                if (drawPlayerList(
                        "##keyframe-subject-list",
                        PlayerPickerMode::SegmentSubject,
                        ctx.state.replayPlayers,
                        ctx.state.replayEntities,
                        key->second.subject,
                        false,
                        picked
                    )
                    && picked.uuid != key->second.subject) {
                    EditorAction action{EditorActionType::SetKeyframeSubject};
                    action.id          = camera->id;
                    action.tick        = originalTick;
                    action.secondaryId = std::move(picked.uuid);
                    submit(std::move(action));
                }

                if (isCustomThird) {
                    property::separator();
                    auto const submitThirdPerson = [&](state::editing::model::CameraKeyframe updated,
                                                       bool                                  convertFollow = false) {
                        EditorAction action{EditorActionType::SetKeyframeThirdPersonSettings};
                        action.id       = camera->id;
                        action.tick     = originalTick;
                        action.value    = convertFollow;
                        action.keyframe = std::move(updated);
                        submit(std::move(action));
                    };

                    bool followRotation = key->second.thirdPersonFollowRotation;
                    if (ImGui::Checkbox(
                            ("playback.refactorEditor.details.thirdPersonFollowRotation"_tr()
                             + "##keyframe-3p-follow-rot")
                                .c_str(),
                            &followRotation
                        )) {
                        auto updated                      = key->second;
                        updated.thirdPersonFollowRotation = followRotation;
                        submitThirdPerson(std::move(updated), true);
                    }
                    bool collideBlocks = key->second.thirdPersonCollideBlocks;
                    if (ImGui::Checkbox(
                            ("playback.refactorEditor.details.thirdPersonCollideBlocks"_tr() + "##keyframe-3p-collide")
                                .c_str(),
                            &collideBlocks
                        )) {
                        auto updated                     = key->second;
                        updated.thirdPersonCollideBlocks = collideBlocks;
                        submitThirdPerson(std::move(updated));
                    }

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
                    if (ImGui::BeginTable(
                            "##keyframe-3p-properties",
                            2,
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                        )) {
                        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                        float distance = key->second.thirdPersonDistance;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.thirdPersonDistance"_tr().c_str());
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::InputFloat("##keyframe-3p-distance", &distance, 0.0f, 0.0f, "%.2f");
                        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(distance)) {
                            auto updated                = key->second;
                            updated.thirdPersonDistance = std::max(0.0f, distance);
                            submitThirdPerson(std::move(updated));
                        }

                        float offset[3] = {
                            key->second.thirdPersonOffset.x,
                            key->second.thirdPersonOffset.y,
                            key->second.thirdPersonOffset.z,
                        };
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.thirdPersonOffset"_tr().c_str());
                        ImGui::TableNextColumn();
                        if (vectorInput("3p-offset", offset, axisLabels, &axisColors, "%.3f")
                            && std::ranges::all_of(offset, [](float value) { return std::isfinite(value); })) {
                            auto updated              = key->second;
                            updated.thirdPersonOffset = {offset[0], offset[1], offset[2]};
                            submitThirdPerson(std::move(updated));
                        }

                        float rotation[3] = {
                            key->second.thirdPersonYaw,
                            key->second.thirdPersonPitch,
                            key->second.thirdPersonRoll,
                        };
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.rotation"_tr().c_str());
                        ImGui::TableNextColumn();
                        if (vectorInput("3p-rotation", rotation, rotationLabels, nullptr, "%.2f")
                            && std::ranges::all_of(rotation, [](float value) { return std::isfinite(value); })) {
                            auto updated             = key->second;
                            updated.thirdPersonYaw   = rotation[0];
                            updated.thirdPersonPitch = rotation[1];
                            updated.thirdPersonRoll  = rotation[2];
                            submitThirdPerson(std::move(updated));
                        }

                        float fov = key->second.thirdPersonFov;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.fov"_tr().c_str());
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::InputFloat("##keyframe-3p-fov", &fov, 0.0f, 0.0f, "%.2f°");
                        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(fov)) {
                            auto updated           = key->second;
                            updated.thirdPersonFov = std::clamp(fov, 1.0f, 179.0f);
                            submitThirdPerson(std::move(updated));
                        }

                        drawKeyframeInterpolationRows(ctx, *camera, originalTick, key->second);
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();
                }
            }
            if (isFree || isCustomThird) {
                property::separator();
                bool ignoreClipSpeed = key->second.ignoreClipSpeed;
                if (ImGui::Checkbox("playback.refactorEditor.details.ignoreClipSpeed"_tr().c_str(), &ignoreClipSpeed)) {
                    EditorAction action{EditorActionType::SetKeyframeIgnoreClipSpeed};
                    action.id    = camera->id;
                    action.tick  = originalTick;
                    action.value = ignoreClipSpeed;
                    submit(std::move(action));
                }
                widgets::itemTooltip("playback.refactorEditor.details.ignoreClipSpeedHint"_tr().c_str());
            }
            property::separator();
            if (property::actionButton(
                    "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    true,
                    property::ButtonVariant::Danger,
                    ICON_TRASH
                )) {
                EditorAction action{EditorActionType::DeleteCameraKeyframe};
                action.id   = camera->id;
                action.tick = originalTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
                ImGui::PopTextWrapPos();
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawMarker(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;

    if (auto const* selected = selection.getAs<state::editing::model::SelectedMarker>()) {
        auto const* marker = findById(project->markers, selected->markerId);
        if (!marker) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.markerMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.marker"_tr().c_str())) {
            if (mMarkerLabelId != marker->id || !mMarkerLabelEditing) {
                mMarkerLabelId     = marker->id;
                mMarkerLabelBuffer = {};
                marker->label.copy(mMarkerLabelBuffer.data(), mMarkerLabelBuffer.size() - 1);
            }
            float const scale      = metrics::scale();
            float const labelWidth = measureLabelColumn({
                "playback.refactorEditor.details.nameLabel"_tr(),
                "playback.refactorEditor.details.tick"_tr(),
            });
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
            if (ImGui::BeginTable(
                    "##marker-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.nameLabel"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##marker-label", mMarkerLabelBuffer.data(), mMarkerLabelBuffer.size());
                mMarkerLabelEditing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::string label = mMarkerLabelBuffer.data();
                    if (!label.empty() && label != marker->label) {
                        EditorAction action{EditorActionType::RenameMarker};
                        action.id   = marker->id;
                        action.name = std::move(label);
                        ctx.submitAction(std::move(action));
                    }
                }

                int tick = marker->tick;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("##marker-tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tick = std::clamp(tick, 0, ctx.state.totalTicks);
                    if (tick != marker->tick) {
                        EditorAction action{EditorActionType::MoveMarker};
                        action.id   = marker->id;
                        action.tick = tick;
                        ctx.submitAction(std::move(action));
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", formatTick(marker->tick).c_str());
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            property::separator();
            if (property::actionButton(
                    "playback.refactorEditor.timeline.seekToMarker"_tr().c_str(),
                    true,
                    property::ButtonVariant::Primary,
                    ICON_MARKER
                )) {
                ctx.commands.seekTo(marker->tick);
            }
            if (property::actionButton(
                    "playback.refactorEditor.details.deleteMarker"_tr().c_str(),
                    true,
                    property::ButtonVariant::Danger,
                    ICON_TRASH
                )) {
                EditorAction action{EditorActionType::DeleteMarker};
                action.id = marker->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawSpeedKeyframe(PanelContext const& ctx) {
    auto const selectedTick = ctx.selection.selectedSpeedKeyframe();
    if (!selectedTick) {
        mSpeedKeyDragging      = false;
        mSpeedKeyDragTick      = -1;
        mSpeedBezierDragHandle = 0;
        mSpeedBezierDragTick   = -1;
        return false;
    }

    auto const  project = ctx.state.project;
    auto const& track   = project->speedTrack;
    auto const  key     = track.keysByTick.find(*selectedTick);
    if (key == track.keysByTick.end()) {
        mSpeedKeyDragging      = false;
        mSpeedKeyDragTick      = -1;
        mSpeedBezierDragHandle = 0;
        mSpeedBezierDragTick   = -1;
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
        return true;
    }

    int const  originalTick = key->first;
    bool const editable     = !track.locked;
    if (mSpeedKeyDragging && (!editable || mSpeedKeyDragTick != originalTick)) {
        mSpeedKeyDragging = false;
        mSpeedKeyDragTick = -1;
    }
    if (mSpeedBezierDragHandle != 0 && (!editable || mSpeedBezierDragTick != originalTick)) {
        mSpeedBezierDragHandle = 0;
    }

    if (property::beginSection("playback.refactorEditor.speed.keyframeDetailsTitle"_tr().c_str())) {
        float const scale      = metrics::scale();
        float const labelWidth = measureLabelColumn({
            "playback.refactorEditor.details.tick"_tr(),
            "playback.refactorEditor.speed.speedValue"_tr(),
            "playback.refactorEditor.details.interpolation"_tr(),
            "playback.refactorEditor.details.bezierCurve"_tr(),
            "playback.refactorEditor.details.bezierControl1"_tr(),
            "playback.refactorEditor.details.bezierControl2"_tr(),
        });

        float const displayedSpeed =
            (mSpeedKeyDragging && mSpeedKeyDragTick == originalTick) ? mSpeedKeyDragValue : key->second.speed;
        auto const submitSpeed = [&](float nextSpeed) {
            float const clamped = state::editing::model::clampSpeedKeyframeValue(nextSpeed);
            if (std::abs(clamped - key->second.speed) > 1e-5f) {
                EditorAction action{EditorActionType::SetSpeedKeyframeValue};
                action.tick       = originalTick;
                action.speedValue = clamped;
                ctx.submitAction(std::move(action));
            }
        };

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##speed-keyframe-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int tick = originalTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(originalTick), [&] {
                ImGui::InputInt("##speed-key-tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tick = std::clamp(tick, 0, std::max(0, project->totalTicks));
                    if (tick != originalTick && !track.keysByTick.contains(tick)) {
                        EditorAction action{EditorActionType::MoveSpeedKeyframe};
                        action.tick          = originalTick;
                        action.secondaryTick = tick;
                        ctx.submitAction(std::move(action));
                        ctx.selection.selectSpeedKeyframe(tick);
                    }
                }
            });

            float speedInput = displayedSpeed;
            char  pctBuf[24]{};
            std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", static_cast<double>(displayedSpeed * 100.0f));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.speed.speedValue"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(pctBuf, [&] {
                ImGui::InputFloat("##speed-key-value", &speedInput, 0.0f, 0.0f, "%.2fx");
                if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(speedInput)) {
                    submitSpeed(speedInput);
                }
            });

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // Piecewise log2 speed slider centered at 1.0x covering [0.01x, 32x].
        {
            static constexpr float kSpeedKeySnapPresets[] =
                {0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
            drawLogSpeedSlider(
                "##speed-key-slider",
                editable,
                key->second.speed,
                state::editing::model::MinSpeedKeyframeValue,
                state::editing::model::MaxSpeedKeyframeValue,
                state::editing::model::MinSpeedKeyframeValue,
                state::editing::model::MaxSpeedKeyframeValue,
                kSpeedKeySnapPresets,
                mSpeedKeyDragging,
                mSpeedKeyDragValue,
                submitSpeed,
                [](float sliderSpeed) {
                    char tip[48]{};
                    std::snprintf(
                        tip,
                        sizeof(tip),
                        "%.2fx (%.0f%%)",
                        static_cast<double>(sliderSpeed),
                        static_cast<double>(sliderSpeed * 100.0f)
                    );
                    return std::string(tip);
                }
            );
            if (mSpeedKeyDragging) mSpeedKeyDragTick = originalTick;
            else mSpeedKeyDragTick = -1;
        }

        // Quick speed preset chips.
        {
            static constexpr float kSpeedKeyPresets[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
            drawSpeedPresetChips(displayedSpeed, kSpeedKeyPresets, submitSpeed);
        }

        property::separator();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##speed-keyframe-curve",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int const currentInterpolation = static_cast<int>(key->second.interpolationType);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (widgets::beginCombo(
                    "##speed-key-interpolation",
                    interpolationName(key->second.interpolationType).c_str()
                )) {
                for (int index = 0; index < 8; ++index) {
                    auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                    if (ImGui::Selectable(interpolationName(value).c_str(), index == currentInterpolation)) {
                        EditorAction action{EditorActionType::SetSpeedKeyframeInterpolation};
                        action.tick = originalTick;
                        action.kind = index;
                        ctx.submitAction(std::move(action));
                    }
                }
                widgets::endCombo();
            }

            if (key->second.interpolationType == state::editing::model::CameraInterpolationType::CubicBezier) {
                int const prevHandle = mSpeedBezierDragHandle;
                drawBezierCurveRows(
                    "speed-bezier",
                    !editable,
                    key->second.bezierCtrl1,
                    key->second.bezierCtrl2,
                    mSpeedBezierDragHandle,
                    mSpeedBezierDraftCtrl1,
                    mSpeedBezierDraftCtrl2,
                    [&](state::editing::model::Vec2 c1, state::editing::model::Vec2 c2) {
                        EditorAction action{EditorActionType::SetSpeedKeyframeBezierControls};
                        action.tick        = originalTick;
                        action.bezierCtrl1 = c1;
                        action.bezierCtrl2 = c2;
                        ctx.submitAction(std::move(action));
                    }
                );
                if (prevHandle == 0 && mSpeedBezierDragHandle != 0) {
                    mSpeedBezierDragTick = originalTick;
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.speed.deleteKeyframe"_tr().c_str(),
                true,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            EditorAction action{EditorActionType::DeleteSpeedKeyframe};
            action.tick = originalTick;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        ImGui::EndDisabled();
        if (!editable) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    } else {
        mSpeedKeyDragging      = false;
        mSpeedKeyDragTick      = -1;
        mSpeedBezierDragHandle = 0;
        mSpeedBezierDragTick   = -1;
    }
    return true;
}

bool DetailsPanel::drawSpeedTrack(PanelContext const& ctx) {
    if (!ctx.selection.isSpeedTrackSelected()) return false;

    auto const  project = ctx.state.project;
    auto const& track   = project->speedTrack;
    if (!track.isVisible()) return false;
    if (property::beginSection("playback.refactorEditor.speed.trackDetailsTitle"_tr().c_str())) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.speed.curveHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        property::separator();

        property::textRow(
            "playback.refactorEditor.speed.keyframeCount"_tr().c_str(),
            std::to_string(track.keysByTick.size()).c_str()
        );
        if (track.keysByTick.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.speed.emptyHint"_tr().c_str());
            ImGui::PopTextWrapPos();
        } else {
            float minSpeed = track.keysByTick.begin()->second.speed;
            float maxSpeed = minSpeed;
            for (auto const& [tick, kf] : track.keysByTick) {
                minSpeed = std::min(minSpeed, kf.speed);
                maxSpeed = std::max(maxSpeed, kf.speed);
            }
            char rangeBuf[48]{};
            std::snprintf(
                rangeBuf,
                sizeof(rangeBuf),
                "%.2fx - %.2fx",
                static_cast<double>(minSpeed),
                static_cast<double>(maxSpeed)
            );
            property::textRow("playback.refactorEditor.speed.speedRange"_tr().c_str(), rangeBuf);
        }

        property::separator();
        bool enabled = track.enabled;
        if (ImGui::Checkbox("playback.refactorEditor.speed.toggleEnabled"_tr().c_str(), &enabled)) {
            EditorAction action{EditorActionType::SetSpeedTrackEnabled};
            action.value = enabled;
            ctx.submitAction(std::move(action));
        }
        bool locked = track.locked;
        if (ImGui::Checkbox("playback.refactorEditor.speed.toggleLocked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetSpeedTrackLocked};
            action.value = locked;
            ctx.submitAction(std::move(action));
        }

        property::separator();
        int const  playheadTick = std::clamp(ctx.state.currentTick, 0, std::max(0, project->totalTicks));
        bool const canAdd       = !track.locked && !track.keysByTick.contains(playheadTick);
        if (property::actionButton(
                "playback.refactorEditor.speed.addKeyframeAtPlayhead"_tr().c_str(),
                canAdd,
                property::ButtonVariant::Primary,
                ICON_ADD_KEYFRAME
            )) {
            float const sampled =
                static_cast<float>(keyframe::sampleSpeedTrackAt(track, static_cast<long double>(playheadTick)));
            EditorAction action{EditorActionType::AddSpeedKeyframe};
            action.tick       = playheadTick;
            action.speedValue = state::editing::model::clampSpeedKeyframeValue(sampled);
            ctx.submitAction(std::move(action));
        }
        bool const canClear = !track.locked && !track.keysByTick.empty();
        if (property::actionButton(
                "playback.refactorEditor.speed.clearKeyframes"_tr().c_str(),
                canClear,
                property::ButtonVariant::Subtle,
                ICON_RESET
            )) {
            ctx.submitAction(EditorAction{EditorActionType::ClearSpeedKeyframes});
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.speed.removeTrack"_tr().c_str(),
                !track.locked,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            ctx.submitAction(EditorAction{EditorActionType::RemoveSpeedTrack});
            ctx.selection.clear();
        }
        if (track.locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    }
    return true;
}

bool DetailsPanel::drawFreezeKeyframe(PanelContext const& ctx) {
    auto const selectedTick = ctx.selection.selectedFreezeKeyframe();
    if (!selectedTick) {
        mFreezeDelayDragging = false;
        mFreezeDelayDragTick = -1;
        return false;
    }

    auto const  project = ctx.state.project;
    auto const& track   = project->freezeTrack;
    auto const  key     = track.keysByTick.find(*selectedTick);
    if (key == track.keysByTick.end()) {
        mFreezeDelayDragging = false;
        mFreezeDelayDragTick = -1;
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
        return true;
    }

    int const  originalTick = key->first;
    bool const editable     = !track.locked;
    if (mFreezeDelayDragging && (!editable || !key->second.frozen || mFreezeDelayDragTick != originalTick)) {
        mFreezeDelayDragging = false;
        mFreezeDelayDragTick = -1;
    }

    if (property::beginSection("playback.refactorEditor.freeze.keyframeDetailsTitle"_tr().c_str())) {
        float const scale      = metrics::scale();
        float const labelWidth = measureLabelColumn({"playback.refactorEditor.details.tick"_tr()});

        auto const submitFreeze = [&](bool nextFrozen, int nextDelay) {
            int const clampedDelay = state::editing::model::clampFreezeDelayTicks(nextDelay);
            if (nextFrozen != key->second.frozen || clampedDelay != key->second.frozenDelay) {
                EditorAction action{EditorActionType::SetFreezeKeyframe};
                action.tick  = originalTick;
                action.value = nextFrozen;
                action.kind  = clampedDelay;
                ctx.submitAction(std::move(action));
            }
        };

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##freeze-keyframe-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int tick = originalTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(originalTick), [&] {
                ImGui::InputInt("##freeze-key-tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tick = std::clamp(tick, 0, std::max(0, project->totalTicks));
                    if (tick != originalTick && !track.keysByTick.contains(tick)) {
                        EditorAction action{EditorActionType::MoveFreezeKeyframe};
                        action.tick          = originalTick;
                        action.secondaryTick = tick;
                        ctx.submitAction(std::move(action));
                        ctx.selection.selectFreezeKeyframe(tick);
                    }
                }
            });

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        bool frozen = key->second.frozen;
        if (ImGui::Checkbox("playback.refactorEditor.freeze.frozen"_tr().c_str(), &frozen)) {
            submitFreeze(frozen, key->second.frozenDelay);
        }

        property::separator();
        ImGui::BeginDisabled(!key->second.frozen);
        ImGui::TextUnformatted("playback.refactorEditor.freeze.slowdownDelay"_tr().c_str());

        int const displayedDelay = (mFreezeDelayDragging && mFreezeDelayDragTick == originalTick)
                                     ? mFreezeDelayDragValue
                                     : key->second.frozenDelay;
        int       delayDraft     = displayedDelay;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt(
            "##freeze-delay",
            &delayDraft,
            state::editing::model::MinFreezeDelayTicks,
            state::editing::model::MaxFreezeDelayTicks,
            "%dt",
            ImGuiSliderFlags_AlwaysClamp
        );
        // Hold slider edits in a local draft so dragging commits a single undo entry on release.
        if (ImGui::IsItemActive()) {
            mFreezeDelayDragging  = true;
            mFreezeDelayDragTick  = originalTick;
            mFreezeDelayDragValue = state::editing::model::clampFreezeDelayTicks(delayDraft);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            submitFreeze(key->second.frozen, delayDraft);
        }
        if (!ImGui::IsItemActive()) {
            mFreezeDelayDragging = false;
            mFreezeDelayDragTick = -1;
        }

        {
            static constexpr int kFreezeDelayPresets[] = {0, 3, 5, 10};
            drawPresetChips<int>(
                kFreezeDelayPresets,
                [](int preset) {
                    char buf[16]{};
                    std::snprintf(buf, sizeof(buf), "%dt", preset);
                    return std::string(buf);
                },
                [displayedDelay](int preset) { return displayedDelay == preset; },
                [](int) { return theme::kAccent; },
                [&](int preset) { submitFreeze(key->second.frozen, preset); }
            );
        }

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.freeze.slowdownDelayHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndDisabled();

        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.freeze.deleteKeyframe"_tr().c_str(),
                true,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            mFreezeDelayDragging = false;
            mFreezeDelayDragTick = -1;
            EditorAction action{EditorActionType::DeleteFreezeKeyframe};
            action.tick = originalTick;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        ImGui::EndDisabled();
        if (!editable) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    } else {
        mFreezeDelayDragging = false;
        mFreezeDelayDragTick = -1;
    }
    return true;
}

bool DetailsPanel::drawFreezeTrack(PanelContext const& ctx) {
    if (!ctx.selection.isFreezeTrackSelected()) return false;

    auto const  project = ctx.state.project;
    auto const& track   = project->freezeTrack;
    if (!track.isVisible()) return false;
    if (property::beginSection("playback.refactorEditor.freeze.trackDetailsTitle"_tr().c_str())) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.freeze.trackHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        property::separator();

        property::textRow(
            "playback.refactorEditor.freeze.keyframeCount"_tr().c_str(),
            std::to_string(track.keysByTick.size()).c_str()
        );
        if (track.keysByTick.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.freeze.emptyHint"_tr().c_str());
            ImGui::PopTextWrapPos();
        }

        property::separator();
        bool enabled = track.enabled;
        if (ImGui::Checkbox("playback.refactorEditor.details.enabled"_tr().c_str(), &enabled)) {
            EditorAction action{EditorActionType::SetFreezeTrackEnabled};
            action.value = enabled;
            ctx.submitAction(std::move(action));
        }
        bool locked = track.locked;
        if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetFreezeTrackLocked};
            action.value = locked;
            ctx.submitAction(std::move(action));
        }

        property::separator();
        int const  playheadTick = std::clamp(ctx.state.currentTick, 0, std::max(0, project->totalTicks));
        bool const canAdd       = !track.locked && !track.keysByTick.contains(playheadTick);
        if (property::actionButton(
                "playback.refactorEditor.freeze.addKeyframeAtPlayhead"_tr().c_str(),
                canAdd,
                property::ButtonVariant::Primary,
                ICON_ADD_KEYFRAME
            )) {
            auto const   nextIt     = track.keysByTick.upper_bound(playheadTick);
            bool const   prevFrozen = nextIt != track.keysByTick.begin() && std::prev(nextIt)->second.frozen;
            EditorAction action{EditorActionType::AddFreezeKeyframe};
            action.tick  = playheadTick;
            action.value = !prevFrozen;
            action.kind  = 0;
            ctx.submitAction(std::move(action));
        }
        bool const canClear = !track.locked && !track.keysByTick.empty();
        if (property::actionButton(
                "playback.refactorEditor.freeze.clearKeyframes"_tr().c_str(),
                canClear,
                property::ButtonVariant::Subtle,
                ICON_RESET
            )) {
            ctx.submitAction(EditorAction{EditorActionType::ClearFreezeKeyframes});
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.freeze.removeTrack"_tr().c_str(),
                !track.locked,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            ctx.submitAction(EditorAction{EditorActionType::RemoveFreezeTrack});
            ctx.selection.clear();
        }
        if (track.locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    }
    return true;
}

bool DetailsPanel::drawTimelapseKeyframe(PanelContext const& ctx) {
    auto const selectedTick = ctx.selection.selectedTimelapseKeyframe();
    if (!selectedTick) {
        mTimelapseTextTick.reset();
        mTimelapseTextEditing = false;
        return false;
    }

    auto const  project = ctx.state.project;
    auto const& track   = project->timelapseTrack;
    auto const  key     = track.keysByTick.find(*selectedTick);
    if (key == track.keysByTick.end()) {
        mTimelapseTextTick.reset();
        mTimelapseTextEditing = false;
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
        return true;
    }

    int const  originalTick = key->first;
    bool const editable     = !track.locked;
    if (!editable) {
        mTimelapseTextEditing = false;
    }

    auto const syncTextBuffer = [&](int tickKey, int outTicks) {
        mTimelapseTextTick = tickKey;
        mTimelapseTextBuffer.fill('\0');
        std::string const formatted = formatTick(outTicks);
        formatted.copy(mTimelapseTextBuffer.data(), mTimelapseTextBuffer.size() - 1);
    };

    if (property::beginSection("playback.refactorEditor.timelapse.keyframeDetailsTitle"_tr().c_str())) {
        float const scale      = metrics::scale();
        float const labelWidth = measureLabelColumn({
            "playback.refactorEditor.details.tick"_tr(),
            "playback.refactorEditor.timelapse.outputTime"_tr(),
            "playback.refactorEditor.timelapse.outputTicks"_tr(),
        });

        auto const submitOutputTicks = [&](int nextOutputTicks) {
            int const clamped = state::editing::model::clampTimelapseOutputTicks(nextOutputTicks);
            if (clamped != key->second.outputTicks) {
                EditorAction action{EditorActionType::SetTimelapseKeyframeOutputTicks};
                action.tick          = originalTick;
                action.secondaryTick = clamped;
                ctx.submitAction(std::move(action));
            }
        };

        if (mTimelapseTextTick != originalTick || !mTimelapseTextEditing) {
            syncTextBuffer(originalTick, key->second.outputTicks);
        }

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##timelapse-keyframe-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int tick = originalTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(originalTick), [&] {
                ImGui::InputInt("##timelapse-key-tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tick = std::clamp(tick, 0, std::max(0, project->totalTicks));
                    if (tick != originalTick && !track.keysByTick.contains(tick)) {
                        mTimelapseTextTick.reset();
                        mTimelapseTextEditing = false;
                        EditorAction action{EditorActionType::MoveTimelapseKeyframe};
                        action.tick          = originalTick;
                        action.secondaryTick = tick;
                        ctx.submitAction(std::move(action));
                        ctx.selection.selectTimelapseKeyframe(tick);
                    }
                }
            });

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timelapse.outputTime"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##timelapse-output-time", mTimelapseTextBuffer.data(), mTimelapseTextBuffer.size());
            mTimelapseTextEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (auto const parsed = keyframe::parseTimelapseDurationTicks(mTimelapseTextBuffer.data())) {
                    submitOutputTicks(*parsed);
                    syncTextBuffer(originalTick, *parsed);
                } else {
                    syncTextBuffer(originalTick, key->second.outputTicks);
                }
            }

            int outputTicks = key->second.outputTicks;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timelapse.outputTicks"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##timelapse-output-ticks", &outputTicks, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                int const clamped = state::editing::model::clampTimelapseOutputTicks(outputTicks);
                submitOutputTicks(clamped);
                syncTextBuffer(originalTick, clamped);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.timelapse.outputTimeHint"_tr().c_str());
        ImGui::PopTextWrapPos();

        bool const hasPrev = key != track.keysByTick.begin();
        auto const prevIt  = hasPrev ? std::prev(key) : track.keysByTick.end();
        int const  baseOut = hasPrev ? prevIt->second.outputTicks : 0;
        {
            static constexpr int kTimelapseDeltaSecPresets[] = {1, 5, 10, 30};
            ImGui::PushID("timelapse-delta-presets");
            drawPresetChips<int>(
                kTimelapseDeltaSecPresets,
                [](int sec) {
                    char buf[16]{};
                    std::snprintf(buf, sizeof(buf), "+%ds", sec);
                    return std::string(buf);
                },
                [&](int sec) {
                    int const target =
                        state::editing::model::clampTimelapseOutputTicks(baseOut + sec * widgets::kTicksPerSecond);
                    return key->second.outputTicks == target;
                },
                [](int) { return theme::kAccent; },
                [&](int sec) {
                    int const target =
                        state::editing::model::clampTimelapseOutputTicks(baseOut + sec * widgets::kTicksPerSecond);
                    submitOutputTicks(target);
                    syncTextBuffer(originalTick, target);
                }
            );
            ImGui::PopID();
        }
        ImGui::EndDisabled();

        if (track.keysByTick.size() < 2) {
            property::separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(theme::kWarning),
                "%s",
                "playback.refactorEditor.timelapse.requiresTwoKeyframes"_tr().c_str()
            );
            ImGui::PopTextWrapPos();
        } else if (hasPrev) {
            property::separator();
            if (key->second.outputTicks <= prevIt->second.outputTicks) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(
                    ImGui::ColorConvertU32ToFloat4(theme::kError),
                    "%s",
                    "playback.refactorEditor.timelapse.invalidOrderHint"_tr().c_str()
                );
                ImGui::PopTextWrapPos();
            } else {
                int const         deltaOut = key->second.outputTicks - prevIt->second.outputTicks;
                int const         deltaIn  = originalTick - prevIt->first;
                std::string const segDur   = formatTick(deltaOut) + " (" + std::to_string(deltaOut) + "t)";
                double const      rawSpeed = static_cast<double>(deltaIn) / static_cast<double>(deltaOut);
                double const      speed    = std::clamp(
                    rawSpeed,
                    state::editing::model::MinTimelapseSpeed,
                    state::editing::model::MaxTimelapseSpeed
                );
                char speedBuf[32]{};
                std::snprintf(speedBuf, sizeof(speedBuf), "%.2fx", speed);
                property::textRow("playback.refactorEditor.timelapse.segmentDuration"_tr().c_str(), segDur.c_str());
                property::textRow("playback.refactorEditor.timelapse.effectiveSpeed"_tr().c_str(), speedBuf);
            }
        }

        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.timelapse.deleteKeyframe"_tr().c_str(),
                editable,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            mTimelapseTextTick.reset();
            mTimelapseTextEditing = false;
            EditorAction action{EditorActionType::DeleteTimelapseKeyframe};
            action.tick = originalTick;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        if (!editable) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    } else {
        mTimelapseTextTick.reset();
        mTimelapseTextEditing = false;
    }
    return true;
}

bool DetailsPanel::drawTimelapseTrack(PanelContext const& ctx) {
    if (!ctx.selection.isTimelapseTrackSelected()) return false;

    auto const  project = ctx.state.project;
    auto const& track   = project->timelapseTrack;
    if (!track.isVisible()) return false;

    if (property::beginSection("playback.refactorEditor.timelapse.trackDetailsTitle"_tr().c_str())) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.timelapse.trackHint"_tr().c_str());
        if (keyframe::hasActiveTimelapseTrack(*project)) {
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(theme::kWarning),
                "%s  %s",
                ICON_INFO,
                "playback.refactorEditor.timelapse.overrideNotice"_tr().c_str()
            );
        }
        ImGui::PopTextWrapPos();
        property::separator();

        auto const   segments = keyframe::collectTimelapseSegments(track);
        size_t const validCount =
            static_cast<size_t>(std::ranges::count_if(segments, &keyframe::TimelapseSegment::valid));
        property::textRow(
            "playback.refactorEditor.timelapse.keyframeCount"_tr().c_str(),
            std::to_string(track.keysByTick.size()).c_str()
        );
        property::textRow(
            "playback.refactorEditor.timelapse.validSegments"_tr().c_str(),
            std::to_string(validCount).c_str()
        );
        if (track.keysByTick.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.timelapse.emptyHint"_tr().c_str());
            ImGui::PopTextWrapPos();
        } else if (track.keysByTick.size() == 1) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(theme::kWarning),
                "%s",
                "playback.refactorEditor.timelapse.requiresTwoKeyframes"_tr().c_str()
            );
            ImGui::PopTextWrapPos();
        }

        property::separator();
        bool enabled = track.enabled;
        if (ImGui::Checkbox("playback.refactorEditor.details.enabled"_tr().c_str(), &enabled)) {
            EditorAction action{EditorActionType::SetTimelapseTrackEnabled};
            action.value = enabled;
            ctx.submitAction(std::move(action));
        }
        bool locked = track.locked;
        if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetTimelapseTrackLocked};
            action.value = locked;
            ctx.submitAction(std::move(action));
        }

        property::separator();
        int const  playheadTick = std::clamp(ctx.state.currentTick, 0, std::max(0, project->totalTicks));
        bool const canAdd       = !track.locked && !track.keysByTick.contains(playheadTick);
        if (property::actionButton(
                "playback.refactorEditor.timelapse.addKeyframeAtPlayhead"_tr().c_str(),
                canAdd,
                property::ButtonVariant::Primary,
                ICON_ADD_KEYFRAME
            )) {
            EditorAction action{EditorActionType::AddTimelapseKeyframe};
            action.tick          = playheadTick;
            action.secondaryTick = keyframe::suggestTimelapseOutputTicks(track, playheadTick);
            ctx.submitAction(std::move(action));
        }
        bool const canClear = !track.locked && !track.keysByTick.empty();
        if (property::actionButton(
                "playback.refactorEditor.timelapse.clearKeyframes"_tr().c_str(),
                canClear,
                property::ButtonVariant::Subtle,
                ICON_RESET
            )) {
            ctx.submitAction(EditorAction{EditorActionType::ClearTimelapseKeyframes});
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.timelapse.removeTrack"_tr().c_str(),
                !track.locked,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            ctx.submitAction(EditorAction{EditorActionType::RemoveTimelapseTrack});
            ctx.selection.clear();
        }
        if (track.locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    }
    return true;
}

bool DetailsPanel::drawAudioTrack(PanelContext const& ctx) {
    auto const selectedTrackId = ctx.selection.selectedAudioTrackId();
    if (!selectedTrackId) {
        mAudioTrackNameId.clear();
        mAudioTrackNameBuffer.fill('\0');
        mAudioTrackNameEditing = false;
        mAudioTrackVolumeDragId.clear();
        mAudioTrackVolumeDragging  = false;
        mAudioTrackVolumeDragValue = 1.0f;
        return false;
    }

    auto const  project = ctx.state.project;
    auto const* track   = state::editing::ClipOps::findAudioTrack(*project, *selectedTrackId);
    if (!track) {
        mAudioTrackNameId.clear();
        mAudioTrackNameBuffer.fill('\0');
        mAudioTrackNameEditing = false;
        mAudioTrackVolumeDragId.clear();
        mAudioTrackVolumeDragging  = false;
        mAudioTrackVolumeDragValue = 1.0f;
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.noSelection"_tr().c_str());
        return true;
    }

    bool const editable = !track->locked;
    if (!editable) {
        mAudioTrackNameEditing = false;
        mAudioTrackVolumeDragId.clear();
        mAudioTrackVolumeDragging = false;
    }

    if (property::beginSection("playback.refactorEditor.audio.trackDetailsTitle"_tr().c_str())) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.audio.trackHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        property::separator();

        if (mAudioTrackNameId != track->id || !mAudioTrackNameEditing) {
            mAudioTrackNameId     = track->id;
            mAudioTrackNameBuffer = {};
            track->name.copy(mAudioTrackNameBuffer.data(), mAudioTrackNameBuffer.size() - 1);
        }

        float const scale      = metrics::scale();
        float const labelWidth = measureLabelColumn({
            "playback.refactorEditor.details.nameLabel"_tr(),
            "playback.refactorEditor.audio.clipCount"_tr(),
        });

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##audio-track-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.nameLabel"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!editable);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##audio-track-name", mAudioTrackNameBuffer.data(), mAudioTrackNameBuffer.size());
            mAudioTrackNameEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string name = mAudioTrackNameBuffer.data();
                if (!name.empty() && name != track->name) {
                    EditorAction action{EditorActionType::RenameAudioTrack};
                    action.id   = track->id;
                    action.name = std::move(name);
                    ctx.submitAction(std::move(action));
                } else {
                    mAudioTrackNameBuffer = {};
                    track->name.copy(mAudioTrackNameBuffer.data(), mAudioTrackNameBuffer.size() - 1);
                }
            }
            ImGui::EndDisabled();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.clipCount"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(std::to_string(track->clips.size()).c_str());

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        if (track->clips.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.emptyHint"_tr().c_str());
            ImGui::PopTextWrapPos();
        }

        property::separator();
        ImGui::BeginDisabled(!editable);
        drawAudioVolumeControl(
            "audio-track",
            editable,
            track->volume,
            track->id,
            mAudioTrackVolumeDragId,
            mAudioTrackVolumeDragging,
            mAudioTrackVolumeDragValue,
            [&](float nextVolume) {
                EditorAction action{EditorActionType::SetAudioTrackVolume};
                action.id     = track->id;
                action.volume = nextVolume;
                ctx.submitAction(std::move(action));
            }
        );

        property::separator();
        bool muted = track->muted;
        if (ImGui::Checkbox("playback.refactorEditor.audio.muted"_tr().c_str(), &muted)) {
            EditorAction action{EditorActionType::SetAudioTrackMuted};
            action.id    = track->id;
            action.value = muted;
            ctx.submitAction(std::move(action));
        }
        bool enabled = track->enabled;
        if (ImGui::Checkbox("playback.refactorEditor.details.enabled"_tr().c_str(), &enabled)) {
            EditorAction action{EditorActionType::SetAudioTrackEnabled};
            action.id    = track->id;
            action.value = enabled;
            ctx.submitAction(std::move(action));
        }
        ImGui::EndDisabled();

        bool locked = track->locked;
        if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetAudioTrackLocked};
            action.id    = track->id;
            action.value = locked;
            ctx.submitAction(std::move(action));
        }

        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.audio.addAudioFile"_tr().c_str(),
                editable,
                property::ButtonVariant::Primary,
                ICON_AUDIO
            )) {
            if (auto const picked = utils::pickAudioFile()) {
                EditorAction action{EditorActionType::AddAudioClip};
                action.id   = track->id;
                action.path = *picked;
                action.tick = std::clamp(ctx.state.currentTick, 0, std::max(0, project->totalTicks));
                ctx.submitAction(std::move(action));
            }
        }
        if (property::actionButton(
                "playback.refactorEditor.audio.removeTrack"_tr().c_str(),
                editable,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            EditorAction action{EditorActionType::RemoveAudioTrack};
            action.id = track->id;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        if (track->locked) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    } else {
        mAudioTrackNameEditing    = false;
        mAudioTrackVolumeDragging = false;
        mAudioTrackVolumeDragId.clear();
    }
    return true;
}

bool DetailsPanel::drawAudioClip(PanelContext const& ctx) {
    auto const selected = ctx.selection.selectedAudioClip();
    if (!selected) {
        mAudioClipPathId.clear();
        mAudioClipPathBuffer.fill('\0');
        mAudioClipPathEditing = false;
        mAudioClipVolumeDragId.clear();
        mAudioClipVolumeDragging  = false;
        mAudioClipVolumeDragValue = 1.0f;
        return false;
    }

    auto const  project = ctx.state.project;
    auto const* track   = state::editing::ClipOps::findAudioTrack(*project, selected->trackId);
    auto const* clip    = track ? state::editing::ClipOps::findAudioClip(*track, selected->clipId) : nullptr;
    if (!track || !clip) {
        mAudioClipPathId.clear();
        mAudioClipPathBuffer.fill('\0');
        mAudioClipPathEditing = false;
        mAudioClipVolumeDragId.clear();
        mAudioClipVolumeDragging  = false;
        mAudioClipVolumeDragValue = 1.0f;
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.clipMissing"_tr().c_str());
        return true;
    }

    bool const editable = !track->locked;
    if (!editable) {
        mAudioClipPathEditing    = false;
        mAudioClipVolumeDragging = false;
        mAudioClipVolumeDragId.clear();
    }

    if (property::beginSection("playback.refactorEditor.audio.clipDetailsTitle"_tr().c_str())) {
        if (mAudioClipPathId != clip->id || !mAudioClipPathEditing) {
            mAudioClipPathId     = clip->id;
            mAudioClipPathBuffer = {};
            clip->filePath.copy(mAudioClipPathBuffer.data(), mAudioClipPathBuffer.size() - 1);
        }

        auto const submitProps = [&](std::string const& filePath,
                                     double             trimStartSeconds,
                                     double             durationSeconds,
                                     float              volume,
                                     bool               muted) {
            EditorAction action{EditorActionType::SetAudioClipProps};
            action.id               = track->id;
            action.secondaryId      = clip->id;
            action.path             = utf8ToPath(filePath);
            action.trimStartSeconds = state::editing::model::clampAudioSeconds(trimStartSeconds);
            action.durationSeconds  = state::editing::model::clampAudioSeconds(durationSeconds);
            action.volume           = state::editing::model::clampAudioVolume(volume);
            action.muted            = muted;
            ctx.submitAction(std::move(action));
        };

        float const scale      = metrics::scale();
        float const labelWidth = measureLabelColumn({
            "playback.refactorEditor.audio.filePath"_tr(),
            "playback.refactorEditor.audio.status"_tr(),
            "playback.refactorEditor.audio.startTick"_tr(),
            "playback.refactorEditor.audio.trimStart"_tr(),
            "playback.refactorEditor.audio.duration"_tr(),
        });

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f * scale, 4.0f * scale});
        if (ImGui::BeginTable(
                "##audio-clip-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.filePath"_tr().c_str());
            ImGui::TableNextColumn();
            {
                std::string const browseLabel = "playback.refactorEditor.audio.browse"_tr();
                float const       browseW =
                    ImGui::CalcTextSize(browseLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                float const gap    = ImGui::GetStyle().ItemSpacing.x;
                float const inputW = std::max(40.0f * scale, ImGui::GetContentRegionAvail().x - browseW - gap);
                ImGui::SetNextItemWidth(inputW);
                ImGui::InputText("##audio-clip-path", mAudioClipPathBuffer.data(), mAudioClipPathBuffer.size());
                mAudioClipPathEditing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::string const nextPath = mAudioClipPathBuffer.data();
                    if (!nextPath.empty() && nextPath != clip->filePath) {
                        submitProps(nextPath, clip->trimStartSeconds, clip->durationSeconds, clip->volume, clip->muted);
                    } else {
                        mAudioClipPathBuffer = {};
                        clip->filePath.copy(mAudioClipPathBuffer.data(), mAudioClipPathBuffer.size() - 1);
                    }
                }
                ImGui::SameLine(0.0f, gap);
                if (ImGui::Button((browseLabel + "##audio-clip-browse").c_str(), {browseW, 0.0f})) {
                    if (auto const picked = utils::pickAudioFile(utf8ToPath(clip->filePath))) {
                        std::string const pickedUtf8 = pathToUtf8(*picked);
                        mAudioClipPathBuffer         = {};
                        pickedUtf8.copy(mAudioClipPathBuffer.data(), mAudioClipPathBuffer.size() - 1);
                        mAudioClipPathEditing = false;
                        submitProps(
                            pickedUtf8,
                            clip->trimStartSeconds,
                            clip->durationSeconds,
                            clip->volume,
                            clip->muted
                        );
                    }
                }
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.status"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (clip->filePath.empty()) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.audio.statusFailed"_tr().c_str());
            } else {
                auto const asset = audio::external::getOrRequestAudioAsset(clip->filePath);
                if (!asset) {
                    ImGui::TextDisabled("%s", "playback.refactorEditor.audio.statusLoading"_tr().c_str());
                } else if (asset->state == audio::external::AudioLoadState::Pending) {
                    ImGui::TextDisabled("%s", "playback.refactorEditor.audio.statusLoading"_tr().c_str());
                } else if (asset->state == audio::external::AudioLoadState::Ready) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(
                        ImGui::ColorConvertU32ToFloat4(theme::kSuccess),
                        "%s",
                        "playback.refactorEditor.audio.statusReady"_tr(asset->durationSeconds).c_str()
                    );
                    ImGui::PopTextWrapPos();
                } else {
                    std::string const failedText =
                        asset->errorMessage.empty()
                            ? "playback.refactorEditor.audio.statusFailed"_tr()
                            : ("playback.refactorEditor.audio.statusFailed"_tr() + ": " + asset->errorMessage);
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kError), "%s", failedText.c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            int startTick = clip->startTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.startTick"_tr().c_str());
            ImGui::TableNextColumn();
            drawInputWithHint(formatTick(clip->startTick), [&] {
                ImGui::InputInt("##audio-clip-start-tick", &startTick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    startTick = project->totalTicks > 0 ? std::clamp(startTick, 0, project->totalTicks)
                                                        : std::max(0, startTick);
                    if (startTick != clip->startTick) {
                        EditorAction action{EditorActionType::MoveAudioClip};
                        action.id          = track->id;
                        action.secondaryId = clip->id;
                        action.tick        = startTick;
                        ctx.submitAction(std::move(action));
                    }
                }
            });

            double trimStart = clip->trimStartSeconds;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.trimStart"_tr().c_str());
            widgets::itemTooltip("playback.refactorEditor.audio.trimStartHint"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##audio-clip-trim-start", &trimStart, 0.0, 0.0, "%.2f");
            widgets::itemTooltip("playback.refactorEditor.audio.trimStartHint"_tr().c_str());
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(trimStart)) {
                double const clamped = state::editing::model::clampAudioSeconds(trimStart);
                if (std::abs(clamped - clip->trimStartSeconds) > 1e-9) {
                    submitProps(clip->filePath, clamped, clip->durationSeconds, clip->volume, clip->muted);
                }
            }

            double duration = clip->durationSeconds;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.audio.duration"_tr().c_str());
            widgets::itemTooltip("playback.refactorEditor.audio.durationHint"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##audio-clip-duration", &duration, 0.0, 0.0, "%.2f");
            widgets::itemTooltip("playback.refactorEditor.audio.durationHint"_tr().c_str());
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(duration)) {
                double const clamped = state::editing::model::clampAudioSeconds(duration);
                if (std::abs(clamped - clip->durationSeconds) > 1e-9) {
                    submitProps(clip->filePath, clip->trimStartSeconds, clamped, clip->volume, clip->muted);
                }
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        property::separator();
        drawAudioVolumeControl(
            "audio-clip",
            editable,
            clip->volume,
            clip->id,
            mAudioClipVolumeDragId,
            mAudioClipVolumeDragging,
            mAudioClipVolumeDragValue,
            [&](float nextVolume) {
                submitProps(clip->filePath, clip->trimStartSeconds, clip->durationSeconds, nextVolume, clip->muted);
            }
        );

        property::separator();
        bool muted = clip->muted;
        if (ImGui::Checkbox("playback.refactorEditor.audio.muted"_tr().c_str(), &muted)) {
            submitProps(clip->filePath, clip->trimStartSeconds, clip->durationSeconds, clip->volume, muted);
        }

        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.audio.deleteClip"_tr().c_str(),
                editable,
                property::ButtonVariant::Danger,
                ICON_TRASH
            )) {
            EditorAction action{EditorActionType::RemoveAudioClip};
            action.id          = track->id;
            action.secondaryId = clip->id;
            ctx.submitAction(std::move(action));
            ctx.selection.selectAudioTrack(track->id);
        }
        ImGui::EndDisabled();
        if (!editable) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_LOCK, "playback.refactorEditor.details.locked"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    } else {
        mAudioClipPathEditing    = false;
        mAudioClipVolumeDragging = false;
        mAudioClipVolumeDragId.clear();
    }
    return true;
}

void DetailsPanel::drawOverview(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    auto const submit  = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (property::beginSection("playback.refactorEditor.details.overview"_tr().c_str())) {
        auto const* activeScene  = state::editing::ClipOps::activeScene(*project);
        size_t      clipCount    = 0;
        size_t      enabledCount = 0;
        for (auto const& camera : project->cameras) {
            clipCount += camera.clips.size();
            for (auto const& clip : camera.clips) {
                if (clip.exportEnabled) ++enabledCount;
            }
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.selectionHint"_tr().c_str());
        ImGui::PopTextWrapPos();
        property::separator();
        if (activeScene) {
            property::formattedRow("playback.refactorEditor.details.sceneName"_tr(activeScene->name));
        }
        property::formattedRow("playback.refactorEditor.details.total"_tr(formatTick(project->totalTicks)));
        property::formattedRow("playback.refactorEditor.details.cameras"_tr(project->cameras.size()));
        property::formattedRow("playback.refactorEditor.details.clipsSummary"_tr(clipCount, enabledCount));
        property::separator();
        std::string const addCameraLabel = "playback.refactorEditor.details.addFreeCamera"_tr();
        std::string const addSceneLabel  = "playback.refactorEditor.details.addScene"_tr();
        int const         createChoice   = property::actionButtonPair(
            {addCameraLabel.c_str(), ICON_CAMERA, property::ButtonVariant::Primary, true},
            {addSceneLabel.c_str(), ICON_ADD, property::ButtonVariant::Secondary, true}
        );
        if (createChoice == 0) {
            submit({EditorActionType::AddFreeCamera});
        } else if (createChoice == 1) {
            submit({EditorActionType::AddScene});
        }
        bool const canReset =
            project->totalTicks > 0 && std::ranges::none_of(project->cameras, [](auto const& camera) {
                return camera.locked || std::ranges::any_of(camera.clips, &state::editing::model::Clip::locked);
            });
        if (property::actionButton(
                "playback.refactorEditor.details.resetClipsToFull"_tr().c_str(),
                canReset,
                property::ButtonVariant::Subtle,
                ICON_RESET
            )) {
            submit({EditorActionType::ResetClipsToFullRange});
        }
        property::endSection();
    }
}

void DetailsPanel::drawExportPage(PanelContext const& ctx) {
    auto const& state        = ctx.state;
    auto const& status       = state.exportStatus;
    bool const  exportActive = exporting::isExportActive(status.state);

    property::beginInspector(
        "playback.refactorEditor.inspector.export"_tr(),
        state.capabilities.ffmpegVideoExport ? "playback.refactorEditor.export.mp4"_tr()
                                             : "playback.refactorEditor.export.pngSequence"_tr()
    );

    if (property::beginSection("playback.refactorEditor.export.output"_tr().c_str())) {
        // Mirrors what the export dialog will start with: its frame rate, I/O range (-1 means unset), and compiled
        // duration.
        int const totalTicks = std::max(0, state.totalTicks);
        int const exportIn   = ctx.commands.exportStartTick();
        int const exportOut  = ctx.commands.exportEndTick();
        int const rangeStart = std::clamp(exportIn, 0, totalTicks);
        int const rangeEnd   = exportOut < 0 ? totalTicks : std::clamp(exportOut, 0, totalTicks);
        int const fps        = std::max(1, ctx.commands.exportFps());
        property::textRow(
            "playback.refactorEditor.export.frameRate"_tr().c_str(),
            (std::to_string(fps) + " FPS").c_str()
        );
        property::textRow(
            "playback.refactorEditor.export.timeline"_tr().c_str(),
            (formatTick(rangeStart) + " - " + formatTick(std::max(rangeStart, rangeEnd))).c_str()
        );

        exporting::ExportSettings previewSettings;
        previewSettings.outputDirectory = ".";
        previewSettings.outputName      = "preview";
        bool const hasCustomRange       = exportIn >= 0 && exportOut > exportIn && exportOut <= totalTicks;
        previewSettings.startTick       = hasCustomRange ? exportIn : 0;
        previewSettings.endTick         = hasCustomRange ? exportOut : totalTicks;
        previewSettings.frameRate       = {fps, 1};
        previewSettings.resolutionX     = 1920;
        previewSettings.resolutionY     = 1080;
        previewSettings.ssaa            = 1;
        bool const hasSceneRange = exportIn >= 0 && exportOut > exportIn && (exportIn > 0 || exportOut < totalTicks);
        previewSettings.rangeMode =
            hasSceneRange ? exporting::ExportRangeMode::SequenceInOut : exporting::ExportRangeMode::EnabledClips;
        std::string durationText = "-";
        if (auto const compiled = exporting::ExportPlanCompiler::compile(previewSettings, *state.project)) {
            uint64_t const frameCount      = compiled.plan->frameCount;
            double const   durationSeconds = static_cast<double>(frameCount) / static_cast<double>(fps);
            durationText = "playback.refactorEditor.export.durationValue"_tr(durationSeconds, frameCount);
        }
        property::textRow("playback.refactorEditor.details.outputDuration"_tr().c_str(), durationText.c_str());
        if (!state.capabilities.ffmpegVideoExport) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s  %s", ICON_INFO, "playback.refactorEditor.export.ffmpegUnavailable"_tr().c_str());
            ImGui::PopTextWrapPos();
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.inspector.openExportDialog"_tr().c_str(),
                state.capabilities.videoExport && !exportActive && state.totalTicks > 0,
                property::ButtonVariant::Primary,
                ICON_EXPORT
            )) {
            ctx.commands.openExportDialog();
        }
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.exportStatus"_tr().c_str())) {
        if (status.state == exporting::ExportState::Idle) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.export.state.idle"_tr().c_str());
        } else {
            if (status.totalFrames > 0) {
                auto const completed =
                    std::min(status.totalFrames, std::max(status.submittedFrames, status.writtenFrames));
                float const fraction = static_cast<float>(completed) / static_cast<float>(status.totalFrames);
                ImGui::ProgressBar(fraction, {-1.0f, ImGui::GetFontSize() + 6.0f * metrics::scale()});
            }
            if (!status.message.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(status.message.c_str());
                ImGui::PopTextWrapPos();
            }
            if (property::actionButton(
                    "playback.refactorEditor.export.cancel"_tr().c_str(),
                    exportActive,
                    property::ButtonVariant::Danger,
                    ICON_CLOSE
                )) {
                ctx.submitAction({EditorActionType::CancelExport});
            }
            if (status.state == exporting::ExportState::Completed && !status.outputPath.empty()
                && property::actionButton(
                    "playback.refactorEditor.export.showInFolder"_tr().c_str(),
                    true,
                    property::ButtonVariant::Secondary,
                    ICON_FOLDER_OPEN
                )) {
                playback::utils::revealInExplorer(status.outputPath);
            }
        }
        property::endSection();
    }
}

void DetailsPanel::drawPreviewPage(PanelContext const& ctx) {
    property::beginInspector(
        "playback.refactorEditor.inspector.preview"_tr(),
        "playback.refactorEditor.inspector.previewSubtitle"_tr()
    );
    auto const hint = [](std::string const& text) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    };

    // An export renders what these say, so they stay put while one is running.
    ImGui::BeginDisabled(exporting::isExportActive(ctx.state.exportStatus.state));

    if (property::beginSection("playback.refactorEditor.inspector.hud"_tr().c_str())) {
        struct Toggle {
            visuals::HudElement element;
            std::string         label;
        };
        std::array const toggles{
            Toggle{visuals::HudElement::Chat,       "playback.refactorEditor.inspector.hudChat"_tr()      },
            Toggle{visuals::HudElement::BossBar,    "playback.refactorEditor.inspector.hudBossBar"_tr()   },
            Toggle{visuals::HudElement::Title,      "playback.refactorEditor.inspector.hudTitle"_tr()     },
            Toggle{visuals::HudElement::Scoreboard, "playback.refactorEditor.inspector.hudScoreboard"_tr()},
            Toggle{visuals::HudElement::ActionBar,  "playback.refactorEditor.inspector.hudActionBar"_tr() },
            Toggle{visuals::HudElement::Hotbar,     "playback.refactorEditor.inspector.hudHotbar"_tr()    },
        };
        float const scale = metrics::scale();
        int const   cols  = ImGui::GetContentRegionAvail().x >= 180.0f * scale ? 2 : 1;
        if (cols == 2) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f * scale, 2.0f * scale});
            if (ImGui::BeginTable("##hud-grid", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
                for (auto const& toggle : toggles) {
                    ImGui::TableNextColumn();
                    bool shown = ctx.commands.isHudElementShown(toggle.element);
                    ImGui::PushID(static_cast<int>(toggle.element));
                    if (ImGui::Checkbox((toggle.label + "##set-hud").c_str(), &shown))
                        ctx.commands.setHudElementShown(toggle.element, shown);
                    ImGui::PopID();
                    if (toggle.element == visuals::HudElement::Hotbar)
                        widgets::itemTooltip("playback.refactorEditor.inspector.hudHotbarHint"_tr().c_str());
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        } else {
            for (auto const& toggle : toggles) {
                bool shown = ctx.commands.isHudElementShown(toggle.element);
                ImGui::PushID(static_cast<int>(toggle.element));
                if (ImGui::Checkbox((toggle.label + "##set-hud").c_str(), &shown))
                    ctx.commands.setHudElementShown(toggle.element, shown);
                ImGui::PopID();
                if (toggle.element == visuals::HudElement::Hotbar)
                    widgets::itemTooltip("playback.refactorEditor.inspector.hudHotbarHint"_tr().c_str());
            }
        }
        hint("playback.refactorEditor.inspector.hudHint"_tr());
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.previewAudio"_tr().c_str())) {
        bool muted = ctx.commands.isPreviewAudioMuted();
        if (ImGui::Checkbox(("playback.refactorEditor.inspector.previewMute"_tr() + "##set-mute").c_str(), &muted))
            ctx.commands.setPreviewAudioMuted(muted);

        ImGui::BeginDisabled(muted);
        ImGui::TextUnformatted("playback.refactorEditor.inspector.previewVolume"_tr().c_str());
        float percent = ctx.commands.previewAudioVolume() * 100.0f;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##set-preview-volume", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            ctx.commands.setPreviewAudioVolume(percent / 100.0f, false);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.commands.setPreviewAudioVolume(percent / 100.0f, true);
        ImGui::EndDisabled();

        bool silent = ctx.commands.isPreviewSilentWhilePaused();
        if (ImGui::Checkbox(
                ("playback.refactorEditor.inspector.previewSilentPaused"_tr() + "##set-silent-paused").c_str(),
                &silent
            ))
            ctx.commands.setPreviewSilentWhilePaused(silent);
        widgets::itemTooltip("playback.refactorEditor.inspector.previewSilentPausedHint"_tr().c_str());
        hint("playback.refactorEditor.inspector.previewAudioHint"_tr());
        property::endSection();
    }

    ImGui::EndDisabled();
}

void DetailsPanel::drawSettingsPage(PanelContext const& ctx) {
    property::beginInspector(
        "playback.refactorEditor.inspector.settings"_tr(),
        "playback.refactorEditor.inspector.settingsSubtitle"_tr()
    );

    if (property::beginSection("playback.refactorEditor.inspector.uiScale"_tr().c_str())) {
        struct Tier {
            UiScaleTier tier;
            std::string label;
        };
        float const      scale = metrics::scale();
        UiScaleTier const current = ctx.commands.uiScaleTier();
        if (property::choiceChip(
                ("playback.refactorEditor.inspector.uiScaleAuto"_tr() + "##ui-scale-auto").c_str(),
                current == UiScaleTier::Auto
            )) {
            ctx.commands.setUiScaleTier(UiScaleTier::Auto);
        }
        std::array const fixedTiers{
            Tier{UiScaleTier::Small,  "playback.refactorEditor.inspector.uiScaleSmall"_tr() },
            Tier{UiScaleTier::Medium, "playback.refactorEditor.inspector.uiScaleMedium"_tr()},
            Tier{UiScaleTier::Large,  "playback.refactorEditor.inspector.uiScaleLarge"_tr() },
            Tier{UiScaleTier::Huge,   "playback.refactorEditor.inspector.uiScaleHuge"_tr()  },
        };
        float const gap    = 4.0f * scale;
        float const availW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        float const halfW  = (availW - gap) * 0.5f;
        bool        fits2  = true;
        for (auto const& entry : fixedTiers) {
            if (ImGui::CalcTextSize(entry.label.c_str()).x + 14.0f * scale > halfW) {
                fits2 = false;
                break;
            }
        }
        for (size_t i = 0; i < fixedTiers.size(); ++i) {
            auto const& entry = fixedTiers[i];
            bool const  right = fits2 && (i % 2 == 1);
            if (right) ImGui::SameLine(0.0f, gap);
            float const w = !fits2 ? -1.0f : (right ? -1.0f : std::floor(halfW));
            ImGui::PushID(static_cast<int>(entry.tier));
            if (property::choiceChip(entry.label.c_str(), current == entry.tier, w)) {
                ctx.commands.setUiScaleTier(entry.tier);
            }
            ImGui::PopID();
        }
        if (current == UiScaleTier::Auto) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(
                "%s",
                "playback.refactorEditor.inspector.uiScaleAutoHint"_tr(
                    static_cast<int>(calculateReplayUIScale(current, ImGui::GetIO().DisplaySize.y) * 100.0f)
                )
                    .c_str()
            );
            ImGui::PopTextWrapPos();
        }
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.viewSettings"_tr().c_str())) {
        bool infoOverlay = ctx.commands.isInfoOverlayVisible();
        if (ImGui::Checkbox(("playback.refactorEditor.viewport.infoOverlay"_tr() + "##set-info").c_str(), &infoOverlay))
            ctx.commands.setInfoOverlayVisible(infoOverlay);
        bool autoPreview = ctx.commands.isAutoPreviewEnabled();
        if (ImGui::Checkbox(("playback.refactorEditor.viewport.autoPreview"_tr() + "##set-auto").c_str(), &autoPreview))
            ctx.commands.setAutoPreviewEnabled(autoPreview);
        bool cameraPath = ctx.commands.isCameraPathVisible();
        if (ImGui::Checkbox(("playback.refactorEditor.menu.cameraPath"_tr() + "##set-path").c_str(), &cameraPath))
            ctx.commands.toggleCameraPath();
        bool snap = ctx.commands.isSnapEnabled();
        if (ImGui::Checkbox(("playback.refactorEditor.timeline.snap"_tr() + "##set-snap").c_str(), &snap))
            ctx.commands.setSnapEnabled(snap);
        property::endSection();
    }

    drawOptiPistonSection();

    if (property::beginSection("playback.refactorEditor.viewport.aspectTooltip"_tr().c_str())) {
        float const scale   = metrics::scale();
        float const current = ctx.commands.videoAspectRatio();
        float const gap     = 4.0f * scale;
        float const availW  = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        int const   cols    = availW >= 200.0f * scale ? 3 : 2;
        float const cellW   = std::max(1.0f, (availW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
        int         index   = 0;
        for (auto const& preset : widgets::kAspectPresets) {
            int const col = index % cols;
            if (col > 0) ImGui::SameLine(0.0f, gap);
            float const w = (col == cols - 1) ? -1.0f : std::floor(cellW);
            ImGui::PushID(index);
            if (property::choiceChip(preset.label, std::abs(current - preset.ratio) < 0.01f, w)) {
                ctx.commands.setVideoAspectRatio(preset.ratio);
            }
            ImGui::PopID();
            ++index;
        }
        property::endSection();
    }
}

void DetailsPanel::drawOptiPistonSection() {
    // Absent OptiPiston means no section at all, not a disabled one.
    auto const piston = integration::optiPistonSettings();
    if (!piston) {
        mPistonDurationDraft.reset();
        return;
    }
    if (!property::beginSection("playback.refactorEditor.inspector.optiPiston"_tr().c_str())) return;

    bool enabled = piston->enabled;
    if (ImGui::Checkbox(("playback.refactorEditor.inspector.pistonEnabled"_tr() + "##set-piston").c_str(), &enabled))
        integration::setOptiPistonEnabled(enabled);

    ImGui::BeginDisabled(!piston->enabled);
    ImGui::TextUnformatted("playback.refactorEditor.inspector.pistonDuration"_tr().c_str());
    // Held locally while dragging so the config is written once, on release.
    float duration = mPistonDurationDraft.value_or(piston->durationTicks);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(
        "##set-piston-duration",
        &duration,
        piston->minDurationTicks,
        piston->maxDurationTicks,
        "%.2f gt",
        ImGuiSliderFlags_AlwaysClamp
    );
    if (ImGui::IsItemActive()) mPistonDurationDraft = duration;
    if (ImGui::IsItemDeactivatedAfterEdit()) integration::setOptiPistonDuration(duration);
    if (!ImGui::IsItemActive()) mPistonDurationDraft.reset();
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.optiPistonHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}

} // namespace playback::editor::ui

#include "DetailsPanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/integration/OptiPistonBridge.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/utils/ShellUtils.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

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

float keyframeLabelWidth() {
    float width = 72.0f;
    for (auto const& label : {
             "playback.refactorEditor.details.tick"_tr(),
             "playback.refactorEditor.details.lensMode"_tr(),
             "playback.refactorEditor.details.position"_tr(),
             "playback.refactorEditor.details.rotation"_tr(),
             "playback.refactorEditor.details.fov"_tr(),
             "playback.refactorEditor.details.interpolation"_tr(),
         }) {
        width = std::max(width, ImGui::CalcTextSize(label.c_str()).x + 8.0f);
    }
    return width;
}

bool vectorInput(
    char const* id,
    float (&values)[3],
    std::array<char const*, 3> const& labels,
    std::array<ImVec4, 3> const*      colors,
    char const*                       format
) {
    bool edited = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {3.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.0f, 3.0f});
    if (ImGui::BeginTable("##axes", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            if (colors) ImGui::TextColored((*colors)[axis], "%s", labels[axis]);
            else ImGui::TextDisabled("%s", labels[axis]);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(axis);
            ImGui::InputFloat("##value", &values[axis], 0.0f, 0.0f, format);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return edited;
}

bool vectorInput2(char const* id, float (&values)[2], std::array<char const*, 2> const& labels, char const* format) {
    bool edited = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {3.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.0f, 3.0f});
    if (ImGui::BeginTable("##axes2", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        for (int axis = 0; axis < 2; ++axis) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", labels[axis]);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(axis);
            ImGui::InputFloat("##value", &values[axis], 0.0f, 0.0f, format);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return edited;
}

[[nodiscard]] float speedToSliderT(float speed) {
    float const safe = std::max(speed, 0.01f);
    if (safe <= 1.0f) return 0.5f * (1.0f + std::clamp(std::log2(safe) / 2.0f, -1.0f, 0.0f));
    return 0.5f + 0.5f * std::clamp(std::log2(safe) / 3.0f, 0.0f, 1.0f);
}

[[nodiscard]] float sliderTToSpeed(float t) {
    float const clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped <= 0.5f) return std::exp2(4.0f * clamped - 2.0f);
    return std::exp2(6.0f * clamped - 3.0f);
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

} // namespace

void DetailsPanel::draw(PanelContext const& ctx) {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImVec2 const avail  = ImGui::GetContentRegionAvail();
    float const  railW  = metrics::rail();
    drawRail(railW, avail.y);

    ImGui::SetCursorScreenPos({origin.x + railW, origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::kPanelPadding, theme::kPanelPadding});
    ImGui::BeginChild("##InspectorBody", {std::max(1.0f, avail.x - railW), avail.y}, false);
    if (!ctx.state.project) {
        widgets::noActiveProjectPlaceholder();
    } else {
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
    ImGui::PopStyleVar();
}

void DetailsPanel::drawRail(float width, float height) {
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    auto*        drawList = ImGui::GetWindowDrawList();
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
    float       y      = origin.y + 4.0f;
    for (auto const& entry : entries) {
        bool const active = mPage == entry.page;
        ImGui::SetCursorScreenPos({origin.x + (width - button) * 0.5f, y});
        ImGui::PushID(static_cast<int>(entry.page));
        if (widgets::iconToggle("rail", entry.icon, entry.tooltip.c_str(), active, entry.enabled)) mPage = entry.page;
        ImGui::PopID();
        if (active) {
            drawList->AddRectFilled({origin.x, y + 3.0f}, {origin.x + 2.0f, y + button - 3.0f}, theme::kRailActive);
        }
        y += button + 2.0f;
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
    }
    property::beginInspector("playback.refactorEditor.inspector.selection"_tr(), subject);

    if (drawClip(ctx)) return;
    if (drawCamera(ctx)) return;
    if (drawKeyframes(ctx)) return;
    if (drawKeyframe(ctx)) return;
    if (drawMarker(ctx)) return;
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
        bool const editable = !camera->locked && !clip->locked;
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

        int const   sourceTicks       = std::max(0, clip->outTick - clip->inTick);
        float const sourceSec         = static_cast<float>(sourceTicks) / static_cast<float>(widgets::kTicksPerSecond);
        float       outputDurationSec = sourceSec / std::max(state::editing::model::MinClipSpeed, clip->speed);
        auto const  submitSpeed       = [&](float nextSpeed) {
            float const clamped =
                std::clamp(nextSpeed, state::editing::model::MinClipSpeed, state::editing::model::MaxClipSpeed);
            if (std::abs(clamped - clip->speed) > 1e-4f) {
                EditorAction action{EditorActionType::SetClipSpeed};
                action.id    = clip->id;
                action.speed = clamped;
                ctx.submitAction(std::move(action));
            }
        };

        ImGui::BeginDisabled(!editable);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
        if (ImGui::BeginTable(
                "##clip-properties",
                2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
            )) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            int inTick = clip->inTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.start"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
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
            ImGui::TextDisabled("%s", formatTick(clip->inTick).c_str());

            int outTick = clip->outTick;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.end"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
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
            ImGui::TextDisabled("%s", formatTick(clip->outTick).c_str());

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.duration"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(formatTick(sourceTicks).c_str());

            float speed = clip->speed;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.speed"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat("##clip-speed", &speed, 0.0f, 0.0f, "%.2fx");
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(speed)) {
                submitSpeed(speed);
            }
            ImGui::TextDisabled("%.0f%%", static_cast<double>(clip->speed * 100.0f));

            float editedDurationSec = outputDurationSec;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.outputDuration"_tr().c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat("##clip-out-duration", &editedDurationSec, 0.0f, 0.0f, "%.2f s");
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(editedDurationSec) && editedDurationSec > 0.001f
                && sourceSec > 0.0f) {
                submitSpeed(sourceSec / editedDurationSec);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // Piecewise log2 speed slider centered at 1.0x.
        {
            float const  availW     = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            float const  sliderH    = std::round(18.0f * scale);
            float const  thumbR     = 5.5f * scale;
            float const  railH      = 4.0f * scale;
            float const  alphaScale = editable ? 1.0f : ImGui::GetStyle().Alpha;
            ImVec2 const min        = ImGui::GetCursorScreenPos();
            ImVec2 const max        = {min.x + availW, min.y + sliderH};

            ImGui::InvisibleButton("##clip-speed-slider", {availW, sliderH});
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
                    submitSpeed(1.0f);
                } else if (active && !ignoreDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float const rawT     = std::clamp((ImGui::GetMousePos().x - trackMinX) / trackW, 0.0f, 1.0f);
                    float       newSpeed = sliderTToSpeed(rawT);
                    if (ImGui::GetIO().KeyShift) {
                        constexpr float kSnapPresets[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
                        float           bestDist       = std::abs(rawT - speedToSliderT(kSnapPresets[0]));
                        float           bestPreset     = kSnapPresets[0];
                        for (int i = 1; i < IM_ARRAYSIZE(kSnapPresets); ++i) {
                            float const dist = std::abs(rawT - speedToSliderT(kSnapPresets[i]));
                            if (dist < bestDist) {
                                bestDist   = dist;
                                bestPreset = kSnapPresets[i];
                            }
                        }
                        newSpeed = bestPreset;
                    }
                    submitSpeed(newSpeed);
                }
            }

            float const t          = speedToSliderT(clip->speed);
            float const thumbX     = trackMinX + t * trackW;
            float const railTop    = centerY - railH * 0.5f;
            float const railBottom = centerY + railH * 0.5f;
            float const railRound  = railH * 0.5f;
            auto* const dl         = ImGui::GetWindowDrawList();

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

            ImU32 const accent = speedAccentColor(clip->speed);
            if (clip->speed > 1.001f || clip->speed < 0.999f) {
                float const fillLeft  = std::min(centerX, thumbX);
                float const fillRight = std::max(centerX, thumbX);
                if (fillRight - fillLeft > 0.5f) {
                    dl->AddRectFilled(
                        {fillLeft, railTop},
                        {fillRight, railBottom},
                        scaleAlpha(accent, alphaScale),
                        railRound
                    );
                }
            }

            ImU32 const thumbCol =
                (editable && (hovered || active)) ? brightenColor(accent, active ? 0.30f : 0.18f) : accent;
            dl->AddCircleFilled({thumbX, centerY}, thumbR, scaleAlpha(thumbCol, alphaScale));
            dl->AddCircle({thumbX, centerY}, thumbR, scaleAlpha(theme::kKeyframeOutline, alphaScale), 0, 1.0f * scale);

            char tip[64]{};
            std::snprintf(
                tip,
                sizeof(tip),
                "%.2fx (%.0f%%) | %.2f s",
                static_cast<double>(clip->speed),
                static_cast<double>(clip->speed * 100.0f),
                static_cast<double>(outputDurationSec)
            );
            widgets::itemTooltip(tip);
        }

        // Equal-width speed preset chips.
        {
            constexpr float kSpeedPresets[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
            constexpr int   kPresetCount    = IM_ARRAYSIZE(kSpeedPresets);
            float const     spacing         = 3.0f * scale;
            float const     availW          = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            float const     chipW           = std::max(
                1.0f,
                (availW - spacing * static_cast<float>(kPresetCount - 1)) / static_cast<float>(kPresetCount)
            );
            float const chipH = std::round(ImGui::GetFontSize() + 6.0f * scale);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing, spacing});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.0f * scale, 0.0f});
            for (int i = 0; i < kPresetCount; ++i) {
                if (i > 0) ImGui::SameLine();
                float const preset       = kSpeedPresets[i];
                bool const  active       = std::abs(clip->speed - preset) < 0.001f;
                ImU32 const presetAccent = speedAccentColor(preset);
                char        label[16]{};
                std::snprintf(label, sizeof(label), "%.2gx", static_cast<double>(preset));

                float const w =
                    (i == kPresetCount - 1) ? std::max(1.0f, ImGui::GetContentRegionAvail().x) : std::floor(chipW);
                if (active) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(presetAccent, 0x44));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(presetAccent, 0x5c));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(presetAccent, 0x70));
                    ImGui::PushStyleColor(ImGuiCol_Border, presetAccent);
                    ImGui::PushStyleColor(ImGuiCol_Text, presetAccent);
                } else {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, theme::kFineDividerWidth);
                    ImGui::PushStyleColor(ImGuiCol_Button, theme::kButton);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
                    ImGui::PushStyleColor(ImGuiCol_Border, theme::kFineDivider);
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
                }
                if (ImGui::Button(label, {w, chipH})) {
                    submitSpeed(preset);
                }
                ImGui::PopStyleColor(5);
                ImGui::PopStyleVar();
            }
            ImGui::PopStyleVar(3);
        }

        bool muted = clip->muted;
        if (ImGui::Checkbox("playback.refactorEditor.details.muted"_tr().c_str(), &muted)) {
            EditorAction action{EditorActionType::SetClipMuted};
            action.id    = clip->id;
            action.value = muted;
            ctx.submitAction(std::move(action));
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(camera->locked);
        bool exportEnabled = clip->exportEnabled;
        if (ImGui::Checkbox("playback.refactorEditor.details.exportEnabled"_tr().c_str(), &exportEnabled)) {
            EditorAction action{EditorActionType::SetClipExportEnabled};
            action.id    = clip->id;
            action.value = exportEnabled;
            ctx.submitAction(std::move(action));
        }

        bool locked = clip->locked;
        if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
            EditorAction action{EditorActionType::SetClipLocked};
            action.id    = clip->id;
            action.value = locked;
            ctx.submitAction(std::move(action));
        }
        ImGui::EndDisabled();

        property::separator();
        int const  playhead = ctx.state.currentTick;
        bool const canSplit = editable && clip->inTick < playhead && playhead < clip->outTick;
        if (property::actionButton("playback.refactorEditor.details.splitClipAtPlayhead"_tr().c_str(), canSplit)) {
            EditorAction action{EditorActionType::SplitClip};
            action.id   = clip->id;
            action.tick = playhead;
            ctx.submitAction(std::move(action));
        }
        bool const canTrimIn = editable && minIn <= playhead && playhead < clip->outTick && playhead != clip->inTick;
        if (property::actionButton("playback.refactorEditor.details.trimClipInToPlayhead"_tr().c_str(), canTrimIn)) {
            EditorAction action{EditorActionType::MoveAndTrimClip};
            action.id            = clip->id;
            action.tick          = playhead;
            action.secondaryTick = clip->outTick;
            ctx.submitAction(std::move(action));
        }
        bool const canTrimOut = editable && clip->inTick < playhead && playhead <= maxOut && playhead != clip->outTick;
        if (property::actionButton("playback.refactorEditor.details.trimClipOutToPlayhead"_tr().c_str(), canTrimOut)) {
            EditorAction action{EditorActionType::MoveAndTrimClip};
            action.id            = clip->id;
            action.tick          = clip->inTick;
            action.secondaryTick = playhead;
            ctx.submitAction(std::move(action));
        }
        bool const canExport = ctx.state.capabilities.videoExport
                            && !exporting::isExportActive(ctx.state.exportStatus.state) && project->totalTicks > 0;
        if (property::actionButton("playback.refactorEditor.details.exportClip"_tr().c_str(), canExport)) {
            ctx.commands.openExportSingleClip(clip->id);
        }
        if (property::actionButton("playback.refactorEditor.details.deleteClip"_tr().c_str(), editable)) {
            EditorAction action{EditorActionType::DeleteClip};
            action.id = clip->id;
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        if (camera->locked) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
        } else if (clip->locked) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.clipLocked"_tr().c_str());
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
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.clipsSummary"_tr(camera->clips.size(), enabledClips).c_str()
            );
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
                auto            shake      = *camera->shake;
                bool            edited     = false;
                constexpr float labelWidth = 72.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
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
            if (property::actionButton("playback.refactorEditor.details.addKeyframeAtPlayhead"_tr().c_str())) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = ctx.state.currentTick;
                submit(std::move(action));
            }
            bool const canResetClips = !camera->locked && project->totalTicks > 0
                                    && std::ranges::none_of(camera->clips, &state::editing::model::Clip::locked);
            if (property::actionButton(
                    "playback.refactorEditor.details.resetTrackClipsToFull"_tr().c_str(),
                    canResetClips
                )) {
                EditorAction action{EditorActionType::ResetClipsToFullRange};
                action.id = camera->id;
                submit(std::move(action));
            }
            property::separator();
            if (property::actionButton("playback.refactorEditor.details.deleteCamera"_tr().c_str())) {
                EditorAction action{EditorActionType::DeleteCamera};
                action.id = camera->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
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
    bool                                            mixedMode       = false;
    bool                                            anyInterpolated = false;
    bool                                            anyPerson       = false;
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
        if (key->second.mode == state::editing::model::CameraKeyframeMode::Interpolated) {
            anyInterpolated = true;
        } else {
            anyPerson = true;
            if (!sharedSubject) sharedSubject = key->second.subject;
            else if (*sharedSubject != key->second.subject) mixedSubject = true;
        }
        int const type = static_cast<int>(key->second.interpolationType);
        if (!shared) shared = type;
        else if (*shared != type) mixed = true;
        if (!sharedIgnore) sharedIgnore = key->second.ignoreClipSpeed;
        else if (*sharedIgnore != key->second.ignoreClipSpeed) mixedIgnore = true;
    }

    if (property::beginSection("playback.refactorEditor.details.multiKeyframe"_tr().c_str())) {
        property::textRow(
            "playback.refactorEditor.details.keyframeCount"_tr().c_str(),
            std::to_string(selection.keyframeCount()).c_str()
        );
        property::textRow("playback.refactorEditor.details.trackCount"_tr().c_str(), std::to_string(tracks).c_str());
        property::separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.lensMode"_tr().c_str());
        ImGui::SameLine();
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
        if (anyInterpolated) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
            ImGui::SameLine();
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
        property::separator();
        {
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
                "playback.refactorEditor.details.deleteKeyframes"_tr(selection.keyframeCount()).c_str()
            )) {
            EditorAction action{EditorActionType::DeleteCameraKeyframes};
            action.keyframeRefs = std::move(refs);
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.multiKeyframeHint"_tr().c_str());
        property::endSection();
    }
    return true;
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
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.camera"_tr().c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(camera->name.c_str());
            property::separator();
            ImGui::BeginDisabled(camera->locked);
            bool const person =
                key->second.mode != state::editing::model::CameraKeyframeMode::Interpolated;
            int const       originalTick = key->first;
            int             tick         = originalTick;
            float const     labelWidth   = keyframeLabelWidth();
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
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

                // Person keyframes take their view from the replay player; interpolated keyframes use explicit pose.
                if (!person) {
                    float position[3] = {key->second.position.x, key->second.position.y, key->second.position.z};
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.position"_tr().c_str());
                    ImGui::TableNextColumn();
                    static constexpr std::array positionLabels{"X", "Y", "Z"};
                    static std::array const     positionColors{
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisX),
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisY),
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisZ),
                    };
                    if (vectorInput("position", position, positionLabels, &positionColors, "%.3f")
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
                    auto const       yawLabel   = "playback.refactorEditor.details.yaw"_tr();
                    auto const       pitchLabel = "playback.refactorEditor.details.pitch"_tr();
                    auto const       rollLabel  = "playback.refactorEditor.details.roll"_tr();
                    std::array const rotationLabels{yawLabel.c_str(), pitchLabel.c_str(), rollLabel.c_str()};
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

                    int interpolation = static_cast<int>(key->second.interpolationType);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (widgets::beginCombo(
                            "##interpolation",
                            interpolationName(key->second.interpolationType).c_str()
                        )) {
                        for (int index = 0; index < 8; ++index) {
                            auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                            if (ImGui::Selectable(interpolationName(value).c_str(), index == interpolation)) {
                                EditorAction action{EditorActionType::SetKeyframeInterpolation};
                                action.id   = camera->id;
                                action.tick = originalTick;
                                action.kind = index;
                                submit(std::move(action));
                            }
                        }
                        widgets::endCombo();
                    }

                    if (key->second.interpolationType == state::editing::model::CameraInterpolationType::CubicBezier) {
                        if (mBezierDragHandle != 0
                            && (mBezierDragCameraId != camera->id || mBezierDragTick != originalTick)) {
                            mBezierDragHandle = 0;
                        }
                        auto ctrl1 = mBezierDragHandle != 0 ? mBezierDraftCtrl1 : key->second.bezierCtrl1;
                        auto ctrl2 = mBezierDragHandle != 0 ? mBezierDraftCtrl2 : key->second.bezierCtrl2;
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

                            ImGui::InvisibleButton("##bezier-canvas", {availW, boxH});
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
                            if (hovered && !camera->locked) {
                                if (d1 <= hitR2 && (d1 <= d2 || d2 > hitR2)) hoveredH = 1;
                                else if (d2 <= hitR2) hoveredH = 2;
                            }
                            if (hoveredH != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                mBezierDragCameraId = camera->id;
                                mBezierDragTick     = originalTick;
                                mBezierDragHandle   = hoveredH;
                                mBezierDraftCtrl1   = ctrl1;
                                mBezierDraftCtrl2   = ctrl2;
                            }
                            if (mBezierDragHandle != 0) {
                                if (!camera->locked && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                    if (mBezierDragHandle == 1) mBezierDraftCtrl1 = fromScreen(mouse);
                                    else mBezierDraftCtrl2 = fromScreen(mouse);
                                    ctrl1 = mBezierDraftCtrl1;
                                    ctrl2 = mBezierDraftCtrl2;
                                    p1    = toScreen(ctrl1);
                                    p2    = toScreen(ctrl2);
                                } else {
                                    EditorAction action{EditorActionType::SetKeyframeBezierControls};
                                    action.id          = camera->id;
                                    action.tick        = originalTick;
                                    action.bezierCtrl1 = mBezierDraftCtrl1;
                                    action.bezierCtrl2 = mBezierDraftCtrl2;
                                    mBezierDragHandle  = 0;
                                    submit(std::move(action));
                                }
                            }
                            if (hoveredH != 0 || mBezierDragHandle != 0) {
                                ImGui::SetMouseCursor(
                                    mBezierDragHandle != 0 ? theme::kCursorGrabbing : theme::kCursorGrab
                                );
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
                            ImU32 const c1Color =
                                (mBezierDragHandle == 1 || hoveredH == 1) ? theme::kSelected : theme::kKeyframe;
                            ImU32 const c2Color =
                                (mBezierDragHandle == 2 || hoveredH == 2) ? theme::kSelected : theme::kKeyframe;
                            dl->AddCircleFilled(p1, handleR, c1Color);
                            dl->AddCircle(p1, handleR, theme::kKeyframeOutline, 0, 1.2f * uiScale);
                            dl->AddCircleFilled(p2, handleR, c2Color);
                            dl->AddCircle(p2, handleR, theme::kKeyframeOutline, 0, 1.2f * uiScale);
                        }

                        static constexpr std::array xyLabels{"X", "Y"};
                        float                       c1[2] = {ctrl1.x, ctrl1.y};
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.bezierControl1"_tr().c_str());
                        ImGui::TableNextColumn();
                        if (vectorInput2("bezier-c1", c1, xyLabels, "%.2f") && std::isfinite(c1[0])
                            && std::isfinite(c1[1])) {
                            EditorAction action{EditorActionType::SetKeyframeBezierControls};
                            action.id          = camera->id;
                            action.tick        = originalTick;
                            action.bezierCtrl1 = {std::clamp(c1[0], 0.0f, 1.0f), std::clamp(c1[1], 0.0f, 1.0f)};
                            action.bezierCtrl2 = ctrl2;
                            submit(std::move(action));
                        }

                        float c2[2] = {ctrl2.x, ctrl2.y};
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", "playback.refactorEditor.details.bezierControl2"_tr().c_str());
                        ImGui::TableNextColumn();
                        if (vectorInput2("bezier-c2", c2, xyLabels, "%.2f") && std::isfinite(c2[0])
                            && std::isfinite(c2[1])) {
                            EditorAction action{EditorActionType::SetKeyframeBezierControls};
                            action.id          = camera->id;
                            action.tick        = originalTick;
                            action.bezierCtrl1 = ctrl1;
                            action.bezierCtrl2 = {std::clamp(c2[0], 0.0f, 1.0f), std::clamp(c2[1], 0.0f, 1.0f)};
                            submit(std::move(action));
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            if (person) {
                // The subject holds from this key until the next key on the same track (or end of replay).
                property::separator();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.personApplyHint"_tr().c_str());
                ImGui::PopTextWrapPos();
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
            }
            property::separator();
            {
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
            if (property::actionButton("playback.refactorEditor.details.deleteKeyframe"_tr().c_str())) {
                EditorAction action{EditorActionType::DeleteCameraKeyframe};
                action.id   = camera->id;
                action.tick = originalTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
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
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
            if (ImGui::BeginTable(
                    "##marker-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 72.0f);
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
            if (property::actionButton("playback.refactorEditor.timeline.seekToMarker"_tr().c_str())) {
                ctx.commands.seekTo(marker->tick);
            }
            if (property::actionButton("playback.refactorEditor.details.deleteMarker"_tr().c_str())) {
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
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.selectionHint"_tr().c_str());
        ImGui::Spacing();
        if (activeScene) {
            ImGui::TextUnformatted("playback.refactorEditor.details.sceneName"_tr(activeScene->name).c_str());
        }
        ImGui::TextUnformatted("playback.refactorEditor.details.total"_tr(formatTick(project->totalTicks)).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.cameras"_tr(project->cameras.size()).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.clipsSummary"_tr(clipCount, enabledCount).c_str());
        ImGui::Spacing();
        if (property::actionButton("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            submit({EditorActionType::AddFreeCamera});
        }
        if (property::actionButton("playback.refactorEditor.details.addScene"_tr().c_str())) {
            submit({EditorActionType::AddScene});
        }
        bool const canReset =
            project->totalTicks > 0 && std::ranges::none_of(project->cameras, [](auto const& camera) {
                return camera.locked || std::ranges::any_of(camera.clips, &state::editing::model::Clip::locked);
            });
        if (property::actionButton("playback.refactorEditor.details.resetClipsToFull"_tr().c_str(), canReset)) {
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
        // Mirrors what the export dialog will start with: its frame rate and the I/O range (-1 means unset).
        int const rangeStart = std::clamp(ctx.commands.exportStartTick(), 0, state.totalTicks);
        int const rangeEnd   = ctx.commands.exportEndTick() < 0
                                 ? state.totalTicks
                                 : std::clamp(ctx.commands.exportEndTick(), 0, state.totalTicks);
        property::textRow(
            "playback.refactorEditor.export.frameRate"_tr().c_str(),
            (std::to_string(ctx.commands.exportFps()) + " FPS").c_str()
        );
        property::textRow(
            "playback.refactorEditor.export.timeline"_tr().c_str(),
            (formatTick(rangeStart) + " - " + formatTick(std::max(rangeStart, rangeEnd))).c_str()
        );
        if (!state.capabilities.ffmpegVideoExport) {
            ImGui::TextDisabled("%s  %s", ICON_INFO, "playback.refactorEditor.export.ffmpegUnavailable"_tr().c_str());
        }
        if (property::actionButton(
                "playback.refactorEditor.inspector.openExportDialog"_tr().c_str(),
                state.capabilities.videoExport && !exportActive && state.totalTicks > 0
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
                ImGui::ProgressBar(fraction, {-1.0f, ImGui::GetFontSize() + 4.0f});
            }
            if (!status.message.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(status.message.c_str());
                ImGui::PopTextWrapPos();
            }
            if (property::actionButton("playback.refactorEditor.export.cancel"_tr().c_str(), exportActive)) {
                ctx.submitAction({EditorActionType::CancelExport});
            }
            if (status.state == exporting::ExportState::Completed && !status.outputPath.empty()
                && property::actionButton("playback.refactorEditor.export.showInFolder"_tr().c_str(), true)) {
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
        for (auto const& toggle : toggles) {
            bool shown = ctx.commands.isHudElementShown(toggle.element);
            ImGui::PushID(static_cast<int>(toggle.element));
            if (ImGui::Checkbox((toggle.label + "##set-hud").c_str(), &shown))
                ctx.commands.setHudElementShown(toggle.element, shown);
            ImGui::PopID();
            if (toggle.element == visuals::HudElement::Hotbar)
                widgets::itemTooltip("playback.refactorEditor.inspector.hudHotbarHint"_tr().c_str());
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
        std::array const tiers{
            Tier{UiScaleTier::Auto,   "playback.refactorEditor.inspector.uiScaleAuto"_tr()  },
            Tier{UiScaleTier::Small,  "playback.refactorEditor.inspector.uiScaleSmall"_tr() },
            Tier{UiScaleTier::Medium, "playback.refactorEditor.inspector.uiScaleMedium"_tr()},
            Tier{UiScaleTier::Large,  "playback.refactorEditor.inspector.uiScaleLarge"_tr() },
            Tier{UiScaleTier::Huge,   "playback.refactorEditor.inspector.uiScaleHuge"_tr()  },
        };
        UiScaleTier const current = ctx.commands.uiScaleTier();
        for (auto const& entry : tiers) {
            if (ImGui::RadioButton(entry.label.c_str(), current == entry.tier)) ctx.commands.setUiScaleTier(entry.tier);
        }
        if (current == UiScaleTier::Auto) {
            ImGui::TextDisabled(
                "%s",
                "playback.refactorEditor.inspector.uiScaleAutoHint"_tr(
                    static_cast<int>(calculateReplayUIScale(current, ImGui::GetIO().DisplaySize.y) * 100.0f)
                )
                    .c_str()
            );
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
        float const current = ctx.commands.videoAspectRatio();
        for (auto const& preset : widgets::kAspectPresets) {
            if (ImGui::RadioButton(preset.label, std::abs(current - preset.ratio) < 0.01f))
                ctx.commands.setVideoAspectRatio(preset.ratio);
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
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.optiPistonHint"_tr().c_str());
    property::endSection();
}

} // namespace playback::editor::ui

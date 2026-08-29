#include "DetailsPanel.h"

#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"

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

constexpr int kTicksPerSecond = 20;

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

using widgets::formatTick;

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

std::string categoryName(state::editing::model::SubActorCategory category) {
    using Category = state::editing::model::SubActorCategory;
    switch (category) {
    case Category::Players:
        return "playback.refactorEditor.details.categoryName.players"_tr();
    case Category::Creatures:
        return "playback.refactorEditor.details.categoryName.creatures"_tr();
    case Category::Entities:
        return "playback.refactorEditor.details.categoryName.entities"_tr();
    case Category::Default:
    default:
        return "playback.refactorEditor.details.categoryName.default"_tr();
    }
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

} // namespace

void DetailsPanel::draw(PanelContext const& ctx) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) {
        widgets::noActiveProjectPlaceholder();
        return;
    }

    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };
    auto const& selection = ctx.selection;
    char        search[128]{};
    std::string subject = "playback.refactorEditor.details.noSelection"_tr();
    if (selection.getAs<state::editing::model::SelectedWorldActor>()) {
        subject = "playback.refactorEditor.details.worldActor"_tr();
    } else if (selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        subject = "playback.refactorEditor.details.worldActorSegment"_tr();
    } else if (selection.getAs<state::editing::model::SelectedSubActor>()) {
        subject = "playback.refactorEditor.details.subActor"_tr();
    } else if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        subject            = camera ? camera->name : "playback.refactorEditor.details.camera"_tr();
    } else if (selection.getAs<state::editing::model::SelectedKeyframe>()) {
        subject = "playback.refactorEditor.details.cameraKeyframe"_tr();
    } else if (selection.getAs<state::editing::model::SelectedMarker>()) {
        subject = "playback.refactorEditor.details.marker"_tr();
    }
    auto const inspectorTitle = "playback.refactorEditor.details.title"_tr();
    auto const searchHint     = "playback.refactorEditor.details.searchProperties"_tr();
    property::beginInspector(inspectorTitle, subject);
    property::searchBar("##details-search", searchHint.c_str(), search, sizeof(search));

    if (selection.getAs<state::editing::model::SelectedWorldActor>()) {
        if (property::beginSection("playback.refactorEditor.details.worldActor"_tr().c_str())) {
            ImGui::TextUnformatted("playback.refactorEditor.details.name"_tr(
                                       project->worldActor.name.empty()
                                           ? "playback.refactorEditor.details.untitled"_tr()
                                           : project->worldActor.name
            )
                                       .c_str());
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.total"_tr(formatTick(project->worldActor.totalTicks)).c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.segments"_tr(project->worldActor.segments.size()).c_str()
            );
            for (auto const& segment : project->worldActor.segments) {
                if (ImGui::Selectable((formatTick(segment.startTick) + " - " + formatTick(segment.endTick) + "  "
                                       + std::to_string(segment.speed) + "x")
                                          .c_str())) {
                    ctx.selection.select(state::editing::model::SelectedWorldActorSegment{segment.id});
                }
            }
            property::separator();
            if (property::beginSection("playback.refactorEditor.details.subActors"_tr().c_str())) {
                static constexpr std::array<state::editing::model::SubActorCategory, 4> categories{
                    state::editing::model::SubActorCategory::Default,
                    state::editing::model::SubActorCategory::Players,
                    state::editing::model::SubActorCategory::Creatures,
                    state::editing::model::SubActorCategory::Entities,
                };
                for (auto category : categories) {
                    auto const&  actors = project->worldActor.subActors;
                    size_t const count =
                        static_cast<size_t>(std::count_if(actors.begin(), actors.end(), [category](auto const& actor) {
                            return actor.category == category;
                        }));
                    std::string const header = categoryName(category) + " (" + std::to_string(count) + ")";
                    if (count > 0 && ImGui::CollapsingHeader(header.c_str())) {
                        size_t shown = 0;
                        for (auto const& actor : actors) {
                            if (actor.category != category) continue;
                            if (shown >= 100) {
                                ImGui::TextDisabled(
                                    "%s",
                                    "playback.refactorEditor.details.moreActors"_tr(count - shown).c_str()
                                );
                                break;
                            }
                            ++shown;
                            if (ImGui::Selectable(actor.name.empty() ? actor.id.c_str() : actor.name.c_str())) {
                                ctx.selection.select(state::editing::model::SelectedSubActor{actor.id});
                            }
                        }
                    }
                }
                property::endSection();
            }
            property::endSection();
        }
        return;
    }

    if (auto const* selected = selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.worldActorSegmentMissing"_tr().c_str());
            return;
        }
        bool const isFirst = &project->worldActor.segments.front() == segment;
        bool const isLast  = &project->worldActor.segments.back() == segment;
        if (property::beginSection("playback.refactorEditor.details.worldActorSegment"_tr().c_str())) {
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.range"_tr(formatTick(segment->startTick), formatTick(segment->endTick))
                    .c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.duration"_tr(segment->endTick - segment->startTick).c_str()
            );
            ImGui::TextUnformatted("playback.refactorEditor.details.sourceTick"_tr(segment->sourceTick).c_str());
            ImGui::BeginDisabled(segment->locked);
            int        startTick  = segment->startTick;
            int        endTick    = segment->endTick;
            auto const startLabel = "playback.refactorEditor.details.start"_tr();
            auto const endLabel   = "playback.refactorEditor.details.end"_tr();
            ImGui::BeginDisabled(isFirst);
            if (ImGui::InputInt(startLabel.c_str(), &startTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = std::clamp(startTick, 1, endTick - 1);
                action.kind = endTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(isLast);
            if (ImGui::InputInt(endLabel.c_str(), &endTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = startTick;
                action.kind = std::clamp(endTick, startTick + 1, project->totalTicks);
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            float      speed      = segment->speed;
            auto const speedLabel = "playback.refactorEditor.details.speed"_tr();
            if (ImGui::SliderFloat(speedLabel.c_str(), &speed, 0.1f, 10.0f, "%.2fx")
                && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::SetWorldActorSpeed};
                action.id    = segment->id;
                action.speed = speed;
                submit(std::move(action));
            }
            if (property::actionButton("playback.refactorEditor.details.splitAtPlayhead"_tr().c_str())) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = state.currentTick;
                submit(std::move(action));
            }
            if (property::actionButton("playback.refactorEditor.details.rippleDelete"_tr().c_str())) {
                EditorAction action{EditorActionType::RippleDeleteWorldActorSegment};
                action.id = segment->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (segment->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.segmentLocked"_tr().c_str());
            }
            property::endSection();
        }
        return;
    }

    if (auto const* selected = selection.getAs<state::editing::model::SelectedSubActor>()) {
        auto const* actor = findById(project->worldActor.subActors, selected->subActorId);
        if (!actor) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.subActorMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("playback.refactorEditor.details.subActor"_tr().c_str())) {
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.name"_tr(actor->name.empty() ? actor->id : actor->name).c_str()
            );
            ImGui::TextUnformatted("playback.refactorEditor.details.category"_tr(categoryName(actor->category)).c_str()
            );
            ImGui::TextUnformatted("playback.refactorEditor.details.positionValue"_tr(
                                       actor->position.x,
                                       actor->position.y,
                                       actor->position.z
            )
                                       .c_str());
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.rotationValue"_tr(actor->rotation.x, actor->rotation.y).c_str()
            );
            if (actor->boundCameraIds.empty()) {
                ImGui::TextUnformatted("playback.refactorEditor.details.noBoundCameras"_tr().c_str());
            } else {
                ImGui::TextUnformatted("playback.refactorEditor.details.boundCamerasLabel"_tr().c_str());
                for (auto const& cameraId : actor->boundCameraIds) {
                    auto const* camera = findById(project->cameras, cameraId);
                    ImGui::BulletText("%s", camera ? camera->name.c_str() : cameraId.c_str());
                }
            }
            property::separator();
            if (!actor->agentDetails.empty()) {
                if (property::beginSection("playback.refactorEditor.details.agentDetails"_tr().c_str())) {
                    auto updated = actor->agentDetails;
                    bool edited  = false;
                    for (auto& [key, value] : updated) {
                        char buf[256]{};
                        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(("##agent-" + key).c_str(), buf, sizeof(buf))) value = buf;
                        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
                    }
                    if (edited) {
                        EditorAction action{EditorActionType::SetSubActorDetails};
                        action.id      = actor->id;
                        action.details = std::move(updated);
                        submit(std::move(action));
                    }
                    property::endSection();
                }
            }
            static char newDetailKey[64]{};
            auto const  newFieldHint = "playback.refactorEditor.details.newDetailField"_tr();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##agent-new-key", newFieldHint.c_str(), newDetailKey, sizeof(newDetailKey));
            if (property::actionButton("playback.refactorEditor.details.addDetailField"_tr().c_str())
                && newDetailKey[0] != '\0') {
                auto details          = actor->agentDetails;
                details[newDetailKey] = "";
                EditorAction action{EditorActionType::SetSubActorDetails};
                action.id      = actor->id;
                action.details = std::move(details);
                submit(std::move(action));
                newDetailKey[0] = '\0';
            }
            ImGui::Spacing();
            if (property::actionButton("playback.refactorEditor.details.createBindingCamera"_tr().c_str())) {
                EditorAction action{EditorActionType::CreateBindingCamera};
                action.id   = actor->id;
                action.name = "playback.refactorEditor.details.bindingCameraName"_tr(actor->name);
                submit(std::move(action));
            }
            property::endSection();
        }
        return;
    }

    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("playback.refactorEditor.details.camera"_tr().c_str())) {
            property::textRow("playback.refactorEditor.details.nameLabel"_tr().c_str(), camera->name.c_str());
            std::string const keyframeCount = std::to_string(camera->keysByTick.size());
            property::textRow("playback.refactorEditor.details.keyframesLabel"_tr().c_str(), keyframeCount.c_str());
            bool       enabled      = camera->enabled;
            auto const enabledLabel = "playback.refactorEditor.details.enabled"_tr();
            if (ImGui::Checkbox(enabledLabel.c_str(), &enabled)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = camera->id;
                action.value = enabled;
                submit(std::move(action));
            }
            ImGui::BeginDisabled(camera->locked);
            if (!camera->bindingEntityUuid.empty()) {
                property::separator();
                auto const* actor = findById(project->worldActor.subActors, camera->bindingEntityUuid);
                ImGui::TextUnformatted(
                    "playback.refactorEditor.details.boundTo"_tr(actor ? actor->name : camera->bindingEntityUuid)
                        .c_str()
                );
                if (property::actionButton("playback.refactorEditor.details.unbindCamera"_tr().c_str())) {
                    EditorAction action{EditorActionType::UnbindCamera};
                    action.id = camera->id;
                    submit(std::move(action));
                }
            }
            property::separator();
            if (property::actionButton("playback.refactorEditor.details.addKeyframeAtPlayhead"_tr().c_str())) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = state.currentTick;
                submit(std::move(action));
            }
            for (auto const& [keyTick, _] : camera->keysByTick) {
                auto const* selectedKey = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
                bool const selected = selectedKey && selectedKey->trackId == camera->id && selectedKey->tick == keyTick;
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(176, 128, 18, 255));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(205, 157, 32, 255));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(232, 184, 45, 255));
                if (ImGui::Selectable("playback.refactorEditor.details.tickValue"_tr(keyTick).c_str(), selected)) {
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, keyTick});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera->id;
                    submit(std::move(previewAction));
                    ctx.commands.seekTo(keyTick);
                }
                ImGui::PopStyleColor(3);
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
        return;
    }

    if (auto const* selected = selection.getAs<state::editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return;
        }
        auto const key = camera->keysByTick.find(selected->tick);
        if (key == camera->keysByTick.end()) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("playback.refactorEditor.details.cameraKeyframe"_tr().c_str())) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.camera"_tr().c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(camera->name.c_str());
            property::separator();
            ImGui::BeginDisabled(camera->locked);
            int const       originalTick = key->first;
            int             tick         = originalTick;
            constexpr float labelWidth   = 72.0f;
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

                float position[3] = {key->second.position.x, key->second.position.y, key->second.position.z};
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.position"_tr().c_str());
                ImGui::TableNextColumn();
                static constexpr std::array positionLabels{"X", "Y", "Z"};
                static constexpr std::array positionColors{
                    ImVec4{0.95f, 0.38f, 0.38f, 1.0f},
                    ImVec4{0.42f, 0.82f, 0.45f, 1.0f},
                    ImVec4{0.38f, 0.62f, 0.96f, 1.0f},
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
                static constexpr std::array rotationLabels{"Yaw", "Pitch", "Roll"};
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
                ImGui::TextDisabled("FOV");
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
                if (ImGui::BeginCombo("##interpolation", interpolationName(key->second.interpolationType).c_str())) {
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
                    ImGui::EndCombo();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
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
        return;
    }

    if (auto const* selected = selection.getAs<state::editing::model::SelectedMarker>()) {
        auto const* marker = findById(project->markers, selected->markerId);
        if (!marker) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.markerMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("playback.refactorEditor.details.marker"_tr().c_str())) {
            property::textRow("playback.refactorEditor.details.nameLabel"_tr().c_str(), marker->label.c_str());
            property::textRow("playback.refactorEditor.details.tick"_tr().c_str(), formatTick(marker->tick).c_str());
            ImGui::BeginDisabled();
            if (property::actionButton("playback.refactorEditor.details.deleteMarker"_tr().c_str(), false)) {}
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.markerNotWired"_tr().c_str());
            property::endSection();
        }
        return;
    }

    if (property::beginSection("playback.refactorEditor.details.overview"_tr().c_str())) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.selectionHint"_tr().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.details.name"_tr(
                                   project->worldActor.name.empty() ? "playback.refactorEditor.details.untitled"_tr()
                                                                    : project->worldActor.name
        )
                                   .c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.total"_tr(formatTick(project->totalTicks)).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.cameras"_tr(project->cameras.size()).c_str());
        ImGui::TextUnformatted(
            "playback.refactorEditor.details.segments"_tr(project->worldActor.segments.size()).c_str()
        );
        ImGui::Spacing();
        if (property::actionButton("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            EditorAction action{EditorActionType::AddFreeCamera};
            action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
            submit(std::move(action));
        }
        property::endSection();
    }
}

} // namespace playback::editor::ui

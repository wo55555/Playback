#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/components/PropertyControls.h"

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

std::string formatTick(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    int const centiseconds = tick % kTicksPerSecond * (100 / kTicksPerSecond);
    std::snprintf(value, sizeof(value), "%02d:%02d.%02d", totalSeconds / 60, totalSeconds % 60, centiseconds);
    return value;
}

char const* interpolationName(state::editing::model::CameraInterpolationType interpolation) {
    static constexpr std::array
        names{"Smooth", "Linear", "Ease In", "Ease Out", "Ease InOut", "Hold", "Hermite", "Cubic Bezier"};
    return names[std::clamp(static_cast<int>(interpolation), 0, static_cast<int>(names.size()) - 1)];
}

char const* categoryName(state::editing::model::SubActorCategory category) {
    static constexpr std::array names{"Default", "Players", "Creatures", "Entities"};
    return names[std::clamp(static_cast<int>(category), 0, static_cast<int>(names.size()) - 1)];
}

void submit(EditorAction action) { ReplayEditor::getInstance().submitAction(std::move(action)); }

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

void DetailsPanel::draw() {
    auto&       editor  = ReplayEditor::getInstance();
    auto const& state   = editor.state();
    auto const  project = state.project;
    if (!project) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
        return;
    }

    auto const& selection = editor.selection();
    char        search[128]{};
    property::beginInspector("Details", "Property Inspector");
    property::searchBar("##details-search", "Search properties", search, sizeof(search));

    // ===== World Actor (overview + sub actor tree) =====
    if (selection.getAs<state::editing::model::SelectedWorldActor>()) {
        if (property::beginSection("World Actor")) {
            ImGui::Text("Name: %s", project->worldActor.name.empty() ? "(untitled)" : project->worldActor.name.c_str());
            ImGui::Text("Total: %s", formatTick(project->worldActor.totalTicks).c_str());
            ImGui::Text("Segments: %zu", project->worldActor.segments.size());
            for (auto const& segment : project->worldActor.segments) {
                if (ImGui::Selectable((formatTick(segment.startTick) + " - " + formatTick(segment.endTick) + "  "
                                       + std::to_string(segment.speed) + "x")
                                          .c_str())) {
                    editor.selection().select(state::editing::model::SelectedWorldActorSegment{segment.id});
                }
            }
            property::separator();
            if (property::beginSection("Sub Actors")) {
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
                    char header[64]{};
                    std::snprintf(header, sizeof(header), "%s (%zu)", categoryName(category), count);
                    if (count > 0 && ImGui::CollapsingHeader(header)) {
                        size_t shown = 0;
                        for (auto const& actor : actors) {
                            if (actor.category != category) continue;
                            if (shown >= 100) {
                                ImGui::TextDisabled("... %zu more", count - shown);
                                break;
                            }
                            ++shown;
                            if (ImGui::Selectable(actor.name.empty() ? actor.id.c_str() : actor.name.c_str())) {
                                editor.selection().select(state::editing::model::SelectedSubActor{actor.id});
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

    // ===== World Actor Segment =====
    if (auto const* selected = selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.worldActorSegmentMissing"_tr().c_str());
            return;
        }
        bool const isFirst = &project->worldActor.segments.front() == segment;
        bool const isLast  = &project->worldActor.segments.back() == segment;
        if (property::beginSection("World Actor Segment")) {
            ImGui::Text("Range: %s - %s", formatTick(segment->startTick).c_str(), formatTick(segment->endTick).c_str());
            ImGui::Text("Duration: %d ticks", segment->endTick - segment->startTick);
            ImGui::Text("Source Tick: %d", segment->sourceTick);
            ImGui::BeginDisabled(segment->locked);
            int startTick = segment->startTick;
            int endTick   = segment->endTick;
            ImGui::BeginDisabled(isFirst);
            if (ImGui::InputInt("Start", &startTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = std::clamp(startTick, 1, endTick - 1);
                action.kind = endTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(isLast);
            if (ImGui::InputInt("End", &endTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = startTick;
                action.kind = std::clamp(endTick, startTick + 1, project->totalTicks);
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            float speed = segment->speed;
            if (ImGui::SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.2fx") && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::SetWorldActorSpeed};
                action.id    = segment->id;
                action.speed = speed;
                submit(std::move(action));
            }
            if (property::actionButton("Split at Playhead")) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = state.currentTick;
                submit(std::move(action));
            }
            if (property::actionButton("Ripple Delete")) {
                EditorAction action{EditorActionType::RippleDeleteWorldActorSegment};
                action.id = segment->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (segment->locked) ImGui::TextDisabled("Segment is locked.");
            property::endSection();
        }
        return;
    }

    // ===== Sub Actor =====
    if (auto const* selected = selection.getAs<state::editing::model::SelectedSubActor>()) {
        auto const* actor = findById(project->worldActor.subActors, selected->subActorId);
        if (!actor) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.subActorMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("Sub Actor")) {
            ImGui::Text("Name: %s", actor->name.empty() ? actor->id.c_str() : actor->name.c_str());
            ImGui::Text("Category: %s", categoryName(actor->category));
            ImGui::Text("Position: (%.1f, %.1f, %.1f)", actor->position.x, actor->position.y, actor->position.z);
            ImGui::Text("Rotation: (%.1f, %.1f)", actor->rotation.x, actor->rotation.y);
            if (actor->boundCameraIds.empty()) {
                ImGui::Text("Bound Cameras: none");
            } else {
                ImGui::Text("Bound Cameras:");
                for (auto const& cameraId : actor->boundCameraIds) {
                    auto const* camera = findById(project->cameras, cameraId);
                    ImGui::BulletText("%s", camera ? camera->name.c_str() : cameraId.c_str());
                }
            }
            property::separator();
            if (!actor->agentDetails.empty()) {
                if (property::beginSection("Agent Details")) {
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
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##agent-new-key", "New detail field name", newDetailKey, sizeof(newDetailKey));
            if (property::actionButton("Add Detail Field") && newDetailKey[0] != '\0') {
                auto details          = actor->agentDetails;
                details[newDetailKey] = "";
                EditorAction action{EditorActionType::SetSubActorDetails};
                action.id      = actor->id;
                action.details = std::move(details);
                submit(std::move(action));
                newDetailKey[0] = '\0';
            }
            ImGui::Spacing();
            if (property::actionButton("Create Binding Camera")) {
                EditorAction action{EditorActionType::CreateBindingCamera};
                action.id   = actor->id;
                action.name = "playback.refactorEditor.details.bindingCameraName"_tr(actor->name);
                submit(std::move(action));
            }
            property::endSection();
        }
        return;
    }

    // ===== Camera =====
    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("Camera")) {
            property::textRow("Name", camera->name.c_str());
            std::string const keyframeCount = std::to_string(camera->keysByTick.size());
            property::textRow("Keyframes", keyframeCount.c_str());
            bool enabled = camera->enabled;
            if (ImGui::Checkbox("Enabled", &enabled)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = camera->id;
                action.value = enabled;
                submit(std::move(action));
            }
            ImGui::BeginDisabled(camera->locked);
            ImGui::Text("Source: KeyframeTrack");
            ImGui::Separator();
            if (!camera->bindingEntityUuid.empty()) {
                auto const* actor = findById(project->worldActor.subActors, camera->bindingEntityUuid);
                ImGui::Text("Bound to: %s", actor ? actor->name.c_str() : camera->bindingEntityUuid.c_str());
                ImGui::Text("Binding Mode: %d", camera->bindingMode);
                ImGui::Text("Damping: %.2f", camera->bindingDamping);
                if (property::actionButton("Unbind Camera")) {
                    EditorAction action{EditorActionType::UnbindCamera};
                    action.id = camera->id;
                    submit(std::move(action));
                }
                ImGui::Separator();
            }
            if (camera->shake) {
                ImGui::Text("Shake: %d - %d", camera->shake->startTick, camera->shake->endTick);
            }
            property::separator();
            if (property::actionButton("Add Keyframe at Playhead")) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = state.currentTick;
                submit(std::move(action));
            }
            for (auto const& [keyTick, _] : camera->keysByTick) {
                auto const* selectedKey = editor.selection().getAs<state::editing::model::SelectedKeyframe>();
                bool const selected = selectedKey && selectedKey->trackId == camera->id && selectedKey->tick == keyTick;
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(176, 128, 18, 255));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(205, 157, 32, 255));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(232, 184, 45, 255));
                if (ImGui::Selectable(("Tick " + std::to_string(keyTick)).c_str(), selected)) {
                    editor.selection().select(state::editing::model::SelectedKeyframe{camera->id, keyTick});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera->id;
                    submit(std::move(previewAction));
                    editor.seekTo(keyTick);
                }
                ImGui::PopStyleColor(3);
            }
            property::separator();
            if (property::actionButton("Delete Camera")) {
                EditorAction action{EditorActionType::DeleteCamera};
                action.id = camera->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) ImGui::TextDisabled("Camera is locked.");
            property::endSection();
        }
        return;
    }

    // ===== Camera Keyframe =====
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
        if (property::beginSection("Camera Keyframe")) {
            ImGui::TextDisabled("Camera");
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
                ImGui::TextDisabled("Tick");
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
                ImGui::TextDisabled("Position");
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
                ImGui::TextDisabled("Rotation");
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
                ImGui::TextDisabled("Interpolation");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##interpolation", interpolationName(key->second.interpolationType))) {
                    for (int index = 0; index < 8; ++index) {
                        auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                        if (ImGui::Selectable(interpolationName(value), index == interpolation)) {
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
            if (property::actionButton("Delete Keyframe")) {
                EditorAction action{EditorActionType::DeleteCameraKeyframe};
                action.id   = camera->id;
                action.tick = originalTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) ImGui::TextDisabled("Camera is locked.");
            property::endSection();
        }
        return;
    }

    // ===== Marker =====
    if (auto const* selected = selection.getAs<state::editing::model::SelectedMarker>()) {
        auto const* marker = findById(project->markers, selected->markerId);
        if (!marker) {
            ImGui::TextDisabled("Marker no longer exists.");
            return;
        }
        if (property::beginSection("Marker")) {
            ImGui::Text("Label: %s", marker->label.c_str());
            ImGui::Text("Tick: %s", formatTick(marker->tick).c_str());
            ImGui::BeginDisabled();
            if (property::actionButton("Delete Marker", false)) {}
            ImGui::EndDisabled();
            ImGui::TextDisabled("Marker editing is not yet wired to the backend.");
            property::endSection();
        }
        return;
    }

    // ===== Empty state =====
    if (property::beginSection("Replay Overview")) {
        ImGui::TextDisabled("Select a world actor, camera, keyframe or marker.");
        ImGui::Spacing();
        ImGui::Text("Replay: %s", project->worldActor.name.empty() ? "(untitled)" : project->worldActor.name.c_str());
        ImGui::Text("Duration: %s", formatTick(project->totalTicks).c_str());
        ImGui::Text("Cameras: %zu", project->cameras.size());
        ImGui::Text("World actor segments: %zu", project->worldActor.segments.size());
        ImGui::Spacing();
        if (property::actionButton("Add Free Camera")) {
            EditorAction action{EditorActionType::AddFreeCamera};
            action.name = "playback.refactorEditor.defaults.camera"_tr(project->cameras.size() + 1);
            submit(std::move(action));
        }
        property::endSection();
    }
}

} // namespace playback::editor::ui

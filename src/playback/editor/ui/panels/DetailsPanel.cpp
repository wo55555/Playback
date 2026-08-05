#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/components/PropertyControls.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

std::string formatTick(int tick) {
    char value[32]{};
    tick = std::max(0, tick);
    std::snprintf(value, sizeof(value), "%02d:%02d", tick / 1200, (tick / 20) % 60);
    return value;
}

char const* cameraKindName(editing::model::CameraKind kind) {
    static constexpr std::array names{"Keyframe", "Path", "Rig", "Preset"};
    return names[std::clamp(static_cast<int>(kind), 0, static_cast<int>(names.size()) - 1)];
}

char const* easingName(editing::model::EasingType easing) {
    static constexpr std::array names{"Linear", "Ease In", "Ease Out", "Ease InOut", "Cubic Bezier"};
    return names[std::clamp(static_cast<int>(easing), 0, static_cast<int>(names.size()) - 1)];
}

char const* categoryName(editing::model::SubActorCategory category) {
    static constexpr std::array names{"Default", "Players", "Creatures", "Entities"};
    return names[std::clamp(static_cast<int>(category), 0, static_cast<int>(names.size()) - 1)];
}

char const* presetKindName(editing::model::PresetKind kind) {
    static constexpr std::array names{"First Person", "Third Person", "Free", "Follow Entity", "Orbit", "Telephoto", "Drone"};
    return names[std::clamp(static_cast<int>(kind), 0, static_cast<int>(names.size()) - 1)];
}

char const* splineTypeName(editing::model::SplineType type) {
    static constexpr std::array names{"Linear", "Catmull-Rom", "Cubic Bezier"};
    return names[std::clamp(static_cast<int>(type), 0, static_cast<int>(names.size()) - 1)];
}

void submit(EditorAction action) {
    ReplayEditor::getInstance().submitAction(std::move(action));
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
    char search[128]{};
    property::beginInspector("Details", "Property Inspector");
    property::searchBar("##details-search", "Search properties", search, sizeof(search));

    // ===== Camera Sequence (overview) =====
    if (selection.getAs<editing::model::SelectedSequence>()) {
        if (property::beginSection("Camera Sequence")) {
        ImGui::Text("Duration: %s", formatTick(project->totalTicks).c_str());
        ImGui::Text("Segments: %zu", project->sequence.size());
        int unbound = 0;
        for (auto const& segment : project->sequence) {
            if (segment.cameraId.empty() || !findById(project->cameras, segment.cameraId)) ++unbound;
        }
        if (project->cameras.empty()) {
            ImGui::TextColored(ImVec4(0.94f, 0.75f, 0.13f, 1.0f), "No cameras exist - add one to bind segments.");
        } else if (unbound > 0) {
            ImGui::TextColored(ImVec4(0.94f, 0.75f, 0.13f, 1.0f), "%d segment(s) fall back to the first camera.", unbound);
        } else {
            ImGui::TextColored(ImVec4(0.47f, 0.78f, 0.51f, 1.0f), "All segments bound.");
        }
        property::separator();
        for (auto const& segment : project->sequence) {
            auto const* camera = findById(project->cameras, segment.cameraId);
            char const* label = camera ? camera->name.c_str()
                : (segment.cameraId.empty() ? (project->cameras.empty() ? "(no camera)" : "(auto)") : "(missing)");
            if (ImGui::Selectable((formatTick(segment.startTick) + " - " + formatTick(segment.endTick) + "  " + label).c_str())) {
                editor.selection().select(editing::model::SelectedSequenceSegment{segment.id});
            }
        }
        ImGui::Spacing();
        if (property::actionButton("Split at Playhead")) {
            EditorAction action{EditorActionType::SplitSequence};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        property::endSection();
        }
        return;
    }

    // ===== Sequence Segment =====
    if (auto const* selected = selection.getAs<editing::model::SelectedSequenceSegment>()) {
        auto const* segment = findById(project->sequence, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.sequenceSegmentMissing"_tr().c_str());
            return;
        }
        bool const isFirst = &project->sequence.front() == segment;
        bool const isLast = &project->sequence.back() == segment;
        if (property::beginSection("Sequence Segment")) {
        ImGui::Text("Range: %s - %s", formatTick(segment->startTick).c_str(), formatTick(segment->endTick).c_str());
        ImGui::Text("Duration: %d ticks", segment->endTick - segment->startTick);
        ImGui::BeginDisabled(segment->locked);
        int startTick = segment->startTick;
        int endTick = segment->endTick;
        ImGui::BeginDisabled(isFirst);
        if (ImGui::InputInt("Start", &startTick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::TrimSequence};
            action.id = segment->id;
            action.tick = std::clamp(startTick, 1, endTick - 1);
            action.kind = endTick;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(isLast);
        if (ImGui::InputInt("End", &endTick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::TrimSequence};
            action.id = segment->id;
            action.tick = startTick;
            action.kind = std::clamp(endTick, startTick + 1, project->totalTicks);
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        char const* preview = "Automatic: first camera";
        if (auto const* camera = findById(project->cameras, segment->cameraId)) preview = camera->name.c_str();
        if (ImGui::BeginCombo("Camera", preview)) {
            if (ImGui::Selectable("Automatic: first camera", segment->cameraId.empty())) {
                EditorAction action{EditorActionType::BindSequenceCamera};
                action.id = segment->id;
                submit(std::move(action));
            }
            for (auto const& camera : project->cameras) {
                if (ImGui::Selectable(camera.name.c_str(), camera.id == segment->cameraId)) {
                    EditorAction action{EditorActionType::BindSequenceCamera};
                    action.id          = segment->id;
                    action.secondaryId = camera.id;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (property::actionButton("Split at Playhead")) {
            EditorAction action{EditorActionType::SplitSequence};
            action.tick = state.currentTick;
            submit(std::move(action));
        }
        if (property::actionButton("Delete Segment")) {
            EditorAction action{EditorActionType::DeleteSequenceSegment};
            action.id = segment->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        if (segment->locked) ImGui::TextDisabled("Segment is locked.");
        property::endSection();
        }
        return;
    }

    // ===== World Actor (overview + sub actor tree) =====
    if (selection.getAs<editing::model::SelectedWorldActor>()) {
        if (property::beginSection("World Actor")) {
        ImGui::Text("Name: %s", project->worldActor.name.empty() ? "(untitled)" : project->worldActor.name.c_str());
        ImGui::Text("Total: %s", formatTick(project->worldActor.totalTicks).c_str());
        ImGui::Text("Segments: %zu", project->worldActor.segments.size());
        for (auto const& segment : project->worldActor.segments) {
            if (ImGui::Selectable((formatTick(segment.startTick) + " - " + formatTick(segment.endTick) + "  " + std::to_string(segment.speed) + "x").c_str())) {
                editor.selection().select(editing::model::SelectedWorldActorSegment{segment.id});
            }
        }
        property::separator();
        if (property::beginSection("Sub Actors")) {
        static constexpr std::array<editing::model::SubActorCategory, 4> categories{
            editing::model::SubActorCategory::Default,
            editing::model::SubActorCategory::Players,
            editing::model::SubActorCategory::Creatures,
            editing::model::SubActorCategory::Entities,
        };
        for (auto category : categories) {
            auto const& actors = project->worldActor.subActors;
            size_t const count = static_cast<size_t>(std::count_if(actors.begin(), actors.end(), [category](auto const& actor) { return actor.category == category; }));
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
                        editor.selection().select(editing::model::SelectedSubActor{actor.id});
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
    if (auto const* selected = selection.getAs<editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.worldActorSegmentMissing"_tr().c_str());
            return;
        }
        bool const isFirst = &project->worldActor.segments.front() == segment;
        bool const isLast = &project->worldActor.segments.back() == segment;
        if (property::beginSection("World Actor Segment")) {
        ImGui::Text("Range: %s - %s", formatTick(segment->startTick).c_str(), formatTick(segment->endTick).c_str());
        ImGui::Text("Duration: %d ticks", segment->endTick - segment->startTick);
        ImGui::Text("Source Tick: %d", segment->sourceTick);
        ImGui::BeginDisabled(segment->locked);
        int startTick = segment->startTick;
        int endTick = segment->endTick;
        ImGui::BeginDisabled(isFirst);
        if (ImGui::InputInt("Start", &startTick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::TrimWorldActor};
            action.id = segment->id;
            action.tick = std::clamp(startTick, 1, endTick - 1);
            action.kind = endTick;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(isLast);
        if (ImGui::InputInt("End", &endTick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::TrimWorldActor};
            action.id = segment->id;
            action.tick = startTick;
            action.kind = std::clamp(endTick, startTick + 1, project->totalTicks);
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        float speed = segment->speed;
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.2fx") && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::SetWorldActorSpeed};
            action.id = segment->id;
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
    if (auto const* selected = selection.getAs<editing::model::SelectedSubActor>()) {
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
            bool edited = false;
            for (auto& [key, value] : updated) {
                char buf[256]{};
                std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText(("##agent-" + key).c_str(), buf, sizeof(buf))) value = buf;
                if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
            }
            if (edited) {
                EditorAction action{EditorActionType::SetSubActorDetails};
                action.id = actor->id;
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
            auto details = actor->agentDetails;
            details[newDetailKey] = "";
            EditorAction action{EditorActionType::SetSubActorDetails};
            action.id = actor->id;
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
    if (auto const* selectedCamera = selection.getAs<editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("Camera")) {
        ImGui::Text("Name: %s", camera->name.c_str());
        ImGui::Text("Keyframes: %zu", camera->keys.size());
        ImGui::BeginDisabled(camera->locked);
        std::string const selectedKindName = cameraKindName(camera->kind);
        if (ImGui::BeginCombo("playback.refactorEditor.details.kind"_tr().c_str(), selectedKindName.c_str())) {
            for (int index = 0; index < 4; ++index) {
                auto const        kind     = static_cast<editing::model::CameraKind>(index);
                std::string const kindName = cameraKindName(kind);
                if (ImGui::Selectable(kindName.c_str(), kind == camera->kind)) {
                    EditorAction action{EditorActionType::SetCameraKind};
                    action.id   = camera->id;
                    action.kind = index;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
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
        switch (camera->kind) {
        case editing::model::CameraKind::Keyframe:
            break;
        case editing::model::CameraKind::Path:
            if (camera->path) {
                ImGui::Text("Path: %s", splineTypeName(camera->path->type));
                ImGui::Text("Points: %zu", camera->path->points.size());
            } else {
                ImGui::TextDisabled("No path data.");
            }
            break;
        case editing::model::CameraKind::Rig:
            if (camera->rig) {
                ImGui::Text("Rig base: (%.0f, %.0f, %.0f)", camera->rig->basePosition.x, camera->rig->basePosition.y, camera->rig->basePosition.z);
                ImGui::Text("Rig segments: %zu", camera->rig->segments.size());
            } else {
                ImGui::TextDisabled("No rig data.");
            }
            break;
        case editing::model::CameraKind::Preset:
            if (camera->preset) {
                ImGui::Text("Preset: %s", presetKindName(camera->preset->kind));
                ImGui::Text("FOV: %.0f", camera->preset->fov);
            } else {
                ImGui::TextDisabled("No preset data.");
            }
            break;
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
        for (auto const& key : camera->keys) {
            auto const* selectedKey = editor.selection().getAs<editing::model::SelectedKeyframe>();
            bool const selected = selectedKey && selectedKey->trackId == camera->id && selectedKey->keyframeId == key.id;
            ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(176, 128, 18, 255));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(205, 157, 32, 255));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(232, 184, 45, 255));
            if (ImGui::Selectable(("Tick " + std::to_string(key.tick)).c_str(), selected)) {
                editor.selection().select(editing::model::SelectedKeyframe{camera->id, key.id});
                EditorAction previewAction{EditorActionType::SetPreviewCamera};
                previewAction.id = camera->id;
                submit(std::move(previewAction));
                EditorAction action{EditorActionType::Seek};
                action.tick = key.tick;
                submit(std::move(action));
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
    if (auto const* selected = selection.getAs<editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        auto const* key    = camera ? findById(camera->keys, selected->keyframeId) : nullptr;
        if (!camera || !key) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return;
        }
        if (property::beginSection("Camera Keyframe")) {
        ImGui::Text("Camera: %s", camera->name.c_str());
        ImGui::BeginDisabled(camera->locked);
        int tick = key->tick;
        if (ImGui::InputInt("Tick", &tick) && ImGui::IsItemDeactivatedAfterEdit()) {
            EditorAction action{EditorActionType::MoveCameraKeyframe};
            action.id = camera->id;
            action.secondaryId = key->id;
            action.tick = tick;
            submit(std::move(action));
        }
        ImGui::Text("Position: (%.1f, %.1f, %.1f)", key->position.x, key->position.y, key->position.z);
        ImGui::Text("Rotation: yaw %.1f  pitch %.1f", key->yaw, key->pitch);
        ImGui::Text("FOV: %.1f", key->fov);
        int easing = static_cast<int>(key->easingType);
        if (ImGui::BeginCombo("Easing", easingName(key->easingType))) {
            for (int index = 0; index < 5; ++index) {
                auto value = static_cast<editing::model::EasingType>(index);
                if (ImGui::Selectable(easingName(value), index == easing)) {
                    EditorAction action{EditorActionType::SetKeyframeEasing};
                    action.id = camera->id;
                    action.secondaryId = key->id;
                    action.kind = index;
                    submit(std::move(action));
                }
            }
            ImGui::EndCombo();
        }
        if (property::actionButton("Delete Keyframe")) {
            EditorAction action{EditorActionType::DeleteCameraKeyframe};
            action.id          = camera->id;
            action.secondaryId = key->id;
            submit(std::move(action));
        }
        ImGui::EndDisabled();
        if (camera->locked) ImGui::TextDisabled("Camera is locked.");
        property::endSection();
        }
        return;
    }

    // ===== Marker =====
    if (auto const* selected = selection.getAs<editing::model::SelectedMarker>()) {
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
    ImGui::TextDisabled("Select a sequence segment, world actor, camera, keyframe or marker.");
    ImGui::Spacing();
    ImGui::Text("Replay: %s", project->worldActor.name.empty() ? "(untitled)" : project->worldActor.name.c_str());
    ImGui::Text("Duration: %s", formatTick(project->totalTicks).c_str());
    ImGui::Text("Cameras: %zu", project->cameras.size());
    ImGui::Text("Sequence segments: %zu", project->sequence.size());
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

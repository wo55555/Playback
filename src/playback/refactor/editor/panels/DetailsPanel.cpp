#include "DetailsPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

namespace playback::refactor::editor {

void DetailsPanel::draw() {
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    auto* sel = Editor::getInstance().selection().getSelection();
    if (!sel) {
        drawEmpty();
    } else {
        if (std::get_if<SelectedKeyframe>(sel)) {
            drawKeyframe();
        } else if (std::get_if<SelectedClip>(sel)) {
            drawClip();
        } else if (std::get_if<SelectedMarker>(sel)) {
            drawMarker();
        } else if (std::get_if<SelectedTrack>(sel)) {
            drawCameraTrack();
        } else if (std::get_if<SelectedTransition>(sel)) {
            drawTransition();
        } else {
            drawEmpty();
        }
    }

    ImGui::End();
}

void DetailsPanel::drawEmpty() {
    ImGui::TextWrapped("Select a track, keyframe, clip or marker to edit");

    ImGui::Separator();
    ImGui::Text("Tips:");
    ImGui::TextDisabled(ICON_PLAY " Play/pause     Space");
    ImGui::TextDisabled(ICON_MARKER " Add marker     M");
    ImGui::TextDisabled(ICON_SPLIT " Split clip      Ctrl+K");
    ImGui::TextDisabled(ICON_UNDO " Undo           Ctrl+Z");
    ImGui::TextDisabled(ICON_EXPORT " Export         Ctrl+E");
    ImGui::TextDisabled(ICON_HELP " Help            F1");
}

void DetailsPanel::drawCameraTrack() {
    ImGui::Text("Tracks");
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD)) {
        // Add track
    }

    ImGui::Separator();
    auto& state = Editor::getInstance().state();
    for (size_t i = 0; i < state.cameraTracks.size(); ++i) {
        const auto& track = state.cameraTracks[i];
        bool active = track.active;
        ImGui::Text("%s %s", active ? ICON_TRACK_ACTIVE : ICON_TRACK_OFF, track.name.c_str());
        if (active) {
            ImGui::SameLine();
            ImGui::TextDisabled("(active)");
        }
    }
}

void DetailsPanel::drawKeyframe() {
    ImGui::Text("Keyframe");
    ImGui::Separator();

    // Get the selected keyframe from state
    auto* sel = Editor::getInstance().selection().getAs<SelectedKeyframe>();
    if (!sel) return;

    // Find the keyframe in the state
    auto& state = Editor::getInstance().state();
    for (auto& track : state.cameraTracks) {
        if (track.id != sel->trackId) continue;
        for (auto& kf : track.keyframes) {
            if (kf.id != sel->keyframeId) continue;

            drawVec3Field("Position", kf.position);

            ImGui::Separator();
            drawAngleField("Yaw", kf.rotation.x);
            drawAngleField("Pitch", kf.rotation.y);

            ImGui::Separator();
            drawNumberField("FOV", kf.fov, 1.0f);

            // Easing dropdown
            ImGui::Separator();
            ImGui::Text("Easing");
            const char* easings[] = {"Linear", "Ease In", "Ease Out", "Cubic", "Custom"};
            int idx = std::clamp(kf.easing, 0, 4);
            ImGui::Combo("##easing", &idx, easings, 5);
            kf.easing = idx;

            ImGui::Separator();
            if (ImGui::Button(ICON_DELETE " Delete Keyframe")) {
                // Placeholder: delete via command
            }
            return;
        }
    }
}

void DetailsPanel::drawClip() {
    auto* sel = Editor::getInstance().selection().getAs<SelectedClip>();
    if (!sel) return;

    ImGui::Text("Clip Properties");
    ImGui::Separator();

    auto& state = Editor::getInstance().state();
    for (auto& vt : state.videoTracks) {
        for (auto& clip : vt.clips) {
            if (clip.id != sel->clipId) continue;

            char buf[256];
            strncpy_s(buf, clip.name.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Name", buf, sizeof(buf))) {
                clip.name = buf;
            }

            ImGui::Text("Source: %s", clip.replayFile.c_str());
            ImGui::Text("Track: %s @ tick %d", vt.name.c_str(), clip.trackTick);

            ImGui::Separator();
            ImGui::InputInt("In Tick", &clip.inTick, 1, 10);
            ImGui::InputInt("Out Tick", &clip.outTick, 1, 10);

            ImGui::Separator();
            drawNumberField("Speed", clip.speed, 0.1f);

            ImGui::Separator();
            ImGui::Checkbox("Muted", &clip.muted);
            ImGui::Checkbox("Locked", &clip.locked);

            ImGui::Separator();
            ImGui::Text("Camera Track: %d", clip.activeCameraTrackIdx);
            ImGui::SameLine();
            if (ImGui::SmallButton("+##camera")) {
                clip.activeCameraTrackIdx++;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##camera")) {
                clip.activeCameraTrackIdx = std::max(0, clip.activeCameraTrackIdx - 1);
            }
            return;
        }
    }
}

void DetailsPanel::drawMarker() {
    auto* sel = Editor::getInstance().selection().getAs<SelectedMarker>();
    if (!sel) return;

    ImGui::Text("Marker");
    ImGui::Separator();

    auto& state = Editor::getInstance().state();
    for (auto& m : state.markers) {
        if (m.id != sel->markerId) continue;
        char buf[256];
        strncpy_s(buf, m.label.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText("Label", buf, sizeof(buf))) {
            m.label = buf;
        }
        ImGui::Text("Tick: %d", m.tick);
        return;
    }
}

void DetailsPanel::drawTransition() {
    ImGui::Text("Transition");
    ImGui::Separator();
    ImGui::Text("Transition editing not yet implemented");
}

void DetailsPanel::drawNumberField(std::string_view label, float& v, float step) {
    ImGui::TextUnformatted(label.data());
    ImGui::SameLine();
    ImGui::PushID(label.data());
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputFloat("##val", &v, step, step * 10, "%.2f", ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopID();
}

void DetailsPanel::drawVec3Field(std::string_view label, Vec3& v) {
    ImGui::TextUnformatted(label.data());
    drawNumberField("X", v.x);
    drawNumberField("Y", v.y);
    drawNumberField("Z", v.z);
}

void DetailsPanel::drawAngleField(std::string_view label, float& deg) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x3a, 0x3a, 0x20, 0xff));
    drawNumberField(label, deg, 1.0f);
    ImGui::PopStyleColor();
}

void DetailsPanel::drawColorField(Color4& c) {
    ImGui::ColorEdit4("##color", &c.r, ImGuiColorEditFlags_NoInputs);
}

void DetailsPanel::drawDropdown(std::string_view label, std::string_view current, const std::vector<std::string>& options, int& idx) {
    ImGui::TextUnformatted(label.data());
    ImGui::SameLine();
    if (ImGui::BeginCombo("##combo", current.data())) {
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            bool selected = (i == idx);
            if (ImGui::Selectable(options[i].c_str(), selected)) {
                idx = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void DetailsPanel::drawButton(std::string_view label, std::string_view icon, std::function<void()> onClick) {
    if (ImGui::Button((std::string(icon) + " " + std::string(label)).c_str())) {
        onClick();
    }
}

} // namespace playback::refactor::editor
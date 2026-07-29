#include "DetailsPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

namespace playback::refactor::editor {

// ──────────────────────────────────────────────────────────────
//  draw() — main entry
// ──────────────────────────────────────────────────────────────

void DetailsPanel::draw() {
    auto& editor = Editor::getInstance();
    auto& sel = editor.selection();

    if (sel.isEmpty()) {
        drawEmpty();
        return;
    }

    if (sel.has<SelectedKeyframe>()) {
        drawKeyframe();
    } else if (sel.has<SelectedClip>()) {
        drawClip();
    } else {
        drawEmpty();
    }

    // Always show transition editor at bottom
    ImGui::Separator();
    drawTransitionEditor();
}

// ──────────────────────────────────────────────────────────────
//  drawEmpty()
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawEmpty() {
    ImGui::Text("No selection");
    ImGui::Separator();
    ImGui::TextDisabled("Select a clip, keyframe, or\nmarker to edit properties.");
}

// ──────────────────────────────────────────────────────────────
//  drawCameraTrack()
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawCameraTrack() {
    auto* sel = Editor::getInstance().selection().getAs<SelectedKeyframe>();
    if (!sel) return;

    ImGui::Text("Camera Track");
    ImGui::Separator();

    auto& state = Editor::getInstance().state();
    for (auto& track : state.cameraTracks) {
        if (track.id != sel->trackId) continue;

        char buf[128];
        std::strncpy(buf, track.name.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText("Name", buf, sizeof(buf))) {
            track.name = buf;
        }

        ImGui::Checkbox("Active", &track.active);
        ImGui::Checkbox("Muted", &track.muted);
        ImGui::Checkbox("Locked", &track.locked);
        ImGui::Checkbox("Visible", &track.visible);

        ImGui::Text("Keyframes: %zu", track.keyframes.size());
        break;
    }
}

// ──────────────────────────────────────────────────────────────
//  drawKeyframe()
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawKeyframe() {
    auto* sel = Editor::getInstance().selection().getAs<SelectedKeyframe>();
    if (!sel) return;

    ImGui::Text("Keyframe Properties");
    ImGui::Separator();

    // Find the keyframe
    auto& state = Editor::getInstance().state();
    for (auto& track : state.cameraTracks) {
        for (auto& kf : track.keyframes) {
            if (kf.id != sel->keyframeId) continue;

            ImGui::Text("Track: %s", track.name.c_str());
            ImGui::Text("Tick: %d", kf.tick);

            ImGui::Separator();
            drawVec3Field("Position", kf.position);
            drawAngleField("Yaw", kf.yaw);
            drawAngleField("Pitch", kf.pitch);
            drawColorField("Tint", kf.tint);

            // Easing dropdown
            ImGui::Separator();
            static const char* easingNames[] = {"Linear", "Ease In", "Ease Out", "Ease InOut"};
            int easingIdx = static_cast<int>(kf.easingType);
            easingIdx = std::clamp(easingIdx, 0, 3);
            if (ImGui::BeginCombo("Easing", easingNames[easingIdx])) {
                for (int i = 0; i < 4; ++i) {
                    if (ImGui::Selectable(easingNames[i], i == easingIdx)) {
                        kf.easingType = static_cast<EasingType>(i);
                    }
                }
                ImGui::EndCombo();
            }
            return;
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  drawClip()
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawClip() {
    auto* sel = Editor::getInstance().selection().getAs<SelectedClip>();
    if (!sel) return;

    auto& state = Editor::getInstance().state();
    for (auto& vt : state.videoTracks) {
        for (auto& clip : vt.clips) {
            if (clip.id != sel->clipId) continue;

            ImGui::Text("Clip Properties");
            ImGui::Separator();

            // Name
            char buf[256];
            std::strncpy(buf, clip.name.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Name", buf, sizeof(buf))) {
                clip.name = buf;
            }

            // Source info
            ImGui::Text("Source: %s", clip.replayFile.c_str());
            ImGui::Text("Track: %s", vt.name.c_str());

            ImGui::Separator();

            // Tick positions
            ImGui::InputInt("Track Tick", &clip.trackTick, 1, 10);
            ImGui::InputInt("In Tick", &clip.inTick, 1, 10);
            ImGui::InputInt("Out Tick", &clip.outTick, 1, 10);

            // Duration
            int duration = clip.outTick - clip.inTick;
            ImGui::Text("Duration: %d ticks (%.1f sec)", duration, duration / 20.0f);

            ImGui::Separator();

            // Speed slider
            bool speedChanged = false;
            float speed = clip.speed;
            if (ImGui::SliderFloat("Speed", &speed, 0.1f, 10.0f, "%.1fx")) {
                clip.speed = std::clamp(speed, 0.1f, 10.0f);
            }

            ImGui::Separator();

            // Camera track selector
            if (state.cameraTracks.empty()) {
                ImGui::TextDisabled("No camera tracks available");
            } else {
                int camIdx = clip.activeCameraTrackIdx;
                std::vector<std::string> camNames;
                for (const auto& ct : state.cameraTracks) {
                    camNames.push_back(ct.name);
                }
                if (camIdx >= 0 && camIdx < static_cast<int>(camNames.size())) {
                    drawDropdown("Camera Track", camNames[camIdx], camNames, camIdx);
                    clip.activeCameraTrackIdx = camIdx;
                }
            }

            ImGui::Separator();

            // Toggles
            ImGui::Checkbox("Muted", &clip.muted);
            ImGui::Checkbox("Locked", &clip.locked);

            return;
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  drawMarker() — stub
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawMarker() {
    ImGui::Text("Marker Properties");
    ImGui::Separator();
    ImGui::Text("Marker editing not yet implemented");
}

// ──────────────────────────────────────────────────────────────
//  drawTransition() — stub (kept for compatibility)
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawTransition() {
    drawTransitionEditor();
}

// ──────────────────────────────────────────────────────────────
//  drawTransitionEditor() — full transition creation UI
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawTransitionEditor() {
    ImGui::Text("Add Transition");
    ImGui::Separator();

    // Kind dropdown
    const char* kindNames[] = {"Cut", "Fade", "CrossDissolve"};
    if (ImGui::BeginCombo("Kind", kindNames[mTransitionKindIdx])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kindNames[i], i == mTransitionKindIdx)) {
                mTransitionKindIdx = i;
            }
        }
        ImGui::EndCombo();
    }

    // Duration (only for Fade and CrossDissolve)
    if (mTransitionKindIdx > 0) {
        ImGui::SliderFloat("Duration (ticks)", &mTransitionDuration, 1.0f, 100.0f, "%.0f");
    }

    // Easing
    const char* easingNames[] = {"Linear", "Ease In", "Ease Out", "Ease InOut"};
    if (ImGui::BeginCombo("Easing", easingNames[mTransitionEasingIdx])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(easingNames[i], i == mTransitionEasingIdx)) {
                mTransitionEasingIdx = i;
            }
        }
        ImGui::EndCombo();
    }

    // Create button
    if (ImGui::Button("Create Transition", ImVec2(-1, 0))) {
        // Find selected clip
        auto* sel = Editor::getInstance().selection().getAs<SelectedClip>();
        if (sel) {
            auto& state = Editor::getInstance().state();
            auto kind = mTransitionKindIdx;

            // Find the clip and check if there's a next clip on the same track
            for (auto& vt : state.videoTracks) {
                for (size_t ci = 0; ci < vt.clips.size(); ++ci) {
                    if (vt.clips[ci].id != sel->clipId) continue;
                    if (ci + 1 < vt.clips.size()) {
                        const auto& nextClip = vt.clips[ci + 1];
                        // Submit via bridge for undo/redo
                        EditorBridge::getInstance().addTransition(
                            state, sel->clipId, nextClip.id,
                            kind, static_cast<int>(mTransitionDuration));
                    }
                    break;
                }
            }
        }
    }

    // List existing transitions
    ImGui::Separator();
    ImGui::Text("Existing Transitions");

    auto& state = Editor::getInstance().state();
    if (state.transitions.empty()) {
        ImGui::TextDisabled("No transitions");
    } else {
        int deleteIdx = -1;
        for (int i = 0; i < static_cast<int>(state.transitions.size()); ++i) {
            const auto& t = state.transitions[i];
            const char* kindNames2[] = {"Cut", "Fade", "CrossDissolve"};
            int idx = static_cast<int>(t.kind);
            const char* kindName = (idx >= 0 && idx < 3) ? kindNames2[idx] : "?";

            ImGui::PushID(i);
            char label[128];
            std::snprintf(label, sizeof(label), "%s: %d ticks", kindName, t.durationTicks);
            ImGui::Text("%s", label);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_DELETE)) {
                deleteIdx = i;
            }
            ImGui::PopID();
        }
        if (deleteIdx >= 0) {
            state.transitions.erase(state.transitions.begin() + deleteIdx);
        }
    }
}

// ──────────────────────────────────────────────────────────────
//  Field editors
// ──────────────────────────────────────────────────────────────

void DetailsPanel::drawNumberField(std::string_view label, float& v, float step) {
    ImGui::DragFloat(label.data(), &v, step, 0.0f, 0.0f, "%.2f");
}

void DetailsPanel::drawVec3Field(std::string_view label, Vec3& v) {
    ImGui::DragFloat3(label.data(), &v.x, 0.1f, 0.0f, 0.0f, "%.2f");
}

void DetailsPanel::drawAngleField(std::string_view label, float& deg) {
    float f = deg;
    if (ImGui::DragFloat(label.data(), &f, 1.0f, -180.0f, 180.0f, "%.1f°")) {
        deg = f;
    }
}

void DetailsPanel::drawColorField(Color4& c) {
    ImGui::ColorEdit4("Color", &c.r, ImGuiColorEditFlags_NoInputs);
}

void DetailsPanel::drawDropdown(std::string_view label, std::string_view current,
                                const std::vector<std::string>& options, int& idx)
{
    if (ImGui::BeginCombo(label.data(), current.data())) {
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (ImGui::Selectable(options[i].c_str(), i == idx)) {
                idx = i;
            }
        }
        ImGui::EndCombo();
    }
}

void DetailsPanel::drawButton(std::string_view label, std::string_view icon,
                              std::function<void()> onClick)
{
    if (ImGui::Button(std::string(icon).append(" ").append(label).c_str())) {
        onClick();
    }
}

} // namespace playback::refactor::editor
#include "CurveEditorPanel.h"

#include "imgui.h"

namespace playback::editor::ui {

CurveEditorPanel::CurveEditorPanel() {
    // Default linear curve
    mDefaultCurve.name = "Default";
    mDefaultCurve.points = {
        {0.0f, 0.0f, {0,0}, {0,0}},
        {1.0f, 1.0f, {0,0}, {0,0}}
    };
    mEditor.setCurve(mDefaultCurve);
}

void CurveEditorPanel::draw() {
    if (!mOpen) return;

    // Preset dropdown
    if (ImGui::BeginCombo("Preset", "Custom")) {
        if (ImGui::Selectable("Linear")) {
            BezierCurve linear;
            linear.name = "Linear";
            linear.points = {
                {0.0f, 0.0f, {0,0}, {0,0}},
                {1.0f, 1.0f, {0,0}, {0,0}}
            };
            mEditor.setCurve(linear);
        }
        if (ImGui::Selectable("Ease In")) {
            BezierCurve easeIn;
            easeIn.name = "Ease In";
            easeIn.points = {
                {0.0f, 0.0f, {0,0}, {0,0}},
                {0.4f, 0.2f, {0,0}, {0,0}},
                {1.0f, 1.0f, {0,0}, {0,0}}
            };
            mEditor.setCurve(easeIn);
        }
        if (ImGui::Selectable("Ease Out")) {
            BezierCurve easeOut;
            easeOut.name = "Ease Out";
            easeOut.points = {
                {0.0f, 0.0f, {0,0}, {0,0}},
                {0.6f, 0.8f, {0,0}, {0,0}},
                {1.0f, 1.0f, {0,0}, {0,0}}
            };
            mEditor.setCurve(easeOut);
        }
        if (ImGui::Selectable("Ease InOut")) {
            BezierCurve easeInOut;
            easeInOut.name = "Ease InOut";
            easeInOut.points = {
                {0.0f, 0.0f, {0,0}, {0,0}},
                {0.3f, 0.1f, {0,0}, {0,0}},
                {0.7f, 0.9f, {0,0}, {0,0}},
                {1.0f, 1.0f, {0,0}, {0,0}}
            };
            mEditor.setCurve(easeInOut);
        }
        ImGui::EndCombo();
    }

    // Curve preview area
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float curveH = std::min(avail.y - 80.0f, 200.0f);
    Rect curveArea;
    curveArea.min = ImGui::GetCursorScreenPos();
    curveArea.max = ImVec2(curveArea.min.x + avail.x, curveArea.min.y + curveH);

    // Draw the curve
    mEditor.draw(ImGui::GetWindowDrawList(), curveArea);
    mEditor.handleInput(ImGui::GetIO(), curveArea);

    ImGui::SetCursorScreenPos(ImVec2(curveArea.min.x, curveArea.max.y));

    // Sample point preview
    ImGui::Separator();
    ImGui::Text("Sample at 0.5: %.3f", mEditor.sampleAt(0.5f));
    ImGui::Text("Sample at 0.25: %.3f", mEditor.sampleAt(0.25f));
    ImGui::Text("Sample at 0.75: %.3f", mEditor.sampleAt(0.75f));

}

} // namespace playback::editor::ui

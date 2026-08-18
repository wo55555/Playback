#include "CurveEditorPanel.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

CurveEditorPanel::CurveEditorPanel() {
    mDefaultCurve.name   = "Default";
    mDefaultCurve.points = {
        {0.0f, 0.0f, {0, 0}, {0, 0}},
        {1.0f, 1.0f, {0, 0}, {0, 0}}
    };
    mEditor.setCurve(mDefaultCurve);
}

void CurveEditorPanel::draw() {
    if (!mOpen) return;

    std::string const presetLabel = "playback.refactorEditor.curve.preset"_tr() + "###curve-preset";
    if (ImGui::BeginCombo(presetLabel.c_str(), "playback.refactorEditor.curve.custom"_tr().c_str())) {
        if (ImGui::Selectable("playback.refactorEditor.curve.linear"_tr().c_str())) {
            BezierCurve linear;
            linear.name   = "Linear";
            linear.points = {
                {0.0f, 0.0f, {0, 0}, {0, 0}},
                {1.0f, 1.0f, {0, 0}, {0, 0}}
            };
            mEditor.setCurve(linear);
        }
        if (ImGui::Selectable("playback.refactorEditor.curve.easeIn"_tr().c_str())) {
            BezierCurve easeIn;
            easeIn.name   = "Ease In";
            easeIn.points = {
                {0.0f, 0.0f, {0, 0}, {0, 0}},
                {0.4f, 0.2f, {0, 0}, {0, 0}},
                {1.0f, 1.0f, {0, 0}, {0, 0}}
            };
            mEditor.setCurve(easeIn);
        }
        if (ImGui::Selectable("playback.refactorEditor.curve.easeOut"_tr().c_str())) {
            BezierCurve easeOut;
            easeOut.name   = "Ease Out";
            easeOut.points = {
                {0.0f, 0.0f, {0, 0}, {0, 0}},
                {0.6f, 0.8f, {0, 0}, {0, 0}},
                {1.0f, 1.0f, {0, 0}, {0, 0}}
            };
            mEditor.setCurve(easeOut);
        }
        if (ImGui::Selectable("playback.refactorEditor.curve.easeInOut"_tr().c_str())) {
            BezierCurve easeInOut;
            easeInOut.name   = "Ease InOut";
            easeInOut.points = {
                {0.0f, 0.0f, {0, 0}, {0, 0}},
                {0.3f, 0.1f, {0, 0}, {0, 0}},
                {0.7f, 0.9f, {0, 0}, {0, 0}},
                {1.0f, 1.0f, {0, 0}, {0, 0}}
            };
            mEditor.setCurve(easeInOut);
        }
        ImGui::EndCombo();
    }

    ImVec2 avail  = ImGui::GetContentRegionAvail();
    float  curveH = std::min(avail.y - 80.0f, 200.0f);
    Rect   curveArea;
    curveArea.min = ImGui::GetCursorScreenPos();
    curveArea.max = ImVec2(curveArea.min.x + avail.x, curveArea.min.y + curveH);

    mEditor.draw(ImGui::GetWindowDrawList(), curveArea);

    ImGui::SetCursorScreenPos(ImVec2(curveArea.min.x, curveArea.max.y));

    ImGui::Separator();
    ImGui::TextUnformatted("playback.refactorEditor.curve.sample"_tr(0.5f, mEditor.sampleAt(0.5f)).c_str());
    ImGui::TextUnformatted("playback.refactorEditor.curve.sample"_tr(0.25f, mEditor.sampleAt(0.25f)).c_str());
    ImGui::TextUnformatted("playback.refactorEditor.curve.sample"_tr(0.75f, mEditor.sampleAt(0.75f)).c_str());
}

} // namespace playback::editor::ui

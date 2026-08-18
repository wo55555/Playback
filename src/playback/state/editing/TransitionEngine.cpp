#include "TransitionEngine.h"

#include <algorithm>
#include <cmath>

namespace playback::state::editing {

using model::Clip;
using model::EditorStateExt;
using model::TrackKind;
using model::Transition;
using model::TransitionKind;

RenderPlan TransitionEngine::planAt(int timelineTick, const EditorStateExt& editor) {
    RenderPlan plan;

    // 1) Find all active clips (by timeline tick)
    std::vector<std::pair<int, const Clip*>> active;
    for (const auto& t : editor.videoTracks) {
        if (!t.visible || t.kind != TrackKind::Video) continue;
        for (const auto& c : t.clips) {
            if (c.muted) continue;
            int clipEnd = c.trackTick + (c.outTick - c.inTick);
            if (timelineTick >= c.trackTick && timelineTick < clipEnd) {
                active.push_back({timelineTick - c.trackTick, &c});
            }
        }
    }

    if (active.empty()) return plan;
    if (active.size() == 1) {
        plan.primaryClipId = active[0].second->id;
        return plan;
    }

    // 2) Find transition between the two overlapping clips
    const auto& a = *active[0].second;
    const auto& b = *active[1].second;

    // Find transition
    const Transition* trans = nullptr;
    for (const auto& t : editor.transitions) {
        if (t.fromClipId == a.id && t.toClipId == b.id) {
            trans = &t;
            break;
        }
    }

    if (!trans) {
        // No transition: just use the primary clip
        plan.primaryClipId = a.id;
        return plan;
    }

    // 3) In transition range, blend
    int transStart = b.trackTick - trans->durationTicks; // transition starts before b
    int transEnd   = b.trackTick;

    if (timelineTick < transStart || timelineTick > transEnd) {
        plan.primaryClipId = a.id;
        return plan;
    }

    int tickInTrans      = timelineTick - transStart;
    plan.primaryClipId   = a.id;
    plan.secondaryClipId = b.id;
    plan.kind            = trans->kind;

    switch (trans->kind) {
    case TransitionKind::Cut:
        plan.blendAlpha = (timelineTick < transEnd) ? 0.0f : 1.0f;
        break;

    case TransitionKind::Fade: {
        float t         = static_cast<float>(tickInTrans) / static_cast<float>(trans->durationTicks);
        plan.blendAlpha = easingValue(trans->easing, t);
        break;
    }

    case TransitionKind::CrossDissolve: {
        float t         = static_cast<float>(tickInTrans) / static_cast<float>(trans->durationTicks);
        float e         = easingValue(trans->easing, t);
        plan.blendAlpha = e; // 0=full a, 1=full b
        break;
    }
    }

    return plan;
}

float TransitionEngine::easingValue(int easing, float t) {
    t = std::clamp(t, 0.0f, 1.0f);

    switch (easing) {
    case 0:
        return t; // Linear
    case 1:
        return t * t; // Ease In
    case 2:
        return 1.0f - (1.0f - t) * (1.0f - t); // Ease Out
    case 3:
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f; // Ease InOut
    default:
        return t;
    }
}

} // namespace playback::state::editing

#include "Animator.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace playback::editor::ui {

ImU32 lerpColor(ImU32 from, ImU32 to, float amount) {
    amount             = std::clamp(amount, 0.0f, 1.0f);
    auto const channel = [from, to, amount](int shift) {
        auto const start = static_cast<float>((from >> shift) & 0xffu);
        auto const end   = static_cast<float>((to >> shift) & 0xffu);
        return static_cast<int>(std::lround(start + (end - start) * amount));
    };
    return IM_COL32(channel(0), channel(8), channel(16), channel(24));
}

float advanceAnimation(float current, float target, float deltaTime) {
    float const step = std::min(1.0f, std::max(0.0f, deltaTime) / kAnimationDuration);
    if (step >= 1.0f) return target;
    current += target > current ? step : -step;
    current  = std::clamp(current, 0.0f, 1.0f);
    return std::abs(target - current) < 0.001f ? target : current;
}

float Animator::animate(uint64_t key, float target) {
    auto& entry = mValues[key];
    if (entry.frame != mFrame) {
        entry.frame = mFrame;
        entry.value = advanceAnimation(entry.value, target, ImGui::GetIO().DeltaTime);
    }
    return entry.value;
}

void Animator::beginFrame() {
    ++mFrame;
    for (auto entry = mValues.begin(); entry != mValues.end();) {
        entry = entry->second.frame + 1 < mFrame ? mValues.erase(entry) : std::next(entry);
    }
}

void Animator::clear() {
    mValues.clear();
    mFrame = 1;
}

} // namespace playback::editor::ui

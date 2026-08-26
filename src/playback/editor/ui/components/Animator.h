#pragma once

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace playback::editor::ui {

// Strictly linear progress over kAnimationDuration; exponential catch-up leaves visible trailing.
constexpr float kAnimationDuration = 0.2f;

[[nodiscard]] ImU32 lerpColor(ImU32 from, ImU32 to, float amount);

[[nodiscard]] float advanceAnimation(float current, float target, float deltaTime);

// splitmix64 finalizer; raw FNV output clusters in map implementations that hash uint64 as identity.
[[nodiscard]] constexpr uint64_t mixHash(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

[[nodiscard]] constexpr uint64_t hashText(std::string_view text, uint64_t seed = 0xcbf29ce484222325ull) {
    for (char character : text) {
        seed ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
        seed *= 0x100000001b3ull;
    }
    return seed;
}

// Mixing between the fields keeps ("a", "bc") and ("ab", "c") distinct.
[[nodiscard]] constexpr uint64_t animationKey(std::string_view scope, std::string_view id) {
    return mixHash(hashText(id, mixHash(hashText(scope))));
}

class Animator {
public:
    float animate(uint64_t key, float target);

    // Scope and id stay separate so call sites never concatenate into a per-frame temporary.
    float animate(std::string_view scope, std::string_view id, float target) {
        return animate(animationKey(scope, id), target);
    }

    // Drops entries untouched since the previous beginFrame to keep the map bounded.
    void beginFrame();
    void clear();

private:
    struct Entry {
        float    value{};
        uint64_t frame{};
    };

    // Keys arrive pre-mixed, so re-hashing them in the map would only add work.
    struct IdentityHash {
        [[nodiscard]] size_t operator()(uint64_t key) const noexcept { return static_cast<size_t>(key); }
    };

    std::unordered_map<uint64_t, Entry, IdentityHash> mValues;
    uint64_t                                          mFrame{};
};

} // namespace playback::editor::ui

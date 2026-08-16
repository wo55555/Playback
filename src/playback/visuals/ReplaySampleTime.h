#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace playback::visuals {

struct ReplaySampleTime {
    int64_t numerator{};
    int64_t denominator{1};

    [[nodiscard]] bool isValid() const noexcept { return numerator >= 0 && denominator > 0; }

    [[nodiscard]] int64_t floorTick() const noexcept { return isValid() ? numerator / denominator : 0; }

    [[nodiscard]] int64_t requiredAppliedTick() const noexcept {
        if (!isValid()) return 0;
        auto const whole = numerator / denominator;
        return numerator % denominator == 0 ? whole : whole + 1;
    }

    [[nodiscard]] float partialTick() const noexcept {
        if (!isValid()) return 0.0f;
        auto const remainder = numerator % denominator;
        auto result = static_cast<float>(static_cast<long double>(remainder) / static_cast<long double>(denominator));
        if (remainder != 0 && result >= 1.0f) result = std::nextafter(1.0f, 0.0f);
        return result;
    }

    [[nodiscard]] long double value() const noexcept {
        return isValid() ? static_cast<long double>(numerator) / static_cast<long double>(denominator) : 0.0L;
    }

    [[nodiscard]] static std::optional<ReplaySampleTime>
    fromRational(int64_t tickNumerator, int64_t tickDenominator) noexcept {
        ReplaySampleTime result{tickNumerator, tickDenominator};
        return result.isValid() ? std::optional<ReplaySampleTime>{result} : std::nullopt;
    }

    [[nodiscard]] static std::optional<ReplaySampleTime> fromPreview(int64_t wholeTick, float partial) noexcept {
        constexpr int64_t PreviewDenominator = 1'000'000;
        if (wholeTick < 0 || !std::isfinite(partial)) return std::nullopt;

        partial         = std::clamp(partial, 0.0f, std::nextafter(1.0f, 0.0f));
        auto const unit = std::min<int64_t>(
            PreviewDenominator - 1,
            static_cast<int64_t>(std::llround(static_cast<double>(partial) * PreviewDenominator))
        );
        if (wholeTick > (std::numeric_limits<int64_t>::max() - unit) / PreviewDenominator) return std::nullopt;
        return ReplaySampleTime{wholeTick * PreviewDenominator + unit, PreviewDenominator};
    }
};

} // namespace playback::visuals

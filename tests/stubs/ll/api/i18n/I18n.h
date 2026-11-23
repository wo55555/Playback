#pragma once

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>

namespace ll::i18n_literals {

struct TrProxy {
    std::string key;

    template <typename... Args>
    [[nodiscard]] std::string operator()(Args&&... args) const {
        std::ostringstream oss;
        oss << key;
        ((oss << ':' << std::forward<Args>(args)), ...);
        return oss.str();
    }

    [[nodiscard]] operator std::string() const { return key; }
};

[[nodiscard]] inline TrProxy operator""_tr(char const* str, std::size_t len) { return TrProxy{std::string(str, len)}; }

} // namespace ll::i18n_literals

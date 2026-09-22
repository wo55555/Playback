#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace playback::editor::graphics {

struct GraphicsSwitchSubmitTicket {
    uint64_t transition{};
    uint32_t ordinal{};
};

[[nodiscard]] bool                       openGraphicsSwitchTrace(std::filesystem::path const& path) noexcept;
void                                     closeGraphicsSwitchTrace() noexcept;
[[nodiscard]] uint64_t                   beginGraphicsSwitchTrace(int previous, int requested) noexcept;
[[nodiscard]] uint64_t                   currentGraphicsSwitchTrace() noexcept;
[[nodiscard]] GraphicsSwitchSubmitTicket claimGraphicsSwitchSubmitTrace() noexcept;
void                                     rearmGraphicsSwitchSubmitTrace(uint64_t transition) noexcept;
void recordGraphicsSwitchTrace(uint64_t transition, std::string_view event, std::string_view detail = {}) noexcept;

} // namespace playback::editor::graphics
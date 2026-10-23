#pragma once

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/world/item/ItemStackBase.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace playback::utils {

// MapItem::getMapId returns ActorUniqueID through a hidden pointer in the game but the SDK declares it as an aggregate
// returned in RAX, so calling it shifts every argument. Read the tag directly instead.
inline std::optional<int64_t> readMapUuid(ItemStackBase const& item) {
    if (item.isNull()) return std::nullopt;

    auto const* tag = item.mUserData.get();
    if (!tag) return std::nullopt;

    auto const found = tag->mTags.find(std::string_view{"map_uuid"});
    if (found == tag->mTags.end()) return std::nullopt;

    auto const* value = std::get_if<Int64Tag>(&found->second.mTagStorage);
    if (!value || value->data == -1) return std::nullopt;
    return value->data;
}

} // namespace playback::utils

#include "RayTracing.h"

#include "mc/common/Globals.h"
#include "mc/deps/minecraft_renderer/framebuilder/FrameBuilder.h"

namespace playback::exporting {

bool rayTracingActive() noexcept try {
    auto* const builder = renderDragonFrameBuilder();
    return builder && builder->initialized() && builder->isRayTracingEnabled();
} catch (...) {
    return false;
}

} // namespace playback::exporting

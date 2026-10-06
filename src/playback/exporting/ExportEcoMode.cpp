#include "playback/exporting/ExportEcoMode.h"

#include "playback/Playback.h"

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"

#include <atomic>

namespace playback::exporting {

namespace {

std::atomic_bool gEcoModeSuspended{false};

} // namespace

// After 15 minutes without input, Eco Mode drops the main loop to ~10 FPS and starves the export's GPU work.
void applyExportEcoMode(bool active) noexcept {
    auto client = ll::service::getClientInstance();
    if (!client) return;

    auto& options = client->getOptions();
    auto& logger  = Playback::getInstance().getSelf().getLogger();
    if (active) {
        if (!options.getEcoMode()) return;
        options.setEcoMode(false);
        gEcoModeSuspended.store(true, std::memory_order_release);
        logger.debug("Eco Mode suspended for the export");
        return;
    }

    if (!gEcoModeSuspended.exchange(false, std::memory_order_acq_rel)) return;
    options.setEcoMode(true);
    logger.debug("Eco Mode restored after the export");
}

} // namespace playback::exporting

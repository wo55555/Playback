#include "playback/exporting/ExportEcoMode.h"

#include "playback/Playback.h"

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/settings/DataProvider.h"
#include "mc/client/settings/IBooleanDataProvider.h"
#include "mc/options/option_types/OptionID.h"

#include <atomic>
#include <memory>

namespace playback::exporting {

namespace {

std::atomic_bool gEcoModeSuspended{false};

// This SDK's IOptionRegistry has no Eco Mode accessor, so the option goes through the settings data provider.
std::unique_ptr<Settings::IBooleanDataProvider> openEcoModeOption(ClientInstance& client) {
    auto provider = Settings::DataProvider::createBooleanDataProvider(OptionID::EcoMode, client.getOptions());
    if (!provider || !*provider || !(*provider)->canModify()) return nullptr;
    return std::move(*provider);
}

// Returns false when the value did not take.
bool setEcoMode(Settings::IBooleanDataProvider& option, bool value) {
    option.setValue(value);
    return option.getValue() == value;
}

} // namespace

// After 15 minutes without input, Eco Mode drops the main loop to ~10 FPS and starves the export's GPU work.
void applyExportEcoMode(bool active) noexcept {
    auto client = ll::service::getClientInstance();
    if (!client) return;

    auto  option = openEcoModeOption(*client);
    auto& logger = Playback::getInstance().getSelf().getLogger();
    if (!option) {
        if (active) logger.warn("Eco Mode is not available; a long export may slow down after 15 minutes idle");
        return;
    }

    if (active) {
        if (!option->getValue()) return;
        if (!setEcoMode(*option, false)) {
            logger.warn("Eco Mode could not be switched off for the export");
            return;
        }
        gEcoModeSuspended.store(true, std::memory_order_release);
        logger.debug("Eco Mode suspended for the export");
        return;
    }

    if (!gEcoModeSuspended.exchange(false, std::memory_order_acq_rel)) return;
    if (setEcoMode(*option, true)) logger.debug("Eco Mode restored after the export");
    else logger.warn("Eco Mode could not be restored after the export");
}

// The game has no setting for Eco Mode, so a crash during an export would otherwise leave it off for good.
void restoreEcoModeAfterCrash() noexcept {
    static std::atomic_bool checked{false};
    if (checked.load(std::memory_order_acquire)) return;

    auto client = ll::service::getClientInstance();
    if (!client) return;
    checked.store(true, std::memory_order_release);

    auto option = openEcoModeOption(*client);
    if (!option || option->getValue()) return;
    if (!setEcoMode(*option, true)) return;
    Playback::getInstance().getSelf().getLogger().info("Eco Mode was off at startup; restored it to the game default");
}

} // namespace playback::exporting

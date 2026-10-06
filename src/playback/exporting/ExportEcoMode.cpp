#include "playback/exporting/ExportEcoMode.h"

#include "playback/Playback.h"
#include "playback/exporting/EcoModeJournal.h"

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/settings/DataProvider.h"
#include "mc/client/settings/IBooleanDataProvider.h"
#include "mc/options/option_types/OptionID.h"

#if __has_include("mc/client/options/IOptionRegistry.h")
#include "mc/client/options/IOptionRegistry.h"
#endif
#if __has_include("mc/client/options/IOptions.h")
#include "mc/client/options/IOptions.h"
#endif

#include <atomic>
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace playback::exporting {

namespace {

using Options = std::remove_cvref_t<decltype(std::declval<ClientInstance&>().getOptions())>;

template <class T>
concept HasEcoModeAccessor = requires(T& options, bool value) {
    options.setEcoMode(value);
    { options.getEcoMode() } -> std::convertible_to<bool>;
};

template <class Id>
concept HasEcoModeId = requires { Id::EcoMode; };

std::atomic_bool gEcoModeSuspended{false};

// The SDKs reach Eco Mode differently: a direct accessor on the options object, or only the settings data provider.
template <class O>
class EcoModeOption {
public:
    explicit EcoModeOption(O& options) : mOptions(options) {
        if constexpr (!HasEcoModeAccessor<O>) openProvider(options);
    }

    bool available() const {
        if constexpr (HasEcoModeAccessor<O>) return true;
        else return mProvider != nullptr;
    }

    bool get() {
        if constexpr (HasEcoModeAccessor<O>) return mOptions.getEcoMode();
        else return mProvider->getValue();
    }

    // Returns false when the value did not take.
    bool set(bool value) {
        if constexpr (HasEcoModeAccessor<O>) {
            mOptions.setEcoMode(value);
            return mOptions.getEcoMode() == value;
        } else {
            mProvider->setValue(value);
            return mProvider->getValue() == value;
        }
    }

private:
    template <class Id = OptionID>
    void openProvider(O& options) {
        if constexpr (HasEcoModeId<Id>) {
            auto provider = Settings::DataProvider::createBooleanDataProvider(Id::EcoMode, options);
            if (provider && *provider && (*provider)->canModify()) mProvider = std::move(*provider);
        }
    }

    O&                                              mOptions;
    std::unique_ptr<Settings::IBooleanDataProvider> mProvider;
};

} // namespace

// After 15 minutes without input, Eco Mode drops the main loop to ~10 FPS and starves the export's GPU work.
void applyExportEcoMode(bool active) noexcept {
    if (!active && !gEcoModeSuspended.load(std::memory_order_acquire)) return;

    auto client = ll::service::getClientInstance();
    if (!client) return;

    EcoModeOption<Options> option{client->getOptions()};
    auto&                  logger = Playback::getInstance().getSelf().getLogger();
    if (!option.available()) {
        if (active) logger.warn("Eco Mode is not available; a long export may slow down after 15 minutes idle");
        return;
    }

    if (active) {
        if (!option.get()) return;
        if (!writeEcoModeJournal()) {
            logger.warn("Eco Mode was left on because its restore marker could not be written");
            return;
        }
        if (!option.set(false)) {
            clearEcoModeJournal();
            logger.warn("Eco Mode could not be switched off for the export");
            return;
        }
        gEcoModeSuspended.store(true, std::memory_order_release);
        logger.debug("Eco Mode suspended for the export");
        return;
    }

    if (!gEcoModeSuspended.exchange(false, std::memory_order_acq_rel)) return;
    if (!option.set(true)) {
        logger.warn("Eco Mode could not be restored after the export; it will be retried on the next launch");
        return;
    }
    clearEcoModeJournal();
    logger.debug("Eco Mode restored after the export");
}

void recoverEcoModeAfterInterruptedExport() noexcept {
    static bool checked = false;
    if (checked) return;

    auto client = ll::service::getClientInstance();
    if (!client) return;
    checked = true;

    auto const interrupted = findInterruptedEcoModeJournals();
    if (interrupted.empty()) return;

    EcoModeOption<Options> option{client->getOptions()};
    auto&                  logger = Playback::getInstance().getSelf().getLogger();
    if (!option.available()) {
        logger.warn("An interrupted export may have left Eco Mode off, and it cannot be restored on this version");
        return;
    }

    bool const wasOff = !option.get();
    if (wasOff && !option.set(true)) {
        logger.warn("Eco Mode could not be restored after an interrupted export; retrying on the next launch");
        return;
    }
    discardEcoModeJournals(interrupted);
    if (wasOff) logger.info("Eco Mode restored after an interrupted export");
}

} // namespace playback::exporting

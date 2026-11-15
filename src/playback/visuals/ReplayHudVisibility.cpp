#include "ReplayHudVisibility.h"

#include "playback/Playback.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/replay/ReplaySession.h"
#include "playback/runtime/GameAddress.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/DirtyFlag.h"
#include "mc/client/gui/screens/controllers/HudScreenController.h"
#include "mc/deps/core/string/StringHash.h"

#include <atomic>
#include <memory>
#include <utility>

namespace playback::visuals {

namespace {

constexpr uint32_t bit(HudElement element) { return 1u << static_cast<uint32_t>(element); }

// A replay starts with only the followed player's hotbar shown.
constexpr uint32_t DefaultShownMask = bit(HudElement::Hotbar);
constexpr uint32_t AllElementsMask  = (1u << HudElementCount) - 1;
constexpr uint32_t WholeHudBit      = 1u << HudElementCount;
constexpr uint32_t UnknownMask      = ~0u;

std::atomic<uint32_t> gShownMask{DefaultShownMask};
std::atomic_bool      gHiddenForExport{false};
uint32_t              gLastHiddenMask = UnknownMask;

// The editor, its preview and the export all run inside the replay session.
[[nodiscard]] bool replayContextActive() {
    return Playback::getInstance().isReplayMode() || replay::ReplaySession::getInstance().isActive()
        || exporting::isExportActivityActive();
}

[[nodiscard]] bool wholeHudHiddenNow() {
    return gHiddenForExport.load(std::memory_order_relaxed) && exporting::isExportActivityActive();
}

[[nodiscard]] uint32_t hiddenMaskNow() {
    uint32_t mask = 0;
    if (replayContextActive()) mask = ~gShownMask.load(std::memory_order_relaxed) & AllElementsMask;
    if (wholeHudHiddenNow()) mask |= WholeHudBit;
    return mask;
}

// The pack negates these, so a binding that never got registered leaves the vanilla HUD untouched.
void registerBindings(HudScreenController& ctrl) {
    ctrl.bindBool(StringHash("#playback_hud_hidden"), [] { return wholeHudHiddenNow(); }, [] { return true; });
    ctrl.bindBool(
        StringHash("#playback_hud_chat_hidden"),
        [] { return isHudElementHiddenNow(HudElement::Chat); },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_hud_boss_bar_hidden"),
        [] { return isHudElementHiddenNow(HudElement::BossBar); },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_hud_title_hidden"),
        [] { return isHudElementHiddenNow(HudElement::Title); },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_hud_scoreboard_hidden"),
        [] { return isHudElementHiddenNow(HudElement::Scoreboard); },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_hud_action_bar_hidden"),
        [] { return isHudElementHiddenNow(HudElement::ActionBar); },
        [] { return true; }
    );
}

LL_TYPE_INSTANCE_HOOK(
    HudVisibilityBindHook,
    ll::memory::HookPriority::Normal,
    HudScreenController,
    &HudScreenController::$ctor,
    void*,
    std::shared_ptr<ClientInstanceScreenModel> model
) {
    auto* result = origin(std::move(model));
    registerBindings(*this);
    gLastHiddenMask = UnknownMask;
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    HudVisibilityTickHook,
    ll::memory::HookPriority::Normal,
    HudScreenController,
    runtime::gameAddress(runtime::GameFunction::HudScreenTick),
    ::ui::DirtyFlag
) {
    auto result = origin();
    // HUD bindings only re-evaluate on a dirty tick.
    auto const mask = hiddenMaskNow();
    if (mask != gLastHiddenMask) {
        gLastHiddenMask = mask;
        result =
            static_cast<::ui::DirtyFlag>(static_cast<uint>(result) | static_cast<uint>(::ui::DirtyFlag::UpdateBinds));
    }
    return result;
}

} // namespace

void hookReplayHudVisibility(bool enable) {
    static bool bindHooked = false;
    static bool tickHooked = false;
    if (enable) {
        if (!bindHooked) bindHooked = HudVisibilityBindHook::hook() == 0;
        if (!tickHooked) tickHooked = HudVisibilityTickHook::hook() == 0;
        if (!bindHooked || !tickHooked) {
            Playback::getInstance().getSelf().getLogger().warn("Replay HUD element toggles are unavailable");
        }
        return;
    }
    if (tickHooked) tickHooked = !HudVisibilityTickHook::unhook();
    if (bindHooked) bindHooked = !HudVisibilityBindHook::unhook();
    gHiddenForExport.store(false, std::memory_order_relaxed);
    gLastHiddenMask = UnknownMask;
}

void setHudElementShown(HudElement element, bool shown) {
    if (shown) gShownMask.fetch_or(bit(element), std::memory_order_relaxed);
    else gShownMask.fetch_and(~bit(element), std::memory_order_relaxed);
}

bool isHudElementShown(HudElement element) { return (gShownMask.load(std::memory_order_relaxed) & bit(element)) != 0; }

void setHudHiddenForExport(bool hidden) { gHiddenForExport.store(hidden, std::memory_order_relaxed); }

bool isHudElementHiddenNow(HudElement element) { return (hiddenMaskNow() & bit(element)) != 0; }

} // namespace playback::visuals

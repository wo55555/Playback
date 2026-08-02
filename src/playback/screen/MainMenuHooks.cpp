#include "MainMenuHooks.h"

#include "playback/functions/replay/ReplaySession.h"
#include "playback/refactor/replay-browser/ReplayBrowserWindow.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/controls/UIPropertyBag.h"
#include "mc/client/gui/screens/controllers/MainMenuScreenController.h"
#include "mc/client/gui/screens/controllers/MinecraftScreenController.h"
#include "mc/client/gui/screens/controllers/StartMenuScreenController.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace playback::screen {

namespace {

bool gOpenRequested{};
std::unordered_set<MinecraftScreenController*> gEventControllers;

constexpr std::string_view kButtonOpenReplays = "button.playback_open_replays";
constexpr auto kConsumeAndRefreshFocus = static_cast<::ui::ViewRequest>(
    static_cast<uint>(::ui::ViewRequest::ConsumeEvent) | static_cast<uint>(::ui::ViewRequest::DelayedFocusRefresh)
);

void ensureEvents(MinecraftScreenController& ctrl) {
    if (!gEventControllers.insert(&ctrl).second) return;
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonOpenReplays)), [](UIPropertyBag*) {
        if (!refactor::replay_browser::ReplayBrowserWindow::getInstance().isOpen()) gOpenRequested = true;
        return kConsumeAndRefreshFocus;
    });
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    MainMenuOpenHook,
    ll::memory::HookPriority::Normal,
    MainMenuScreenController,
    &MainMenuScreenController::$onOpen,
    void
) {
    origin();
    ensureEvents(*this);
}

LL_TYPE_INSTANCE_HOOK(
    StartMenuEventsHook,
    ll::memory::HookPriority::Normal,
    StartMenuScreenController,
    &StartMenuScreenController::_registerEventHandlers,
    void
) {
    origin();
    ensureEvents(*this);
}

LL_TYPE_INSTANCE_HOOK(
    StartMenuTickHook,
    ll::memory::HookPriority::Normal,
    StartMenuScreenController,
    &StartMenuScreenController::$tick,
    ::ui::DirtyFlag
) {
    functions::ReplaySession::getInstance().setMinecraftScreenModel(mMinecraftScreenModel);
    auto result = origin();
    if (gOpenRequested) {
        gOpenRequested = false;
        refactor::replay_browser::ReplayBrowserWindow::getInstance().open();
    }
    return result;
}

void hookMainMenu(bool enable) {
    static bool hooked = false;
    if (hooked == enable) return;
    if (enable) {
        MainMenuOpenHook::hook();
        StartMenuEventsHook::hook();
        StartMenuTickHook::hook();
    } else {
        StartMenuTickHook::unhook();
        StartMenuEventsHook::unhook();
        MainMenuOpenHook::unhook();
        gOpenRequested = false;
        gEventControllers.clear();
    }
    hooked = enable;
}

} // namespace playback::screen

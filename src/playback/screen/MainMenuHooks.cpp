#include "MainMenuHooks.h"

#include "playback/editor/ReplayUI.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/controls/UIPropertyBag.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/controllers/MainMenuScreenController.h"
#include "mc/client/gui/screens/controllers/MinecraftScreenController.h"
#include "mc/client/gui/screens/controllers/StartMenuScreenController.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace playback::screen {

namespace {

std::unordered_set<MinecraftScreenController*> gEventControllers;

constexpr std::string_view kButtonOpenReplays      = "button.playback_open_replays";
constexpr auto             kConsumeAndRefreshFocus = static_cast<::ui::ViewRequest>(
    static_cast<uint>(::ui::ViewRequest::ConsumeEvent) | static_cast<uint>(::ui::ViewRequest::DelayedFocusRefresh)
);

void ensureEvents(MinecraftScreenController& ctrl) {
    if (!gEventControllers.insert(&ctrl).second) return;
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonOpenReplays)), [](UIPropertyBag*) {
        editor::submitEditorAction({editor::EditorActionType::OpenReplayBrowser});
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
    setSuspendInput(editor::isReplayBrowserVisible());
    return origin();
}

LL_TYPE_INSTANCE_HOOK(
    ScreenViewPointerLocationHook,
    ll::memory::HookPriority::High,
    ScreenView,
    &ScreenView::_handlePointerLocation,
    void,
    ::glm::vec2 const& position,
    ::FocusImpact      focusImpact,
    bool               forceHandleWhenMotionless,
    bool               isRightStickScrolling
) {
    if (editor::isReplayBrowserVisible()) return;
    origin(position, focusImpact, forceHandleWhenMotionless, isRightStickScrolling);
}

void hookMainMenu(bool enable) {
    static bool hooked = false;
    if (hooked == enable) return;
    if (enable) {
        MainMenuOpenHook::hook();
        StartMenuEventsHook::hook();
        StartMenuTickHook::hook();
        ScreenViewPointerLocationHook::hook();
    } else {
        ScreenViewPointerLocationHook::unhook();
        StartMenuTickHook::unhook();
        StartMenuEventsHook::unhook();
        MainMenuOpenHook::unhook();
        gEventControllers.clear();
    }
    hooked = enable;
}

} // namespace playback::screen

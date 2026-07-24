#include "MainMenuHooks.h"

#include "playback/functions/replay/ReplaySession.h"
#include "playback/screen/ReplayBrowser.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/controls/TextEditComponent.h"
#include "mc/client/gui/controls/UIPropertyBag.h"
#include "mc/client/gui/screens/ScreenViewCommand.h"
#include "mc/client/gui/screens/controllers/MainMenuScreenController.h"
#include "mc/client/gui/screens/controllers/MinecraftScreenController.h"
#include "mc/client/gui/screens/controllers/StartMenuScreenController.h"
#include "mc/deps/core/string/StringHash.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace playback::screen {

namespace {

std::vector<ReplaySummary>            gAllReplayList;
std::vector<ReplaySummary>            gCurrentReplayList;
bool                                  gBrowserOpen   = false;
bool                                  gOpenRequested = false;
bool                                  gUiDirty       = false;
std::optional<std::size_t>            gHighlightedIndex;
std::optional<std::size_t>            gLastClickedIndex;
std::chrono::steady_clock::time_point gLastClickTime;
std::string                           gFilter;
ReplaySort                            gSort             = ReplaySort::LastModified;
bool                                  gSortDesc         = true;
uint                                  gSearchBoxEventId = 0;

std::unordered_set<MinecraftScreenController*> gBindingControllers;
std::unordered_set<MinecraftScreenController*> gEventControllers;

constexpr std::string_view kReplayCollection       = "playback_replays";
constexpr std::string_view kButtonOpenReplays      = "button.playback_open_replays";
constexpr std::string_view kButtonOpenSelected     = "button.playback_open_selected_replay";
constexpr std::string_view kButtonSelectReplay     = "button.playback_select_replay";
constexpr std::string_view kButtonCycleSort        = "button.playback_cycle_sort";
constexpr std::string_view kButtonSortDir          = "button.playback_sort_direction";
constexpr std::string_view kButtonRefresh          = "button.playback_refresh_replays";
constexpr std::string_view kButtonClose            = "button.playback_close_replays";
constexpr std::string_view kSearchBoxId            = "#playback_replay_search_box";
constexpr auto             kConsumeAndRefreshFocus = static_cast<::ui::ViewRequest>(
    static_cast<uint>(::ui::ViewRequest::ConsumeEvent) | static_cast<uint>(::ui::ViewRequest::DelayedFocusRefresh)
);

int replayRowCount() { return static_cast<int>(gCurrentReplayList.size()); }

ReplaySummary const* replayAt(int i) {
    return (i >= 0 && static_cast<std::size_t>(i) < gCurrentReplayList.size()) ? &gCurrentReplayList[i] : nullptr;
}

std::string formatDuration(ReplaySummary const& r) {
    int                secs = std::max(0, (r.totalTicks > 0 ? r.totalTicks : r.durationTicks)) / 20;
    int                h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    std::ostringstream o;
    if (h) o << h << ':' << std::setfill('0') << std::setw(2) << m << ':';
    else o << m << ':';
    o << std::setfill('0') << std::setw(2) << s;
    return o.str();
}

std::string formatSize(std::uintmax_t bytes) {
    constexpr const char* u[] = {"B", "KB", "MB", "GB"};
    double                v   = static_cast<double>(bytes);
    int                   i   = 0;
    while (v >= 1024.0 && i + 1 < 4) {
        v /= 1024.0;
        ++i;
    }
    std::ostringstream o;
    if (i == 0) o << static_cast<std::uintmax_t>(v);
    else o << std::fixed << std::setprecision(v >= 10.0 ? 0 : 1) << v;
    o << ' ' << u[i];
    return o.str();
}

std::string formatLastModified(ReplaySummary const& r) {
    using fs_tp = std::filesystem::file_time_type;
    if (r.lastModified == fs_tp{}) return "--";
    auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        r.lastModified - fs_tp::clock::now() + std::chrono::system_clock::now()
    );
    auto    tt = std::chrono::system_clock::to_time_t(st);
    std::tm lt{};
    if (localtime_s(&lt, &tt)) return "--";
    std::ostringstream o;
    o << std::put_time(&lt, "%Y-%m-%d %H:%M");
    return o.str();
}

std::string replayCountText() {
    return std::to_string(gCurrentReplayList.size()) + " / " + std::to_string(gAllReplayList.size());
}

std::string replaySortText() {
    switch (gSort) {
    case ReplaySort::ReplayName:
        return "playback.ui.sort.name";
    case ReplaySort::WorldName:
        return "playback.ui.sort.world";
    case ReplaySort::Duration:
        return "playback.ui.sort.duration";
    case ReplaySort::FileSize:
        return "playback.ui.sort.size";
    default:
        return "playback.ui.sort.created";
    }
}

std::string replaySortDirText() { return gSortDesc ? "playback.ui.sort.descending" : "playback.ui.sort.ascending"; }

std::string replayTitle(int i) {
    auto* r = replayAt(i);
    return r ? r->displayName() : "-";
}
std::string replayWorld(int i) {
    auto* r = replayAt(i);
    return (r && !r->worldName.empty()) ? r->worldName : "-";
}
std::string replayMetadata(int i) {
    auto* r = replayAt(i);
    if (!r) return {};
    std::ostringstream o;
    if (!r->canOpen) {
        o << "! " << (r->problem.empty() ? "--" : r->problem) << " | " << formatSize(r->fileSize);
        return o.str();
    }
    o << r->replayId << " | " << formatLastModified(*r) << " | " << formatDuration(*r) << " | "
      << formatSize(r->fileSize);
    return o.str();
}
std::string replayStatus(int i) {
    auto* replay = replayAt(i);
    return replay ? (replay->canOpen ? "playback.ui.status.ready" : "playback.ui.status.issue")
                  : "playback.ui.status.empty";
}
std::string replayIndexText(int i) {
    int d = i + 1;
    return d < 10 ? "0" + std::to_string(d) : std::to_string(d);
}
bool replayRowSelected(int i) {
    return gHighlightedIndex.has_value() && *gHighlightedIndex == static_cast<std::size_t>(i);
}
bool replayCanOpen(int i) {
    auto* r = replayAt(i);
    return r && r->canOpen;
}

bool selectedCanOpen() {
    auto* r = gHighlightedIndex.has_value() ? replayAt(static_cast<int>(*gHighlightedIndex)) : nullptr;
    return r && r->canOpen;
}

void applyListView() {
    std::filesystem::path keepPath;
    if (gHighlightedIndex.has_value() && *gHighlightedIndex < gCurrentReplayList.size())
        keepPath = gCurrentReplayList[*gHighlightedIndex].path;

    gCurrentReplayList = ReplayBrowser::filterReplays(gAllReplayList, gFilter);
    ReplayBrowser::sortReplays(gCurrentReplayList, gSort, gSortDesc);

    if (gCurrentReplayList.empty()) {
        gHighlightedIndex.reset();
        gLastClickedIndex.reset();
    } else {
        auto it = gCurrentReplayList.begin();
        if (!keepPath.empty())
            it = std::find_if(gCurrentReplayList.begin(), gCurrentReplayList.end(), [&](auto& r) {
                return r.path == keepPath;
            });
        if (it == gCurrentReplayList.end()) it = gCurrentReplayList.begin();
        gHighlightedIndex = static_cast<std::size_t>(std::distance(gCurrentReplayList.begin(), it));
        gLastClickedIndex.reset();
    }

    gUiDirty = true;
}

void refreshReplayList() {
    gAllReplayList = ReplayBrowser::loadReplays();
    applyListView();
}

void cycleSort() {
    constexpr ReplaySort order[] = {
        ReplaySort::LastModified,
        ReplaySort::ReplayName,
        ReplaySort::WorldName,
        ReplaySort::Duration,
        ReplaySort::FileSize
    };
    auto it = std::find(std::begin(order), std::end(order), gSort);
    gSort   = (it == std::end(order) || ++it == std::end(order)) ? order[0] : *it;
    applyListView();
}

void toggleSortDir() {
    gSortDesc = !gSortDesc;
    applyListView();
}

void updateFilter(std::string f) {
    if (f == gFilter) return;
    gFilter = std::move(f);
    applyListView();
}

void ensureBindings(MinecraftScreenController& ctrl) {
    if (!gBindingControllers.insert(&ctrl).second) return;

    ctrl.bindGridSize(
        StringHash("#playback_replay_grid_dimensions"),
        [] { return glm::ivec2{1, replayRowCount()}; },
        [] { return true; }
    );
    ctrl.bindString(StringHash("#playback_replay_count_text"), replayCountText, [] { return true; });
    ctrl.bindString(StringHash("#playback_replay_search_box_content"), [] { return gFilter; }, [] { return true; });
    ctrl.bindString(StringHash("#playback_replay_sort_text"), replaySortText, [] { return true; });
    ctrl.bindString(StringHash("#playback_replay_sort_direction_text"), replaySortDirText, [] { return true; });
    ctrl.bindBool(StringHash("#playback_selected_replay_can_open"), selectedCanOpen, [] { return true; });
    auto inRange = [](int i) { return i >= 0 && i < replayRowCount(); };
    ctrl.bindStringForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_title"),
        replayTitle,
        inRange
    );
    ctrl.bindStringForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_world_text"),
        replayWorld,
        inRange
    );
    ctrl.bindStringForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_metadata_text"),
        replayMetadata,
        inRange
    );
    ctrl.bindStringForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_status_text"),
        replayStatus,
        inRange
    );
    ctrl.bindStringForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_index_text"),
        replayIndexText,
        inRange
    );
    ctrl.bindBoolForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_selected"),
        replayRowSelected,
        inRange
    );
    ctrl.bindBoolForCollection(
        StringHash(kReplayCollection),
        StringHash("#playback_replay_can_open"),
        replayCanOpen,
        inRange
    );
}

bool openReplay(std::size_t index) {
    return index < gCurrentReplayList.size() && ReplayBrowser::openReplay(gCurrentReplayList[index]);
}

void openBrowser(MinecraftScreenController& ctrl) {
    refreshReplayList();
    gBrowserOpen       = true;
    auto& prepareFocus = ctrl.mScreenViewCommand.get().prepareFocusForModalPopup.get();
    if (prepareFocus) prepareFocus();
    ctrl.displayJsonDefinedControlPopup(
        "playback_replay_browser",
        "playback_replay_popup_factory",
        "playback_replay_browser"
    );
}

void closeBrowser(MinecraftScreenController& ctrl) {
    auto& destroy = ctrl.mControlDestroyCallback.get();
    if (destroy) destroy("playback_replay_popup_factory", "playback_replay_browser");
    gBrowserOpen = false;
}

void selectReplay(MinecraftScreenController& ctrl, std::size_t index) {
    if (index >= gCurrentReplayList.size()) return;

    gHighlightedIndex = index;
    auto now          = std::chrono::steady_clock::now();
    bool doubleClick  = gLastClickedIndex.has_value() && *gLastClickedIndex == index
                     && now - gLastClickTime < std::chrono::milliseconds(450);
    gLastClickedIndex = index;
    gLastClickTime    = now;

    if (doubleClick && openReplay(index)) closeBrowser(ctrl);
    else gUiDirty = true;
}

void ensureEvents(MinecraftScreenController& ctrl) {
    if (!gEventControllers.insert(&ctrl).second) return;

    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonOpenReplays)), [](UIPropertyBag*) {
        if (!gBrowserOpen) gOpenRequested = true;
        return kConsumeAndRefreshFocus;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonCycleSort)), [](UIPropertyBag*) {
        cycleSort();
        return ::ui::ViewRequest::ConsumeEvent;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonSortDir)), [](UIPropertyBag*) {
        toggleSortDir();
        return ::ui::ViewRequest::ConsumeEvent;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonRefresh)), [](UIPropertyBag*) {
        refreshReplayList();
        return ::ui::ViewRequest::ConsumeEvent;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonClose)), [&ctrl](UIPropertyBag*) {
        closeBrowser(ctrl);
        return kConsumeAndRefreshFocus;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonOpenSelected)), [&ctrl](UIPropertyBag*) {
        if (gHighlightedIndex.has_value() && openReplay(*gHighlightedIndex)) closeBrowser(ctrl);
        return ::ui::ViewRequest::ConsumeEvent;
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonSelectReplay)), [&ctrl](UIPropertyBag* bag) {
        if (bag) {
            int index = bag->mJsonValue.get().get("#collection_index", -1).asInt(-1);
            if (index >= 0) selectReplay(ctrl, static_cast<std::size_t>(index));
        }
        return ::ui::ViewRequest::ConsumeEvent;
    });

    gSearchBoxEventId = ctrl._getNameId(std::string(kSearchBoxId));
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
    ensureBindings(*this);
    ensureEvents(*this);
}

LL_TYPE_INSTANCE_HOOK(
    StartMenuBindingsHook,
    ll::memory::HookPriority::Normal,
    StartMenuScreenController,
    &StartMenuScreenController::_registerBindings,
    void
) {
    origin();
    ensureBindings(*this);
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
        openBrowser(*this);
    }
    if (gUiDirty) {
        gUiDirty = false;
        return ::ui::DirtyFlag::All;
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    TextEditSetTextHook,
    ll::memory::HookPriority::Normal,
    TextEditComponent,
    &TextEditComponent::setText,
    void,
    std::string const& text
) {
    origin(text);
    if (gBrowserOpen && static_cast<uint>(mTextEditComponentId) == gSearchBoxEventId) updateFilter(text);
}

void hookMainMenu(bool enable) {
    static bool hooked = false;
    if (hooked == enable) return;
    if (enable) {
        MainMenuOpenHook::hook();
        StartMenuBindingsHook::hook();
        StartMenuEventsHook::hook();
        StartMenuTickHook::hook();
        TextEditSetTextHook::hook();
    } else {
        TextEditSetTextHook::unhook();
        StartMenuTickHook::unhook();
        StartMenuEventsHook::unhook();
        StartMenuBindingsHook::unhook();
        MainMenuOpenHook::unhook();
        gBrowserOpen      = false;
        gOpenRequested    = false;
        gUiDirty          = false;
        gSearchBoxEventId = 0;
        gAllReplayList.clear();
        gCurrentReplayList.clear();
        gBindingControllers.clear();
        gEventControllers.clear();
    }
    hooked = enable;
}

} // namespace playback::screen

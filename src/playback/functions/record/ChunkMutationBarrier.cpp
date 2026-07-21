#include "ChunkMutationBarrier.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/multiplayer/MultiPlayerLevel.h"
#include "mc/deps/core/threading/TaskGroup.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace playback::functions {

namespace {

constexpr std::string_view InsertTaskGroupName = "SubChunk Insert Task Group";

struct InsertGroupBinding {
    MultiPlayerLevel* level{};
    SubChunkManager*  manager{};
    TaskGroup*        group{};
    std::thread::id   clientThread{};

    bool operator==(InsertGroupBinding const&) const = default;
};

struct BarrierState {
    std::timed_mutex   captureMutex;
    std::mutex         bindingMutex;
    InsertGroupBinding binding;
    MultiPlayerLevel*  activeLevel{};
    bool               discoveryHookInstalled{};
};

BarrierState& barrierState() {
    static BarrierState state;
    return state;
}

thread_local MultiPlayerLevel* tickBoundaryLevel{};

void** vtableOf(IBackgroundTaskOwner* owner) noexcept { return *reinterpret_cast<void***>(owner); }

void** findTaskGroupVtable(MultiPlayerLevel& level) {
    void** vtable{};
    level.forEachDimension([&vtable](Dimension& dimension) {
        for (auto* group : {dimension.mChunkGenTaskGroup.get(), dimension.mTaskGroup.get()}) {
            if (!vtable && group) vtable = vtableOf(group);
        }
        return true;
    });
    return vtable;
}

TaskGroup* findCurrentInsertGroup(MultiPlayerLevel& level) {
    auto* owner = TaskGroup::getCurrentTaskGroup();
    if (!owner) return nullptr;

    auto* taskGroupVtable = findTaskGroupVtable(level);
    if (!taskGroupVtable || vtableOf(owner) != taskGroupVtable) return nullptr;

    auto* group = static_cast<TaskGroup*>(owner);
    if (group->mName.get() != InsertTaskGroupName) return nullptr;
    return group;
}

void publishInsertGroup(MultiPlayerLevel& level, SubChunkManager& manager, TaskGroup& group) {
    auto&           state = barrierState();
    std::lock_guard lock(state.bindingMutex);
    if (!state.discoveryHookInstalled || state.activeLevel != &level) return;
    state.binding = {&level, &manager, &group, std::this_thread::get_id()};
}

InsertGroupBinding readBinding() {
    auto&           state = barrierState();
    std::lock_guard lock(state.bindingMutex);
    if (!state.discoveryHookInstalled || state.binding.level != state.activeLevel) return {};
    return state.binding;
}

bool bindingMatches(InsertGroupBinding const& expected) {
    auto&           state = barrierState();
    std::lock_guard lock(state.bindingMutex);
    return state.discoveryHookInstalled && state.activeLevel == expected.level && state.binding == expected;
}

MultiPlayerLevel* getCurrentLevel() {
    auto level = ll::service::getMultiPlayerLevel();
    return level ? level->asMultiPlayerLevel() : nullptr;
}

bool captureContextMatches(InsertGroupBinding const& binding) {
    if (tickBoundaryLevel != binding.level || getCurrentLevel() != binding.level
        || std::this_thread::get_id() != binding.clientThread || !bindingMatches(binding)) {
        return false;
    }

    auto manager = binding.level->getSubChunkManager();
    return manager && manager.get() == binding.manager;
}

bool collectTargetGroups(MultiPlayerLevel& level, TaskGroup& insertGroup, std::vector<TaskGroup*>& groups) {
    groups.clear();
    void** taskGroupVtable{};
    bool   valid = true;

    level.forEachDimension([&](Dimension& dimension) {
        for (auto* group : {dimension.mChunkGenTaskGroup.get(), dimension.mTaskGroup.get()}) {
            if (!group) {
                valid = false;
                continue;
            }

            auto* vtable = vtableOf(group);
            if (!taskGroupVtable) taskGroupVtable = vtable;
            if (vtable != taskGroupVtable) valid = false;
            if (std::ranges::find(groups, group) == groups.end()) groups.push_back(group);
        }
        return valid;
    });

    if (!valid || !taskGroupVtable || vtableOf(&insertGroup) != taskGroupVtable
        || insertGroup.mName.get() != InsertTaskGroupName) {
        return false;
    }

    if (std::ranges::find(groups, &insertGroup) == groups.end()) groups.push_back(&insertGroup);
    return true;
}

bool sameGroups(std::vector<TaskGroup*> const& lhs, std::vector<TaskGroup*> const& rhs) {
    return lhs.size() == rhs.size()
        && std::ranges::all_of(lhs, [&rhs](TaskGroup* group) { return std::ranges::find(rhs, group) != rhs.end(); });
}

bool groupsAreRunningAndEmpty(std::vector<TaskGroup*> const& groups) {
    return std::ranges::all_of(groups, [](TaskGroup* group) {
        return group->getState() == TaskGroupState::Running && group->isEmpty();
    });
}

bool drainTargetGroups(InsertGroupBinding const& binding, std::chrono::steady_clock::time_point deadline) {
    std::vector<TaskGroup*> groups;
    if (!collectTargetGroups(*binding.level, *binding.group, groups)) return false;

    auto* currentOwner = TaskGroup::getCurrentTaskGroup();
    if (std::ranges::any_of(groups, [currentOwner](TaskGroup* group) {
            return static_cast<IBackgroundTaskOwner*>(group) == currentOwner;
        })) {
        return false;
    }

    size_t quiescentPasses{};
    while (std::chrono::steady_clock::now() < deadline) {
        bool performedSync = false;
        for (auto* group : groups) {
            if (group->getState() != TaskGroupState::Running) return false;
            if (!group->isEmpty()) {
                performedSync = true;
                group->sync_DEPRECATED_ASK_TOMMO([] { std::this_thread::yield(); });
            }
            if (group->getState() != TaskGroupState::Running) return false;
        }

        if (!captureContextMatches(binding)) return false;

        std::vector<TaskGroup*> currentGroups;
        if (!collectTargetGroups(*binding.level, *binding.group, currentGroups) || !sameGroups(groups, currentGroups)) {
            return false;
        }
        if (groupsAreRunningAndEmpty(currentGroups)) {
            if (++quiescentPasses >= 2) return true;
        } else {
            quiescentPasses = 0;
        }

        if (!performedSync || quiescentPasses != 0) std::this_thread::yield();
    }
    return false;
}

LL_TYPE_INSTANCE_HOOK(
    ChunkMutationBarrierDiscoveryHook,
    ll::memory::HookPriority::Normal,
    MultiPlayerLevel,
    &MultiPlayerLevel::_onSubChunkLoaded,
    void,
    ChunkSource& source,
    LevelChunk&  chunk,
    short        absoluteSubChunkIndex,
    bool         subChunkVisibilityChanged
) {
    auto manager = getSubChunkManager();
    if (auto* group = findCurrentInsertGroup(*this); group && manager) publishInsertGroup(*this, *manager, *group);
    origin(source, chunk, absoluteSubChunkIndex, subChunkVisibilityChanged);
}

} // namespace

ChunkMutationBarrier::TickBoundaryGuard::TickBoundaryGuard(MultiPlayerLevel& level) noexcept
: mPreviousLevel(std::exchange(tickBoundaryLevel, &level)) {}

ChunkMutationBarrier::TickBoundaryGuard::~TickBoundaryGuard() noexcept { tickBoundaryLevel = mPreviousLevel; }

ChunkMutationBarrier::TickBoundaryGuard ChunkMutationBarrier::enterTickBoundary(MultiPlayerLevel& level) noexcept {
    return TickBoundaryGuard(level);
}

ChunkMutationBarrier::CaptureGuard::CaptureGuard(bool acquired, std::chrono::steady_clock::duration waited) noexcept
: mAcquired(acquired),
  mWaited(waited) {}

ChunkMutationBarrier::CaptureGuard::~CaptureGuard() noexcept { release(); }

void ChunkMutationBarrier::CaptureGuard::release() noexcept {
    if (!mAcquired) return;

    barrierState().captureMutex.unlock();
    mAcquired = false;
}

ChunkMutationBarrier::CaptureGuard ChunkMutationBarrier::capture(std::chrono::milliseconds timeout) {
    auto& state    = barrierState();
    auto  started  = std::chrono::steady_clock::now();
    auto  deadline = started + timeout;

    auto failed = [&] { return CaptureGuard(false, std::chrono::steady_clock::now() - started); };
    if (!tickBoundaryLevel) return failed();
    if (!state.captureMutex.try_lock_until(deadline)) return failed();

    auto binding = readBinding();
    if (!binding.level || !binding.manager || !binding.group || !captureContextMatches(binding)
        || !drainTargetGroups(binding, deadline)) {
        state.captureMutex.unlock();
        return failed();
    }

    // The guard lives inside the post-_subTick client-thread hook. With all worker groups empty, that occupied
    // client thread prevents new root chunk work and client chunk-GC flushes until serialization finishes.
    return CaptureGuard(true, std::chrono::steady_clock::now() - started);
}

void ChunkMutationBarrier::setActiveLevel(MultiPlayerLevel* level) {
    auto&           state = barrierState();
    std::lock_guard captureLock(state.captureMutex);
    std::lock_guard bindingLock(state.bindingMutex);
    if (state.activeLevel == level) return;

    state.activeLevel = level;
    state.binding     = {};
}

bool hookChunkMutationBarrier(bool enable) {
    auto&           state = barrierState();
    std::lock_guard captureLock(state.captureMutex);

    {
        std::lock_guard bindingLock(state.bindingMutex);
        if (state.discoveryHookInstalled == enable) return true;
    }

    if (enable) {
        auto* activeLevel = getCurrentLevel();
        {
            std::lock_guard bindingLock(state.bindingMutex);
            state.binding                = {};
            state.activeLevel            = activeLevel;
            state.discoveryHookInstalled = true;
        }
        if (ChunkMutationBarrierDiscoveryHook::hook() != 0) {
            std::lock_guard bindingLock(state.bindingMutex);
            state.discoveryHookInstalled = false;
            state.activeLevel            = nullptr;
            return false;
        }
        return true;
    }

    InsertGroupBinding previousBinding;
    MultiPlayerLevel*  previousActiveLevel{};
    {
        std::lock_guard bindingLock(state.bindingMutex);
        previousBinding              = state.binding;
        previousActiveLevel          = state.activeLevel;
        state.discoveryHookInstalled = false;
        state.binding                = {};
        state.activeLevel            = nullptr;
    }
    if (!ChunkMutationBarrierDiscoveryHook::unhook()) {
        std::lock_guard bindingLock(state.bindingMutex);
        state.discoveryHookInstalled = true;
        state.activeLevel            = previousActiveLevel;
        state.binding                = previousBinding;
        return false;
    }
    return true;
}

} // namespace playback::functions

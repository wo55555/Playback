#include "ReplayAttachableHooks.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/SkeletalHierarchyIndex.h"
#include "mc/world/actor/animation/AnimationComponent.h"
#include "mc/world/actor/animation/AttachableSlotIndex.h"
#include "mc/world/actor/animation/BoneBindingMethod.h"
#include "mc/world/actor/animation/BoneOrientation.h"
#include "mc/world/item/ItemStack.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace playback::visuals {

namespace {

bool isActiveHierarchy(gsl::span<SkeletalHierarchyIndex const> activeHierarchies, SkeletalHierarchyIndex hierarchy) {
    return std::ranges::find(activeHierarchies, hierarchy) != activeHierarchies.end();
}

bool hasMatchingParentBone(
    AnimationComponent&                     parentAc,
    gsl::span<SkeletalHierarchyIndex const> activeHierarchies,
    HashedString const&                     boneName
) {
    for (auto const hierarchy : activeHierarchies) {
        auto* const parentBones = parentAc.getBoneOrientations(hierarchy, true);
        if (!parentBones) continue;
        for (auto const& parentBone : *parentBones) {
            if (parentBone.getName() == boneName) return true;
        }
    }
    return false;
}

void repairMissingParentHierarchies(
    AnimationComponent&                     parentAc,
    gsl::span<SkeletalHierarchyIndex const> activeHierarchies
) {
    bool missing = false;
    for (auto const hierarchy : activeHierarchies) {
        if (parentAc.getBoneOrientations(hierarchy, true) == nullptr) {
            missing = true;
            break;
        }
    }
    if (!missing) return;

    auto initFn = parentAc.mAnimationComponentInitFunction.get();
    if (!initFn) return;
    parentAc.initializeClientAnimationComponent(std::move(initFn));
    parentAc.forceNextUpdateToApplyAnimations();
    parentAc.applyAnimations(true);
}

void repairAttachableBoneBindings(AnimationComponent& parentAc, AttachableSlotIndex attachableSlotIndex) {
    auto const activeHierarchies = parentAc.getSkeletalHierarchiesToProcess();
    if (activeHierarchies.empty()) return;

    repairMissingParentHierarchies(parentAc, activeHierarchies);

    auto const slotIndex = static_cast<size_t>(attachableSlotIndex);
    auto&      children  = parentAc.getChildAnimationComponents();
    if (slotIndex >= children.size()) return;
    auto* const childAc = children[slotIndex].mChildAnimationComponent.get();
    if (!childAc) return;

    for (auto& [childHierarchy, childBones] : childAc->getAllBoneOrientations()) {
        (void)childHierarchy;
        for (auto& bone : childBones) {
            auto const method = bone.getBoneBindingMethod();
            if (method == BoneBindingMethod::LocalHierarchy) {
                if (bone.mParentBoneIndex >= 0) continue;
                if (!hasMatchingParentBone(parentAc, activeHierarchies, bone.getName())) continue;
                bone.mParentBoneIndex              = -1;
                bone.mParentSkeletalHierarchyIndex = SkeletalHierarchyIndex::Unknown;
                bone.setBoneBindingMethod(BoneBindingMethod::Unset);
            } else if (method == BoneBindingMethod::MatchingParentBoneName) {
                auto const        cachedHierarchy = bone.mParentSkeletalHierarchyIndex;
                auto const        cachedBoneIndex = bone.mParentBoneIndex;
                auto const* const parentBones     = isActiveHierarchy(activeHierarchies, cachedHierarchy)
                                                      ? parentAc.getBoneOrientations(cachedHierarchy, true)
                                                      : nullptr;
                bool const        valid           = parentBones != nullptr && cachedBoneIndex >= 0
                                && static_cast<size_t>(cachedBoneIndex) < parentBones->size()
                                && (*parentBones)[static_cast<size_t>(cachedBoneIndex)].getName() == bone.getName();
                if (valid) continue;
                if (!hasMatchingParentBone(parentAc, activeHierarchies, bone.getName())) continue;
                bone.mParentBoneIndex              = -1;
                bone.mParentSkeletalHierarchyIndex = SkeletalHierarchyIndex::Unknown;
                bone.setOverrideStackMatrix(false);
                bone.setBoneBindingMethod(BoneBindingMethod::Unset);
            }
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    AttachableBoneBindingHook,
    ll::memory::HookPriority::Normal,
    DataDrivenModel,
    &DataDrivenModel::setupAttachableNoChecks,
    void,
    ItemStack const&           itemInstance,
    AttachableSlotIndex const& attachableSlotIndex,
    RenderParams&              renderParams,
    Actor&                     actor,
    bool                       isFirstPerson,
    bool                       runParentSetup
) {
    if (Playback::getInstance().getMode() == PlaybackMode::Replay && runParentSetup) {
        AnimationComponent* parentAc = renderParams.mAnimationComponent;
        if (!parentAc) parentAc = renderParams.mRootAnimationComponent;
        if (!parentAc) parentAc = &actor.getAnimationComponent();
        if (parentAc) repairAttachableBoneBindings(*parentAc, attachableSlotIndex);
    }
    origin(itemInstance, attachableSlotIndex, renderParams, actor, isFirstPerson, runParentSetup);
}

} // namespace

void hookReplayAttachables(bool enable, SubjectHideReason owner) {
    static bool    installed = false;
    static bool    hooked{};
    static uint8_t owners = 0;

    auto const bit = static_cast<uint8_t>(owner);
    if (enable) owners |= bit;
    else owners &= static_cast<uint8_t>(~bit);

    if (enable) {
        if (installed) return;
        auto const begin = std::chrono::steady_clock::now();
        hooked           = AttachableBoneBindingHook::hook() == 0;
        installed        = true;
        Playback::getInstance().getSelf().getLogger().debug(
            "Replay attachable hooks installed in {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count()
        );
        if (!hooked) {
            Playback::getInstance().getSelf().getLogger().warn("Replay attachable hook failed to install");
        }
        return;
    }

    if (!installed || owners != 0) return;
    if (hooked) AttachableBoneBindingHook::unhook();
    hooked    = false;
    installed = false;
}

} // namespace playback::visuals

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

#include <Windows.h>
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
            if (parentBone.mName.get() == boneName) return true;
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
    auto&      children  = parentAc.mChildAnimationComponents.get();
    if (slotIndex >= children.size()) return;
    auto* const childAc = children[slotIndex].mChildAnimationComponent.get();
    if (!childAc) return;

    for (auto& [childHierarchy, childBones] : childAc->mBoneOrientations.get()) {
        (void)childHierarchy;
        for (auto& bone : childBones) {
            auto const method = bone.mBoneBindingMethod;
            if (method == BoneBindingMethod::LocalHierarchy) {
                if (bone.mParentBoneIndex >= 0) continue;
                if (!hasMatchingParentBone(parentAc, activeHierarchies, bone.mName.get())) continue;
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
                                && (*parentBones)[static_cast<size_t>(cachedBoneIndex)].mName.get() == bone.mName.get();
                if (valid) continue;
                if (!hasMatchingParentBone(parentAc, activeHierarchies, bone.mName.get())) continue;
                bone.mParentBoneIndex              = -1;
                bone.mParentSkeletalHierarchyIndex = SkeletalHierarchyIndex::Unknown;
                bone.mOverrideStackMatrix          = false;
                bone.setBoneBindingMethod(BoneBindingMethod::Unset);
            }
        }
    }
}

// DataDrivenModel::setupAttachableNoChecks on 1.26.10.04 (callers pass 6 arguments; runParentSetup is absent).
constexpr uintptr_t SetupAttachableNoChecksRva = 0x209F1E0;

using SetupAttachableNoChecksFn = void (*)(
    DataDrivenModel*           self,
    ItemStack const&           itemInstance,
    AttachableSlotIndex const& attachableSlotIndex,
    RenderParams&              renderParams,
    Actor&                     actor,
    bool                       isFirstPerson
);

void*                       gSetupAttachableTarget{};
SetupAttachableNoChecksFn   gOriginalSetupAttachableNoChecks{};

void setupAttachableNoChecksDetour(
    DataDrivenModel*           self,
    ItemStack const&           itemInstance,
    AttachableSlotIndex const& attachableSlotIndex,
    RenderParams&              renderParams,
    Actor&                     actor,
    bool                       isFirstPerson
) {
    if (Playback::getInstance().getMode() == PlaybackMode::Replay) {
        AnimationComponent* parentAc = renderParams.mAnimationComponent;
        if (!parentAc) parentAc = renderParams.mRootAnimationComponent;
        if (!parentAc) parentAc = &actor.getAnimationComponent();
        if (parentAc) repairAttachableBoneBindings(*parentAc, attachableSlotIndex);
    }
    gOriginalSetupAttachableNoChecks(self, itemInstance, attachableSlotIndex, renderParams, actor, isFirstPerson);
}

void* resolveSetupAttachableNoChecksTarget() noexcept {
    auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (SetupAttachableNoChecksRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
    return reinterpret_cast<void*>(base + SetupAttachableNoChecksRva);
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
        gSetupAttachableTarget = resolveSetupAttachableNoChecksTarget();
        if (gSetupAttachableTarget) {
            ll::memory::FuncPtr original{};
            hooked = ll::memory::hook(
                         gSetupAttachableTarget,
                         ll::memory::toFuncPtr(&setupAttachableNoChecksDetour),
                         &original,
                         ll::memory::HookPriority::Normal
                     )
                  == 0;
            gOriginalSetupAttachableNoChecks = reinterpret_cast<SetupAttachableNoChecksFn>(original);
        } else {
            hooked = false;
        }
        installed = true;
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
    if (hooked && gSetupAttachableTarget) {
        ll::memory::unhook(gSetupAttachableTarget, ll::memory::toFuncPtr(&setupAttachableNoChecksDetour));
    }
    gSetupAttachableTarget           = nullptr;
    gOriginalSetupAttachableNoChecks = nullptr;
    hooked                           = false;
    installed                        = false;
}

} // namespace playback::visuals

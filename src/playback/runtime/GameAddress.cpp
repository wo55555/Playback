#include "playback/runtime/GameAddress.h"

#include "playback/Playback.h"

#include "ll/api/io/Logger.h"

#include <Windows.h>

#include <cstddef>
#include <iterator>

namespace playback::runtime {

namespace {

constexpr size_t FunctionCount = static_cast<size_t>(GameFunction::Count);

struct Entry {
    GameFunction function;
    uintptr_t    rva;
};

// Minecraft.Windows.exe 1.26.60.5.
constexpr DWORD ImageTimeDateStamp = 0x6AD79EE4;
constexpr DWORD ImageSize          = 0x13847000;

constexpr Entry Entries[] = {
    {GameFunction::HasFinishedLoading,                       0x6950D20},
    {GameFunction::ResourceLocationGetFullPath,              0x398C10 },
    {GameFunction::UpdateAttributesPacketConstructor,        0x3D8F290},
    {GameFunction::UpdateAttributesPacketPayloadConstructor, 0x3D8EF40},
    {GameFunction::MapGetFullDataPacket,                     0x83F2F20},
    {GameFunction::GetCameraPosition,                        0x2CEDAE0},
    {GameFunction::UpdateAttribute,                          0x2BF4780},
    {GameFunction::AttributeDataConstructor,                 0x2BEDAF0},
    {GameFunction::AttributeDataDestructor,                  0x2BEE4E0},
    {GameFunction::MapDataApplyToMap,                        0x4169670},
    {GameFunction::NetworkItemStackDescriptorGetItemStack,   0x1AECE70},
    {GameFunction::PlayerIsScoping,                          0x2E8E50 },
    {GameFunction::HudShowSurvivalUi,                        0x4B6D2A0},
    {GameFunction::PlayPredictiveSynchronizedSound,          0x1EC9570},
    {GameFunction::ActorRenderDispatcherRender,              0x594F420},
    {GameFunction::HudScreenTick,                            0x4B5EB90},
    {GameFunction::ContainerScreenIsStillValid,              0x50098C0},
    {GameFunction::RenderItemInHand,                         0x4E0F610},
    {GameFunction::ShouldRenderFirstPersonObjects,           0x4E0DBF0},
    {GameFunction::RenderTransparentFirstPerson3DObjects,    0x4E0DE00},
    {GameFunction::GetCameraTargetActor,                     0x65A8A40},
    {GameFunction::HudVignetteRender,                        0x5750F30},
    {GameFunction::TryGetPortalData,                         0x6F2D140},
    {GameFunction::ActorIsOnFire,                            0xBC4EC0 },
    {GameFunction::ClientOnTick,                             0x65A0090},
};

constexpr bool entriesInOrder() {
    if (std::size(Entries) != FunctionCount) return false;
    for (size_t i = 0; i < FunctionCount; ++i) {
        if (static_cast<size_t>(Entries[i].function) != i) return false;
    }
    return true;
}
static_assert(entriesInOrder());

uintptr_t supportedImageBase() {
    auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const dos  = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto const nt   = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == ImageTimeDateStamp && nt->OptionalHeader.SizeOfImage == ImageSize) return base;
    Playback::getInstance().getSelf().getLogger().error("This Playback build needs Minecraft 1.26.60.5");
    return 0;
}

} // namespace

uintptr_t gameAddress(GameFunction function) {
    static uintptr_t const base  = supportedImageBase();
    auto const             index = static_cast<size_t>(function);
    return base && index < FunctionCount ? base + Entries[index].rva : 0;
}

} // namespace playback::runtime

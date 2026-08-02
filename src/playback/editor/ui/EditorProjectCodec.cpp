#include "EditorProjectCodec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace playback::editor::ui {

using editing::model::AgentDetails;
using editing::model::CameraEntity;
using editing::model::CameraKeyframe;
using editing::model::CameraKind;
using editing::model::CameraLimiter;
using editing::model::CameraPath;
using editing::model::CameraPathPoint;
using editing::model::CameraPreset;
using editing::model::CameraRig;
using editing::model::CameraRigSegment;
using editing::model::CameraShake;
using editing::model::Color4;
using editing::model::EditorStateExt;
using editing::model::EasingType;
using editing::model::PresetKind;
using editing::model::RigMotion;
using editing::model::SplineType;
using editing::model::SubActor;
using editing::model::SubActorCategory;
using editing::model::Vec2;
using editing::model::Vec3;
using editing::model::WorldActorSegment;

namespace {

constexpr uint32_t kMagic = 0x334A4250U;
constexpr uint32_t kMaxCollectionSize = 4096;
constexpr uint32_t kMaxStringSize = 1U << 20;

constexpr std::array<uint32_t, 256> makeCrc32Table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1U) == 0 ? 0U : 0xEDB88320U);
        table[index] = value;
    }
    return table;
}

uint32_t calculateCrc32(std::string_view data) {
    static constexpr auto table = makeCrc32Table();
    uint32_t value = 0xFFFFFFFFU;
    for (unsigned char byte : data) value = (value >> 8) ^ table[(value ^ byte) & 0xFFU];
    return value ^ 0xFFFFFFFFU;
}

class Writer {
public:
    template <typename T> void number(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
        mData.append(bytes, sizeof(T));
    }
    void string(std::string_view value) { number<uint32_t>(static_cast<uint32_t>(value.size())); mData.append(value); }
    [[nodiscard]] std::string take() { return std::move(mData); }
private:
    std::string mData;
};

class Reader {
public:
    explicit Reader(std::string_view data) : mData(data) {}
    template <typename T> bool number(T& value) {
        if (mOffset + sizeof(T) > mData.size()) return false;
        std::memcpy(&value, mData.data() + mOffset, sizeof(T));
        mOffset += sizeof(T);
        return true;
    }
    bool string(std::string& value) {
        uint32_t size{};
        if (!number(size) || size > kMaxStringSize || mOffset + size > mData.size()) return false;
        value.assign(mData.substr(mOffset, size)); mOffset += size; return true;
    }
    [[nodiscard]] bool done() const { return mOffset == mData.size(); }
private:
    std::string_view mData;
    size_t mOffset{};
};

void writeColor(Writer& writer, Color4 value) { writer.number(value.r); writer.number(value.g); writer.number(value.b); writer.number(value.a); }
bool readColor(Reader& reader, Color4& value) { return reader.number(value.r) && reader.number(value.g) && reader.number(value.b) && reader.number(value.a); }
void writeVec2(Writer& writer, Vec2 value) { writer.number(value.x); writer.number(value.y); }
bool readVec2(Reader& reader, Vec2& value) { return reader.number(value.x) && reader.number(value.y); }
void writeVec3(Writer& writer, Vec3 value) { writer.number(value.x); writer.number(value.y); writer.number(value.z); }
bool readVec3(Reader& reader, Vec3& value) { return reader.number(value.x) && reader.number(value.y) && reader.number(value.z); }

template <typename T> bool readCount(Reader& reader, T& count) { return reader.number(count) && count <= kMaxCollectionSize; }

}

std::string EditorProjectCodec::encode(const EditorStateExt& state) {
    Writer writer;
    writer.number(kMagic); writer.number<uint32_t>(kFormatVersion); writer.number(state.totalTicks);
    writer.string(state.projectName); writer.string(state.projectPath);
    writer.number<uint32_t>(static_cast<uint32_t>(state.sequence.size()));
    for (const auto& segment : state.sequence) { writer.string(segment.id); writer.number(segment.startTick); writer.number(segment.endTick); writer.string(segment.cameraId); writeColor(writer, segment.color); writer.number<uint8_t>(segment.locked); }
    writer.string(state.worldActor.id); writer.string(state.worldActor.name); writer.number(state.worldActor.totalTicks);
    writer.number<uint32_t>(static_cast<uint32_t>(state.worldActor.segments.size()));
    for (const auto& segment : state.worldActor.segments) { writer.string(segment.id); writer.number(segment.startTick); writer.number(segment.endTick); writer.number(segment.sourceTick); writer.number(segment.speed); writeColor(writer, segment.color); writer.number<uint8_t>(segment.locked); }
    writer.number<uint32_t>(static_cast<uint32_t>(state.worldActor.subActors.size()));
    for (const auto& actor : state.worldActor.subActors) { writer.string(actor.id); writer.string(actor.name); writer.number<uint8_t>(static_cast<uint8_t>(actor.category)); writeVec3(writer, actor.position); writeVec2(writer, actor.rotation); writer.number<uint32_t>(static_cast<uint32_t>(actor.boundCameraIds.size())); for (const auto& id : actor.boundCameraIds) writer.string(id); writer.number<uint32_t>(static_cast<uint32_t>(actor.agentDetails.size())); for (const auto& [key, value] : actor.agentDetails) { writer.string(key); writer.string(value); } }
    writer.number<uint32_t>(static_cast<uint32_t>(state.cameras.size()));
    for (const auto& camera : state.cameras) { writer.string(camera.id); writer.string(camera.name); writer.number<uint8_t>(static_cast<uint8_t>(camera.kind)); writer.string(camera.bindingEntityUuid); writer.number(camera.bindingMode); writer.number(camera.bindingDamping); writer.number<uint8_t>(camera.active); writer.number<uint8_t>(camera.locked); writer.number<uint32_t>(static_cast<uint32_t>(camera.keys.size())); for (const auto& key : camera.keys) { writer.string(key.id); writer.number(key.tick); writeVec3(writer, key.position); writer.number(key.yaw); writer.number(key.pitch); writer.number(key.fov); writeColor(writer, key.tint); writer.number<uint8_t>(static_cast<uint8_t>(key.easingType)); } }
    writer.number<uint32_t>(static_cast<uint32_t>(state.markers.size()));
    for (const auto& marker : state.markers) { writer.string(marker.id); writer.string(marker.label); writer.number(marker.tick); }
    auto bytes = writer.take();
    auto crc = calculateCrc32(bytes);
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((crc >> shift) & 0xFFU));
    return bytes;
}

std::optional<EditorStateExt> EditorProjectCodec::decode(std::string_view bytes) {
    if (bytes.size() < 4) return std::nullopt;
    uint32_t storedCrc{}; for (int index = 0; index < 4; ++index) storedCrc |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[bytes.size() - 4 + index])) << (index * 8);
    auto payload = bytes.substr(0, bytes.size() - 4);
    if (calculateCrc32(payload) != storedCrc) return std::nullopt;
    Reader reader(payload); uint32_t magic{}, version{}; EditorStateExt state;
    if (!reader.number(magic) || !reader.number(version) || magic != kMagic || version != kFormatVersion || !reader.number(state.totalTicks) || !reader.string(state.projectName) || !reader.string(state.projectPath)) return std::nullopt;
    state.version = static_cast<int>(version); uint32_t count{};
    if (!readCount(reader, count)) return std::nullopt; state.sequence.resize(count);
    for (auto& s : state.sequence) { uint8_t locked{}; if (!reader.string(s.id) || !reader.number(s.startTick) || !reader.number(s.endTick) || !reader.string(s.cameraId) || !readColor(reader, s.color) || !reader.number(locked)) return std::nullopt; s.locked = locked != 0; }
    if (!reader.string(state.worldActor.id) || !reader.string(state.worldActor.name) || !reader.number(state.worldActor.totalTicks) || !readCount(reader, count)) return std::nullopt; state.worldActor.segments.resize(count);
    for (auto& s : state.worldActor.segments) { uint8_t locked{}; if (!reader.string(s.id) || !reader.number(s.startTick) || !reader.number(s.endTick) || !reader.number(s.sourceTick) || !reader.number(s.speed) || !readColor(reader, s.color) || !reader.number(locked)) return std::nullopt; s.locked = locked != 0; }
    if (!readCount(reader, count)) return std::nullopt; state.worldActor.subActors.resize(count);
    for (auto& a : state.worldActor.subActors) { uint8_t category{}; uint32_t nested{}; if (!reader.string(a.id) || !reader.string(a.name) || !reader.number(category) || category > static_cast<uint8_t>(SubActorCategory::Entities) || !readVec3(reader, a.position) || !readVec2(reader, a.rotation) || !readCount(reader, nested)) return std::nullopt; a.category = static_cast<SubActorCategory>(category); a.boundCameraIds.resize(nested); for (auto& id : a.boundCameraIds) if (!reader.string(id)) return std::nullopt; if (!readCount(reader, nested)) return std::nullopt; for (uint32_t i = 0; i < nested; ++i) { std::string key, value; if (!reader.string(key) || !reader.string(value)) return std::nullopt; a.agentDetails.emplace(std::move(key), std::move(value)); } }
    if (!readCount(reader, count)) return std::nullopt; state.cameras.resize(count);
    for (auto& c : state.cameras) { uint8_t kind{}, active{}, locked{}; uint32_t keys{}; if (!reader.string(c.id) || !reader.string(c.name) || !reader.number(kind) || kind > static_cast<uint8_t>(CameraKind::Preset) || !reader.string(c.bindingEntityUuid) || !reader.number(c.bindingMode) || !reader.number(c.bindingDamping) || !reader.number(active) || !reader.number(locked) || !readCount(reader, keys)) return std::nullopt; c.kind = static_cast<CameraKind>(kind); c.active = active != 0; c.locked = locked != 0; c.keys.resize(keys); for (auto& key : c.keys) { uint8_t easing{}; if (!reader.string(key.id) || !reader.number(key.tick) || !readVec3(reader, key.position) || !reader.number(key.yaw) || !reader.number(key.pitch) || !reader.number(key.fov) || !readColor(reader, key.tint) || !reader.number(easing) || easing > static_cast<uint8_t>(EasingType::EaseInOut)) return std::nullopt; key.easingType = static_cast<EasingType>(easing); } }
    if (!readCount(reader, count)) return std::nullopt; state.markers.resize(count);
    for (auto& m : state.markers) if (!reader.string(m.id) || !reader.string(m.label) || !reader.number(m.tick)) return std::nullopt;
    return reader.done() ? std::optional<EditorStateExt>(std::move(state)) : std::nullopt;
}

} // namespace playback::editor::ui

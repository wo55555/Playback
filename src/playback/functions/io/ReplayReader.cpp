#include "AsyncReplaySaver.h"

#include "playback/functions/action/Action.h"
#include "playback/functions/replay/ReplaySession.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>

namespace playback::functions {

static constexpr uint64 MAX_STRING_LENGTH = 65536;

void writeSnapshotContext(PlaybackBuffer& buffer, PlaybackSnapshotContext const& context) {
    buffer.writeVarInt(PlaybackSnapshotContext::FormatVersion, nullptr, nullptr);
    buffer.writeVarInt(context.dimensionId, nullptr, nullptr);
    buffer.writeVarInt(context.dimensionMinHeight, nullptr, nullptr);
    buffer.writeVarInt(context.dimensionMaxHeight, nullptr, nullptr);
    buffer.writeFloat(context.x, nullptr, nullptr);
    buffer.writeFloat(context.y, nullptr, nullptr);
    buffer.writeFloat(context.z, nullptr, nullptr);
    buffer.writeFloat(context.yaw, nullptr, nullptr);
    buffer.writeFloat(context.pitch, nullptr, nullptr);
}

PlaybackSnapshotContext readSnapshotContext(PlaybackBuffer& buffer) {
    auto const version = buffer.getVarInt().value();
    if (version != PlaybackSnapshotContext::FormatVersion) {
        throw std::runtime_error(std::format("Unsupported replay snapshot context version: {}", version));
    }

    PlaybackSnapshotContext context;
    context.dimensionId        = buffer.getVarInt().value();
    context.dimensionMinHeight = buffer.getVarInt().value();
    context.dimensionMaxHeight = buffer.getVarInt().value();
    context.x                  = buffer.getFloat().value();
    context.y                  = buffer.getFloat().value();
    context.z                  = buffer.getFloat().value();
    context.yaw                = buffer.getFloat().value();
    context.pitch              = buffer.getFloat().value();
    return context;
}

ReplayReader::ReplayReader(std::string_view data) : mBuffer(data) {
    int32_t magic = mStream.getVarInt().value();
    if (magic != MAGIC_NUMBER) {
        throw std::runtime_error("ReplayReader: invalid magic number");
    }

    int32_t actionCount    = mStream.getVarInt().value();
    auto&   actionRegistry = functions::ActionRegistry::getInstance();
    for (int32_t i = 0; i < actionCount; ++i) {
        std::string actionName = mStream.getString(MAX_STRING_LENGTH).value();
        Action*     action     = actionRegistry.getAction(actionName);

        if (action == nullptr) {
            throw std::runtime_error(std::format("Missing action: {}", actionName));
        }
        mActionMap[i] = action;
    }

    mSnapshotSize = mStream.getUnsignedInt().value();
    if (mSnapshotSize > mStream.getWritePointer() - mStream.mReadPointer) {
        throw std::runtime_error("ReplayReader: snapshot extends beyond the replay chunk");
    }
    mSnapshotOffset = mStream.mReadPointer;
    mActionsOffset  = mSnapshotOffset + static_cast<uint64>(mSnapshotSize);

    mStream.mReadPointer = mActionsOffset;
}

PlaybackSnapshotContext ReplayReader::readSnapshotContext() {
    auto const savedReadPointer = mStream.mReadPointer;
    mStream.mReadPointer        = mSnapshotOffset;
    try {
        if (mStream.mReadPointer >= mActionsOffset) {
            throw std::runtime_error("Replay snapshot does not contain a snapshot context");
        }

        auto const actionId = mStream.getVarInt().value();
        auto const actionIt = mActionMap.find(actionId);
        if (actionIt == mActionMap.end() || actionIt->second->name != ActionSnapshotContext::getInstance().name) {
            throw std::runtime_error("Replay snapshot does not begin with a snapshot context");
        }

        auto const dataSize = mStream.getUnsignedInt().value();
        if (mStream.mReadPointer > mActionsOffset || dataSize > mActionsOffset - mStream.mReadPointer) {
            throw std::runtime_error("Replay snapshot context extends beyond the replay snapshot");
        }

        std::string    data(mStream.mView.data() + mStream.mReadPointer, dataSize);
        PlaybackBuffer contextBuffer(data);
        auto           context = playback::functions::readSnapshotContext(contextBuffer);
        if (contextBuffer.mReadPointer != contextBuffer.getWritePointer()) {
            throw std::runtime_error("Replay snapshot context was not fully read");
        }
        mStream.mReadPointer = savedReadPointer;
        return context;
    } catch (...) {
        mStream.mReadPointer = savedReadPointer;
        throw;
    }
}

std::vector<PlaybackSerializedGamePacket> ReplayReader::readConfigurationPackets() {
    auto const savedReadPointer = mStream.mReadPointer;
    mStream.mReadPointer        = mSnapshotOffset;

    std::vector<PlaybackSerializedGamePacket> packets;
    try {
        while (mStream.mReadPointer < mActionsOffset) {
            auto const actionId = mStream.getVarInt().value();
            auto const actionIt = mActionMap.find(actionId);
            if (actionIt == mActionMap.end()) {
                throw std::runtime_error(std::format("Unknown replay snapshot action id: {}", actionId));
            }

            auto const dataSize = mStream.getUnsignedInt().value();
            if (mStream.mReadPointer > mActionsOffset || dataSize > mActionsOffset - mStream.mReadPointer) {
                throw std::runtime_error(std::format(
                    "Action {} extends beyond the replay snapshot",
                    actionIt->second->name
                ));
            }

            if (actionIt->second->name == ActionConfigurationPacket::getInstance().name) {
                std::string    data(mStream.mView.data() + mStream.mReadPointer, dataSize);
                PlaybackBuffer configuration(data);
                auto const     packetId = configuration.getVarInt().value();
                auto const     remaining = configuration.getWritePointer() - configuration.mReadPointer;
                packets.push_back({
                    packetId,
                    std::string(configuration.mView.data() + configuration.mReadPointer, remaining)
                });
            }
            mStream.mReadPointer += dataSize;
        }
        mStream.mReadPointer = savedReadPointer;
        return packets;
    } catch (...) {
        mStream.mReadPointer = savedReadPointer;
        throw;
    }
}

void ReplayReader::handleSnapshot(ReplaySession& session) {
    mStream.mReadPointer = mSnapshotOffset;

    session.mIsProcessingSnapshot = true;

    while (mStream.mReadPointer < mActionsOffset) {
        int32_t id = mStream.getVarInt().value();
        auto    it = mActionMap.find(id);
        if (it == mActionMap.end()) {
            throw std::runtime_error(std::format("Unknow action id: {}. Last action was {}", id, mLastActionName));
        }
        Action* action  = it->second;
        mLastActionName = action->name;

        uint32_t dataSize = mStream.getUnsignedInt().value();
        if (mStream.mReadPointer > mActionsOffset || dataSize > mActionsOffset - mStream.mReadPointer) {
            throw std::runtime_error(std::format("Action {} extends beyond the replay snapshot", mLastActionName));
        }
        std::string    buf(mStream.mView.data() + mStream.mReadPointer, dataSize);
        PlaybackBuffer stream(buf);
        action->handle(session, stream);

        if (stream.mReadPointer != stream.getWritePointer()) {
            throw std::runtime_error(std::format(
                "Action {} failed to fully read. Had {} bytes available, only read {}",
                mLastActionName,
                stream.getWritePointer(),
                stream.mReadPointer
            ));
        }
        mStream.mReadPointer += dataSize;
    }

    session.mIsProcessingSnapshot = false;
}

bool ReplayReader::handleNextAction(ReplaySession& session) {
    if (mStream.mReadPointer >= mStream.getWritePointer()) return false;
    if (mStream.mReadPointer < mActionsOffset) {
        mStream.mReadPointer = mActionsOffset;
    }

    int32_t id = mStream.getVarInt().value();
    auto    it = mActionMap.find(id);
    if (it == mActionMap.end()) {
        throw std::runtime_error(std::format("Unknow action id: {}. Last action was {}", id, mLastActionName));
    }
    Action* action  = it->second;
    mLastActionName = action->name;

    uint32_t dataSize = mStream.getUnsignedInt().value();
    if (dataSize > mStream.getWritePointer() - mStream.mReadPointer) {
        throw std::runtime_error(std::format("Action {} extends beyond the replay chunk", mLastActionName));
    }

    std::string    buf(mStream.mView.data() + mStream.mReadPointer, dataSize);
    PlaybackBuffer stream(buf);
    action->handle(session, stream);

    if (stream.mReadPointer != stream.getWritePointer()) {
        throw std::runtime_error(std::format(
            "Action {} failed to fully read. Had {} bytes available, only read {}",
            mLastActionName,
            stream.getWritePointer(),
            stream.mReadPointer
        ));
    }

    mStream.mReadPointer += dataSize;

    return true;
}

} // namespace playback::functions

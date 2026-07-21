#include "AsyncReplaySaver.h"

#include "playback/functions/action/Action.h"
#include "playback/functions/replay/ReplaySession.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>

namespace playback::functions {

static constexpr uint64 MAX_STRING_LENGTH = 65536;

ReplayReader::ReplayReader(std::string_view data) : mBuffer(data) {
    int32_t magic = mStream.getVarInt().value();
    if (magic != MAGIC_NUMBER) {
        throw std::runtime_error("ReplayReader: invalid magic number");
        return;
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

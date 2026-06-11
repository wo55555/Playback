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
        // ll::makeError("ReplayReader: invalid magic number");
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

    mSnapshotSize   = mStream.getVarInt().value();
    mSnapshotOffset = mStream.mReadPointer;
    mActionOffset   = mSnapshotOffset + mSnapshotSize;

    mStream.mReadPointer = mActionOffset;
}

void ReplayReader::handleSnapshot(ReplaySession& session) {
    mStream.mReadPointer = mSnapshotOffset;

    while (mStream.mReadPointer < mActionOffset) {
        int32_t id = mStream.getVarInt().value();
        auto    it = mActionMap.find(id);
        if (it == mActionMap.end()) {
            throw std::runtime_error(std::format("Unknow action id: {}. Last action was {}", id, mLastActionName));
        }
        Action* action  = it->second;
        mLastActionName = action->name;

        int32_t dataSize = mStream.getVarInt().value();

        if (mStream.mReadPointer + dataSize > mStream.mView.size()) {
            throw std::runtime_error(
                std::format(
                    "Action {} failed to fully read. Had {} bytes available, only read {}",
                    mLastActionName,
                    mStream.mView.size(),
                    mStream.mReadPointer
                )
            );
        }

        std::string  buf(mStream.mView.data() + mStream.mReadPointer, dataSize);
        BinaryStream stream(buf);
        action->handle(session, stream);
    }
}

bool ReplayReader::handleNextAction(ReplaySession& session) {
    if (mStream.mReadPointer >= mStream.mView.size()) return false;
    if (mStream.mReadPointer < mActionOffset) {
        mStream.mReadPointer = mActionOffset;
    }

    int32_t id = mStream.getVarInt().value();
    auto    it = mActionMap.find(id);
    if (it == mActionMap.end()) {
        throw std::runtime_error(std::format("Unknow action id: {}. Last action was {}", id, mLastActionName));
    }
    Action* action  = it->second;
    mLastActionName = action->name;

    int32_t dataSize = mStream.getVarInt().value();
    if (mStream.mReadPointer + dataSize > mStream.mView.size()) return false;

    std::string  buf(mStream.mView.data() + mStream.mReadPointer, dataSize);
    BinaryStream stream(buf);
    action->handle(session, stream);

    mStream.mReadPointer += dataSize;

    return true;
}

} // namespace playback::functions

#include "AsyncReplaySaver.h"

#include "playback/functions/action/Action.h"
#include "playback/functions/io/cache/CachedChunkPacket.h"

#include "mc/network/packet/LevelChunkPacket.h"

#include "snappy.h"
#include <uuid.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>


namespace playback::functions {

namespace {

std::filesystem::path createRecordPath() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return utils::PathUtils::createTemp(uuids::to_string(id));
}

} // namespace

AsyncReplaySaver::AsyncReplaySaver() {
    mRecordPath = createRecordPath();

    mReplayWriter.writeHeader();

    mRunning      = true;
    mFinished     = false;
    mWorkerThread = std::thread(&AsyncReplaySaver::workerLoop, this);
}

AsyncReplaySaver::~AsyncReplaySaver() {
    if (mRunning) {
        cancel();
    }
}

void AsyncReplaySaver::workerLoop() {
    while (true) {
        WriteTask task;

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mCondition.wait(lock, [this] { return !mQueue.empty() || !mRunning; });

            if (!mRunning && mQueue.empty()) {
                break;
            }

            if (!mQueue.empty()) {
                task = std::move(mQueue.front());
                mQueue.erase(mQueue.begin());
            }
        }

        if (task) {
            task(mReplayWriter);
        }
    }

    flushCurrentChunkCacheFile();

    mFinished = true;
}

void AsyncReplaySaver::submit(WriteTask task) {
    if (!mRunning) return;

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.push_back(std::move(task));
    }
    mCondition.notify_one();
}

std::filesystem::path AsyncReplaySaver::finish() {
    if (!mRunning) return mRecordPath;

    mRunning = false;
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    return mRecordPath;
}

void AsyncReplaySaver::cancel() {
    mRunning = false;

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.clear();
    }
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }
}

void AsyncReplaySaver::writeGamePackets(std::vector<std::shared_ptr<Packet>> packets) {
    auto sharedPackets = std::make_shared<std::vector<std::shared_ptr<Packet>>>(std::move(packets));

    submit([packets = std::move(sharedPackets), this](ReplayWriter& writer) {
        for (auto& packet : *packets) {
            if (!packet) continue;

            if (auto* levelChunkPacket = dynamic_cast<const LevelChunkPacket*>(packet.get())) {
                int index = -1;

                auto cachedChunkPacket = CachedChunkPacket(*levelChunkPacket, -1);
                bool add               = true;

                std::vector<CachedChunkPacket>& cached = mCachedChunkPackets[cachedChunkPacket.mLongHashCode];
                for (const auto& existingChunkPacket : cached) {
                    if (cachedChunkPacket == existingChunkPacket) {
                        add   = false;
                        index = existingChunkPacket.mIndex;
                        break;
                    }
                }

                if (add) {
                    index                      = mTotalWrittenChunkPackets;
                    mTotalWrittenChunkPackets += 1;

                    int cacheIndex = index / CHUNK_CACHE_SIZE;
                    if (mCurrentChunkCacheIndex >= 0 && cacheIndex != mCurrentChunkCacheIndex) {
                        flushCurrentChunkCacheFile();
                    }
                    mCurrentChunkCacheIndex = cacheIndex;

                    uint64_t startWriterIndex = mChunkCacheOutput.getWritePointer();
                    mChunkCacheOutput.writeVarInt(-1, nullptr, nullptr);

                    packet->write(mChunkCacheOutput);
                    uint64_t endWriterIndex = mChunkCacheOutput.getWritePointer();

                    int32_t size = static_cast<int32_t>(endWriterIndex - startWriterIndex) - 4;
                    mChunkCacheOutput.writeAt(startWriterIndex, size);

                    cachedChunkPacket.mIndex = index;
                    cached.push_back(cachedChunkPacket);
                }

                writer.startAction(ActionLevelChunkCached::getInstance());
                writer.mStream.writeVarInt(index, nullptr, nullptr);
                writer.finishAction(ActionLevelChunkCached::getInstance());

                continue;
            }
        }
    });
}

void AsyncReplaySaver::flushCurrentChunkCacheFile() {
    if (mCurrentChunkCacheIndex < 0) return;

    writeChunkCacheFile(mChunkCacheOutput, mCurrentChunkCacheIndex);
    mChunkCacheOutput.clear();
    mCurrentChunkCacheIndex = -1;
}

void AsyncReplaySaver::writeChunkCacheFile(PlaybackBuffer const& chunkCacheOutput, int index) {
    if (chunkCacheOutput.mBuffer.empty() || chunkCacheOutput.getWritePointer() == 0) return;

    std::error_code ec;
    auto            levelChunkCachePath = mRecordPath / "level_chunk_caches";
    std::filesystem::create_directories(levelChunkCachePath, ec);
    if (ec) return;

    std::string compressed;
    snappy::Compress(chunkCacheOutput.mBuffer.data(), chunkCacheOutput.mBuffer.size(), &compressed);

    std::ofstream file(levelChunkCachePath / (std::to_string(index) + ".bin"), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;

    file.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
}

void AsyncReplaySaver::writeReplayChunk(std::string chunkName, std::string metadata) {
    submit([chunkName = std::move(chunkName), metadata = std::move(metadata), this](ReplayWriter& writer) {
        std::error_code       ec;
        std::filesystem::path chunkFile = mRecordPath / chunkName;

        std::ofstream chunk(chunkFile, std::ios::binary | std::ios::trunc);
        if (!chunk) {
            throw std::runtime_error("Failed to open chunk file");
        }

        std::string chunkData = writer.popBuffer();
        chunk.write(chunkData.data(), static_cast<std::streamsize>(chunkData.size()));
        if (!chunk) {
            throw std::runtime_error("Failed to write chunk data");
        }

        std::filesystem::path metaFile = mRecordPath / "metadata.json";
        if (std::filesystem::exists(metaFile, ec)) {
            std::filesystem::path oldMeta = mRecordPath / "metadata.json.old";
            std::filesystem::remove(oldMeta, ec);
            if (ec) {
                throw std::runtime_error("Failed to remove old metadata backup");
            }
            std::filesystem::rename(metaFile, oldMeta, ec);
            if (ec) {
                throw std::runtime_error("Failed to rename metadata.json to .old");
            }
        }

        std::ofstream meta(metaFile, std::ios::binary | std::ios::trunc);
        if (!meta) {
            throw std::runtime_error("Failed to open metadata file");
        }
        meta.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
        if (!meta) {
            throw std::runtime_error("Failed to write metadata");
        }
    });
}

} // namespace playback::functions

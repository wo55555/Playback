#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BedrockBlockNames.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/VanillaDimensions.h"
#include "mc/world/level/storage/LevelData.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <uuid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

std::string sanitizeFileName(std::string name) {
    for (auto& ch : name) {
        auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|'
            || ch == '?' || ch == '*') {
            ch = '_';
        }
    }

    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    return name.empty() ? "replay" : name;
}

std::string currentReplayTimestampName() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::string uuidReplayName() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return uuids::to_string(id);
}

std::string findAvailableReplayName(std::filesystem::path const& replayDir, std::string baseName) {
    constexpr std::string_view extension = ".zip";

    baseName = sanitizeFileName(std::move(baseName));

    for (int index = 0; index < 10000; ++index) {
        std::string filename = baseName;
        if (index > 0) {
            filename += " (" + std::to_string(index) + ")";
        }
        filename += extension;

        std::error_code ec;
        bool            exists = std::filesystem::exists(replayDir / filename, ec);
        if (ec) {
            getLogger().error("Error while trying to determine replay filename: {}", ec.message());
            break;
        }
        if (!exists) {
            return filename;
        }
    }

    return uuidReplayName() + std::string(extension);
}

std::string
snapshotFailure(ChunkPos const& pos, std::optional<int> subChunkY, std::string_view stage, std::string_view reason) {
    std::ostringstream stream;
    stream << "Chunk snapshot failed at column (" << pos.x << ", " << pos.z << ')';
    if (subChunkY) stream << ", subchunk Y " << *subChunkY;
    stream << " during " << stage << ": " << reason;
    return stream.str();
}

nlohmann::ordered_json metaToJson(PlaybackMeta const& meta) {
    auto chunks = nlohmann::ordered_json::object();
    for (auto const& [chunkName, chunkMeta] : meta.chunks) {
        chunks[chunkName] = metaToJson(chunkMeta);
    }

    nlohmann::ordered_json json{
        {"name",       meta.name        },
        {"worldName",  meta.worldName   },
        {"duration",   meta.duration    },
        {"totalTicks", meta.totalTicks  },
        {"chunks",     std::move(chunks)}
    };

    if (meta.initialView) {
        auto const& view    = *meta.initialView;
        json["initialView"] = {
            {"x",     view.x    },
            {"y",     view.y    },
            {"z",     view.z    },
            {"yaw",   view.yaw  },
            {"pitch", view.pitch}
        };
    }

    return json;
}

PlaybackMeta metaFromJson(nlohmann::ordered_json const& json) {
    PlaybackMeta meta;
    meta.name       = json.value("name", meta.name);
    meta.worldName  = json.value("worldName", meta.worldName);
    meta.duration   = json.value("duration", meta.duration);
    meta.totalTicks = json.value("totalTicks", meta.totalTicks);

    auto initialViewIt = json.find("initialView");
    if (initialViewIt != json.end()) {
        if (!initialViewIt->is_object()) {
            throw std::invalid_argument("Playback metadata initialView must be an object");
        }

        meta.initialView = PlaybackView{
            initialViewIt->at("x").get<float>(),
            initialViewIt->at("y").get<float>(),
            initialViewIt->at("z").get<float>(),
            initialViewIt->at("yaw").get<float>(),
            initialViewIt->at("pitch").get<float>()
        };
    }

    auto chunksIt = json.find("chunks");
    if (chunksIt == json.end() || chunksIt->is_null()) {
        return meta;
    }

    if (!chunksIt->is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunksIt->begin(); it != chunksIt->end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), metaFromJson(it.value()));
    }
    return meta;
}

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json);
    return metaFromJson(j);
}

std::string PlaybackMeta::toJson() const { return metaToJson(*this).dump(); }

Recorder::Recorder() : mSnapshotLevelChunks{}, mSnapshotSubChunks{} {
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
}

void Recorder::start() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer || localPlayer->getDimensionId() != VanillaDimensions::Overworld()) {
        getLogger().error("Recording is only supported in the Overworld");
        return;
    }

    if (mState.load() == State::Paused) {
        mState = State::Recording;
        getLogger().debug("Resume recording");
        return;
    }

    auto& playback = playback::Playback::getInstance();
    if (playback.isReplayMode()) {
        mState = State::Idle;
        getLogger().debug("Skip recording because current save is a replay save");
        return;
    }

    if (mState.load() != State::Idle) {
        getLogger().debug("Recorder is already active");
        return;
    }

    resetStateForNewRecording();
    if (auto error = mAsyncReplaySaver->getError()) {
        cancelRecording(*error);
        return;
    }

    if (!hookNetwork(true)) {
        cancelRecording("Required replay network hooks are unavailable");
        return;
    }

    mState = State::Recording;
    getLogger().info("Recording started");
}

void Recorder::pause() {
    if (mState.load() != State::Recording) return;
    mState = State::Paused;
}

void Recorder::stop() {
    State state = mState.exchange(State::Closing);
    if (state == State::Idle) {
        getLogger().debug("Recorder is not active");
        mState = State::Idle;
        return;
    }
    if (state == State::Closing) {
        getLogger().debug("Recorder is already closing");
        return;
    }

    endTick(true);
    if (mState.load() != State::Closing) return;
    saveRecording();
}

void Recorder::saveRecording() {
    if (!mAsyncReplaySaver) {
        getLogger().error("Failed to stop recording because replay saver is not initialized");
        mState = State::Idle;
        return;
    }

    auto replayPath = mAsyncReplaySaver->finish();
    auto saverError = mAsyncReplaySaver->getError();
    mAsyncReplaySaver.reset();
    mState = State::Idle;

    if (saverError || replayPath.empty()) {
        getLogger().error(
            "Failed to save recording: {}",
            saverError.value_or("the replay saver did not return a completed recording path")
        );
        return;
    }

    auto            replayDir = utils::PathUtils::getReplaysDir();
    std::error_code ec;
    std::filesystem::create_directories(replayDir, ec);
    if (ec) {
        getLogger().error("Error while trying to create replay folder: {}", ec.message());
        return;
    }

    auto outputPath = replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName());
    if (!ReplayExporter::exportReplay(replayPath, outputPath, "")) {
        getLogger().error("Failed to save replay data after recording stopped");
        return;
    }
}

void Recorder::resetStateForNewRecording() {
    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>();

    mMetadata.chunks.clear();
    mMetadata.initialView.reset();
    mRecordingDimension.reset();
    resetChunkSnapshot();
    mOpenChunkView.reset();
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
    mChunkIndex           = 0;
    mTicksInCurrentChunk  = 0;
    mWrittenTicks         = 0;
    mLongestSnapshotStall = {};
    mHasOpenChunk         = false;
    mOpenChunkHasData     = false;
    mNeedsInitialSnapshot = true;
}

void Recorder::endTick(bool close) {
    const auto state = mState.load();
    if (state != State::Recording && state != State::Closing) return;

    if (mAsyncReplaySaver) {
        if (auto error = mAsyncReplaySaver->getError()) {
            failRecording("Replay saver failed: " + *error);
            return;
        }
    }

    if (mRecordingDimension) {
        auto  clientInstance = ll::service::getClientInstance();
        auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
        if (!localPlayer || localPlayer->getDimensionId() != *mRecordingDimension) {
            failRecording("Changing dimensions while recording is not supported");
            return;
        }
    }

    bool const rotateChunk =
        !close && !mNeedsInitialSnapshot.load() && mHasOpenChunk && mTicksInCurrentChunk + 1 >= RECORD_CHUNK_TICKS;

    if (rotateChunk) {
        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        bool                                captured = captureChunkSnapshot(barrierWait);
        auto                                elapsed  = std::chrono::steady_clock::now() - captureStart;

        if (!captured) {
            getLogger().error(
                "Replay snapshot capture failed after {:.3f} ms: {}",
                std::chrono::duration<double, std::milli>(elapsed).count(),
                mSnapshotFailure
            );
            failRecording(mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure);
            return;
        }

        if (!writeTickBoundary() || !finishCurrentChunk(false) || !writeSnapshot()
            || !commitChunkSnapshot(elapsed, barrierWait)) {
            return;
        }
        return;
    }

    if (!writeInitialSnapshotIfNeeded() || !writeTickBoundary()) return;
    if (close && !finishCurrentChunk(true)) return;
}

void Recorder::resetChunkSnapshot() {
    mSnapshotLevelChunks.clear();
    mSnapshotSubChunks.clear();
    mSnapshotView.reset();
    mSnapshotFailure.clear();
}

bool Recorder::captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait) {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        mSnapshotFailure = "The local player is not ready for a chunk snapshot";
        return false;
    }

    auto const dimension = localPlayer->getDimensionId();
    if (dimension != VanillaDimensions::Overworld()) {
        mSnapshotFailure = "Recording is only supported in the Overworld";
        return false;
    }
    if (mRecordingDimension && *mRecordingDimension != dimension) {
        mSnapshotFailure = "Changing dimensions while recording is not supported";
        return false;
    }

    auto mutationGuard = ChunkMutationBarrier::capture();
    barrierWait        = mutationGuard.waited();
    if (!mutationGuard) {
        mSnapshotFailure = "Unable to acquire the chunk mutation barrier";
        return false;
    }

    auto* guardedPlayer = clientInstance->getLocalPlayer();
    if (guardedPlayer != localPlayer || !guardedPlayer || guardedPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while acquiring the chunk mutation barrier";
        return false;
    }
    auto& dimensionObject = guardedPlayer->getDimension();

    struct SnapshotColumn {
        ChunkPos                    pos;
        std::shared_ptr<LevelChunk> chunk;
    };

    std::vector<SnapshotColumn> columns;
    auto const&                 storage = dimensionObject.getChunkSource().getStorage();
    columns.reserve(storage.size());

    for (auto const& [pos, weakChunk] : storage) {
        auto chunk = weakChunk.lock();
        if (!chunk || chunk->mIsEmptyClientChunk
            || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
            continue;
        }
        columns.push_back(SnapshotColumn{pos, std::move(chunk)});
    }

    if (columns.empty()) {
        mSnapshotFailure = "No loaded level chunks are available for the snapshot";
        return false;
    }

    auto const& position = localPlayer->getPosition();
    auto const& rotation = localPlayer->getRotation();
    auto const  view     = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    auto const  center   = SubChunkPos{
        static_cast<int>(std::floor(view.x / 16.0f)),
        static_cast<int>(std::floor(view.y / 16.0f)),
        static_cast<int>(std::floor(view.z / 16.0f))
    };

    std::sort(columns.begin(), columns.end(), [&center](auto const& left, auto const& right) {
        auto leftX         = left.pos.x - center.x;
        auto leftZ         = left.pos.z - center.z;
        auto rightX        = right.pos.x - center.x;
        auto rightZ        = right.pos.z - center.z;
        auto leftDistance  = leftX * leftX + leftZ * leftZ;
        auto rightDistance = rightX * rightX + rightZ * rightZ;
        if (leftDistance != rightDistance) return leftDistance < rightDistance;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    auto air = Block::tryGetFromRegistry(BedrockBlockNames::Air());
    if (!air) {
        mSnapshotFailure = "Unable to resolve the engine air block for a chunk snapshot";
        return false;
    }

    auto const dimensionMinHeight = static_cast<int>(dimensionObject.mHeightRange->mMin);
    auto const dimensionMaxHeight = static_cast<int>(dimensionObject.mHeightRange->mMax);
    auto const expectedSubChunks  = static_cast<size_t>(dimensionObject.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != expectedSubChunks) {
        mSnapshotFailure = "The current dimension has an invalid subchunk height range";
        return false;
    }

    // --- Parallel column serialization ---
    size_t const       numColumns = columns.size();
    unsigned int const numThreads = std::max(1u, std::thread::hardware_concurrency());
    size_t const       batchSize  = std::max(size_t{1}, (numColumns + numThreads - 1) / numThreads);

    struct ColumnResult {
        std::shared_ptr<LevelChunkPacket> levelChunk;
        std::shared_ptr<SubChunkPacket>   subChunk;
        std::string                       error;
    };

    std::vector<std::future<std::vector<ColumnResult>>> futures;

    for (unsigned int t = 0; t < numThreads; ++t) {
        size_t start = t * batchSize;
        if (start >= numColumns) break;
        size_t end = std::min(start + batchSize, numColumns);

        futures.push_back(std::async(
            std::launch::async,
            [&columns, start, end, dimension, air, expectedSubChunks, dimensionMinHeight](
            ) -> std::vector<ColumnResult> {
                std::vector<ColumnResult> results;
                results.reserve(end - start);

                auto saveContext = SaveContextFactory::createNetworkSaveContext();
                if (!saveContext) {
                    results.push_back({{}, {}, "Unable to create a network SaveContext for block actors"});
                    return results;
                }

                for (size_t ci = start; ci < end; ++ci) {
                    ColumnResult result;
                    auto const&  pos   = columns[ci].pos;
                    auto&        chunk = *columns[ci].chunk;

                    if (chunk.mIsEmptyClientChunk
                        || chunk.mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                        result.error =
                            snapshotFailure(pos, std::nullopt, "starting column serialization", "chunk unloaded");
                        results.push_back(std::move(result));
                        return results;
                    }

                    auto const& subChunks = *chunk.mSubChunks;
                    if (subChunks.empty()) {
                        result.error = snapshotFailure(pos, std::nullopt, "validating slots", "no subchunk slots");
                        results.push_back(std::move(result));
                        return results;
                    }
                    if (subChunks.size() != expectedSubChunks
                        || subChunks.size() > static_cast<size_t>(std::numeric_limits<schar>::max()) + 1) {
                        result.error = snapshotFailure(
                            pos,
                            std::nullopt,
                            "validating slots",
                            "subchunk count does not cover the complete dimension height in one packet"
                        );
                        results.push_back(std::move(result));
                        return results;
                    }

                    std::string stage = "creating LevelChunkPacket";
                    try {
                        auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
                        if (!levelBase || levelBase->getId() != MinecraftPacketIds::FullChunkData) {
                            result.error =
                                snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type");
                            results.push_back(std::move(result));
                            return results;
                        }
                        auto level = std::static_pointer_cast<LevelChunkPacket>(std::move(levelBase));

                        level->mPos                           = pos;
                        level->mDimensionId                   = dimension;
                        level->mCacheEnabled                  = false;
                        level->mSubChunksCount                = 0;
                        level->mClientNeedsToRequestSubchunks = true;
                        level->mClientRequestSubChunkLimit    = -1;
                        level->mCacheMetadata->clear();

                        stage = "serializing biome and border data";
                        BinaryStream     levelPayload;
                        VarIntDataOutput levelOutput(levelPayload);
                        chunk.serializeBiomes(levelOutput);
                        chunk.serializeBorderBlocks(levelOutput);
                        level->mSerializedChunk = std::move(levelPayload.mBuffer);

                        stage             = "creating SubChunkPacket";
                        auto subChunkBase = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
                        if (!subChunkBase || subChunkBase->getId() != MinecraftPacketIds::SubChunkPacket) {
                            result.error =
                                snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type");
                            results.push_back(std::move(result));
                            return results;
                        }
                        auto subChunkPacket   = std::static_pointer_cast<SubChunkPacket>(std::move(subChunkBase));
                        int  minimumSubChunkY = dimensionMinHeight / 16;

                        subChunkPacket->mCacheEnabled  = false;
                        subChunkPacket->mDimensionType = dimension;
                        subChunkPacket->mCenterPos     = SubChunkPos{pos.x, minimumSubChunkY, pos.z};
                        subChunkPacket->mSubChunkData->clear();
                        subChunkPacket->mSubChunkData->reserve(subChunks.size());

                        for (size_t index = 0; index < subChunks.size(); ++index) {
                            auto const& subChunk = subChunks[index];
                            int absoluteY = static_cast<int>(static_cast<unsigned char>(subChunk.mAbsoluteIndex));
                            stage         = "validating subchunk slot";

                            if (subChunk.isPlaceHolderSubChunk()) continue;
                            if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
                                && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished) {
                                continue;
                            }

                            BinaryStream serializedSubChunk;
                            bool         allAir = subChunk.isUniform(*air);
                            if (!allAir) {
                                VarIntDataOutput output(serializedSubChunk);
                                subChunk.serialize(output, true);
                            }

                            stage = "serializing block actors";
                            {
                                VarIntDataOutput output(serializedSubChunk);
                                chunk.serializeBlockEntitiesForSubChunk(
                                    output,
                                    SubChunkPos{pos.x, absoluteY, pos.z},
                                    *saveContext
                                );
                            }

                            SubChunkPacket::SubChunkPosOffset offset{};
                            offset.mX       = 0;
                            offset.mY       = static_cast<schar>(index);
                            offset.mZ       = 0;
                            auto resultFlag = allAir ? SubChunkPacket::SubChunkRequestResult::SuccessAllAir
                                                     : SubChunkPacket::SubChunkRequestResult::Success;
                            subChunkPacket->mSubChunkData->emplace_back(offset, resultFlag);
                            auto& data               = subChunkPacket->mSubChunkData->back();
                            data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
                            data.mBlobId             = 0;

                            stage = "populating heightmaps";
                            chunk.populateHeightMapDataForSubChunkPacket(static_cast<short>(absoluteY), data);
                        }

                        result.levelChunk = std::move(level);
                        if (!subChunkPacket->mSubChunkData->empty()) {
                            result.subChunk = std::move(subChunkPacket);
                        }
                    } catch (std::exception const& exception) {
                        result.error = snapshotFailure(pos, std::nullopt, stage, exception.what());
                        results.push_back(std::move(result));
                        return results;
                    } catch (...) {
                        result.error = snapshotFailure(pos, std::nullopt, stage, "unknown engine serialization error");
                        results.push_back(std::move(result));
                        return results;
                    }

                    results.push_back(std::move(result));
                }
                return results;
            }
        ));
    }

    // Collect results from all batches
    std::vector<std::shared_ptr<LevelChunkPacket>> levelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   subChunkPackets;
    levelChunks.reserve(columns.size());
    subChunkPackets.reserve(columns.size());

    for (auto& future : futures) {
        auto batchResults = future.get();
        for (auto& result : batchResults) {
            if (!result.error.empty()) {
                mSnapshotFailure = std::move(result.error);
                return false;
            }
            levelChunks.emplace_back(std::move(result.levelChunk));
            if (result.subChunk) {
                subChunkPackets.emplace_back(std::move(result.subChunk));
            }
        }
    }

    auto* finalPlayer = clientInstance->getLocalPlayer();
    if (finalPlayer != localPlayer || !finalPlayer || finalPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while capturing the chunk snapshot";
        return false;
    }

    mSnapshotLevelChunks = std::move(levelChunks);
    mSnapshotSubChunks   = std::move(subChunkPackets);
    mSnapshotView        = view;
    if (!mMetadata.initialView) {
        mMetadata.initialView = view;
    }
    mRecordingDimension = dimension;

    size_t subChunkCount = 0;
    for (auto const& packet : mSnapshotSubChunks) subChunkCount += packet->mSubChunkData->size();
    getLogger()
        .debug("Prepared replay snapshot with {} columns and {} subchunks", mSnapshotLevelChunks.size(), subChunkCount);
    return true;
}

bool Recorder::commitChunkSnapshot(
    std::chrono::steady_clock::duration captureElapsed,
    std::chrono::steady_clock::duration barrierWait
) {
    mOpenChunkView = mSnapshotView;
    mSnapshotView.reset();
    mLongestSnapshotStall = std::max(mLongestSnapshotStall, captureElapsed);
    mNeedsInitialSnapshot = false;
    mHasOpenChunk         = true;
    mOpenChunkHasData     = true;

    auto elapsedMs = std::chrono::duration<double, std::milli>(captureElapsed).count();
    auto longestMs = std::chrono::duration<double, std::milli>(mLongestSnapshotStall).count();
    auto waitMs    = std::chrono::duration<double, std::milli>(barrierWait).count();
    getLogger().info(
        "Captured replay snapshot with {} columns in {:.3f} ms (longest stall {:.3f} ms, barrier wait {:.3f} ms)",
        mSnapshotLevelChunks.size(),
        elapsedMs,
        longestMs,
        waitMs
    );
    return true;
}

bool Recorder::writeInitialSnapshotIfNeeded() {
    if (!mNeedsInitialSnapshot.load()) return true;

    auto                                captureStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration barrierWait{};
    bool                                captured = captureChunkSnapshot(barrierWait);
    auto                                elapsed  = std::chrono::steady_clock::now() - captureStart;

    if (!captured) {
        getLogger().error(
            "Initial snapshot capture failed after {:.3f} ms: {}",
            std::chrono::duration<double, std::milli>(elapsed).count(),
            mSnapshotFailure
        );
        failRecording(
            mSnapshotFailure.empty() ? "Unable to prepare the initial replay chunk snapshot" : mSnapshotFailure
        );
        return false;
    }

    if (!writeSnapshot()) return false;
    return commitChunkSnapshot(elapsed, barrierWait);
}

bool Recorder::writeSnapshot() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a snapshot");
        return false;
    }

    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.startSnapshot(); })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot header"));
        return false;
    }

    std::vector<std::shared_ptr<Packet>> gamePackets;
    gamePackets.reserve(mSnapshotLevelChunks.size() + mSnapshotSubChunks.size());
    for (auto const& packet : mSnapshotLevelChunks) {
        gamePackets.emplace_back(packet);
    }
    for (auto const& packet : mSnapshotSubChunks) {
        gamePackets.emplace_back(packet);
    }

    if (!mAsyncReplaySaver->writeGamePackets(std::move(gamePackets))) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue generated snapshot packets"));
        return false;
    }
    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.endSnapshot(); })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot footer"));
        return false;
    }
    return true;
}

bool Recorder::writeTickBoundary() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a tick boundary");
        return false;
    }

    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) {
            writer.startAndFinishAction(ActionNextTick::getInstance());
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay tick boundary"));
        return false;
    }
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;

    ++mTicksInCurrentChunk;
    ++mWrittenTicks;
    return true;
}

bool Recorder::finishCurrentChunk(bool close) {
    if (!mHasOpenChunk || !mOpenChunkHasData) return true;
    if (!mOpenChunkView) {
        failRecording("Replay chunk has no snapshot playback view");
        return false;
    }

    std::string chunkName = "chunk_" + std::to_string(mChunkIndex) + ".bin";

    PlaybackMeta chunkMeta;
    chunkMeta.name        = chunkName;
    chunkMeta.worldName   = mMetadata.worldName;
    chunkMeta.duration    = mTicksInCurrentChunk;
    chunkMeta.initialView = mOpenChunkView;
    mMetadata.chunks.insert_or_assign(chunkName, chunkMeta);
    mMetadata.totalTicks = mWrittenTicks;

    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while finishing a replay chunk");
        return false;
    }

    if (!mAsyncReplaySaver->writeReplayChunk(chunkName, mMetadata.toJson())) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay chunk for writing"));
        return false;
    }

    ++mChunkIndex;
    mTicksInCurrentChunk = 0;
    mHasOpenChunk        = false;
    mOpenChunkHasData    = false;
    mOpenChunkView.reset();

    if (!close) {
        mNeedsInitialSnapshot = true;
    }
    return true;
}

void Recorder::failRecording(std::string_view reason) { cancelRecording(reason); }

void Recorder::cancelRecording(std::string_view reason) {
    getLogger().error("Recording cancelled: {}", reason);
    if (mAsyncReplaySaver) {
        mAsyncReplaySaver->cancel();
        mAsyncReplaySaver.reset();
    }

    mState                = State::Idle;
    mNeedsInitialSnapshot = true;
    resetChunkSnapshot();
}

} // namespace playback::functions

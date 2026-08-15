#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace playback::replay {
class ReplaySession;
}
namespace playback::io {
class PlaybackBuffer;
}

namespace playback::action {
using playback::replay::ReplaySession;
using playback::io::PlaybackBuffer;

struct Action {
    std::string name;

    explicit Action(std::string name) : name(std::move(name)) {}
    virtual ~Action() = default;

    virtual void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) = 0;
};

class ActionRegistry {
private:
    std::vector<std::unique_ptr<Action>>     mActions;
    std::unordered_map<std::string, Action*> mNameToAction;

public:
    void                                                      registerAction(std::unique_ptr<Action> action);
    [[nodiscard]] Action*                                     getAction(std::string& name) const;
    [[nodiscard]] std::vector<std::unique_ptr<Action>> const& getActions() const { return mActions; };

private:
    ~ActionRegistry() = default;

public:
    [[nodiscard]] static ActionRegistry& getInstance() {
        static ActionRegistry instance;
        return instance;
    }
};

struct ActionNextTick : Action {
    ActionNextTick() : Action("next_tick") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionNextTick& getInstance() {
        static ActionNextTick instance;
        return instance;
    }
};

struct ActionSnapshotContext : Action {
    ActionSnapshotContext() : Action("snapshot_context") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionSnapshotContext& getInstance() {
        static ActionSnapshotContext instance;
        return instance;
    }
};

struct ActionCreateLocalPlayer : Action {
    ActionCreateLocalPlayer() : Action("create_local_player") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionCreateLocalPlayer& getInstance() {
        static ActionCreateLocalPlayer instance;
        return instance;
    }
};

struct ActionLevelChunkCached : Action {
    ActionLevelChunkCached() : Action("level_chunk_cached") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionLevelChunkCached& getInstance() {
        static ActionLevelChunkCached instance;
        return instance;
    }
};

struct ActionSubChunkCached : Action {
    ActionSubChunkCached() : Action("sub_chunk_cached") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionSubChunkCached& getInstance() {
        static ActionSubChunkCached instance;
        return instance;
    }
};

struct ActionConfigurationPacket : Action {
    ActionConfigurationPacket() : Action("configuration_packet") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionConfigurationPacket& getInstance() {
        static ActionConfigurationPacket instance;
        return instance;
    }
};

struct ActionGamePacket : Action {
    ActionGamePacket() : Action("game_packet") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionGamePacket& getInstance() {
        static ActionGamePacket instance;
        return instance;
    }
};

struct ActionMoveEntities : Action {
    ActionMoveEntities() : Action("move_entities") {}
    void handle(replay::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionMoveEntities& getInstance() {
        static ActionMoveEntities instance;
        return instance;
    }
};

} // namespace playback::action

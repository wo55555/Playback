#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace playback::functions {

class ReplaySession;
class PlaybackBuffer;

struct Action {
    std::string name;

    explicit Action(std::string name) : name(std::move(name)) {}
    virtual ~Action() = default;

    virtual void handle(functions::ReplaySession& replaySession, PlaybackBuffer& data) = 0;
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
    void handle(functions::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionNextTick& getInstance() {
        static ActionNextTick instance;
        return instance;
    }
};

struct ActionLevelChunkCached : Action {
    ActionLevelChunkCached() : Action("level_chunk_cached") {}
    void handle(functions::ReplaySession& replaySession, PlaybackBuffer& data) override;

public:
    [[nodiscard]] static ActionLevelChunkCached& getInstance() {
        static ActionLevelChunkCached instance;
        return instance;
    }
};

} // namespace playback::functions

#pragma once

#include "mc/deps/core/utility/BinaryStream.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace playback::functions {

class ReplaySession;

struct Action {
public:
    std::string name;

    explicit Action(std::string name) : name(std::move(name)) {}
    virtual ~Action() = default;

    virtual void handle(functions::ReplaySession& replaySession, BinaryStream& data) = 0;
};

class ActionRegistry {
private:
    std::vector<std::unique_ptr<Action>>     mActions;
    std::unordered_map<std::string, Action*> mNameToAction;

public:
    void                                                      registerAction(std::unique_ptr<Action> action);
    [[nodiscard]] Action*                                     getAction(std::string name) const;
    [[nodiscard]] std::vector<std::unique_ptr<Action>> const& getActions() const { return mActions; };

private:
    ~ActionRegistry() = default;

public:
    [[nodiscard]] static ActionRegistry& getInstance() {
        static ActionRegistry instance;
        return instance;
    }
};

class ActionNextTick : Action {
public:
    ActionNextTick() : Action("next_tick") {}
    void handle(functions::ReplaySession& replaySession, BinaryStream& data) override;
};

} // namespace playback::functions

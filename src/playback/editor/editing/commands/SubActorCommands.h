#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {
class SetSubActorDetails final : public model::IEditCommand {
public:
    SetSubActorDetails(std::string subActorId, model::AgentDetails details);
    void execute(model::EditorStateExt& state) override;
    void undo(model::EditorStateExt& state) override;
    std::string label() const override;
private:
    std::string mSubActorId;
    model::AgentDetails mDetails;
    std::optional<model::EditorStateExt> mBefore;
};
}

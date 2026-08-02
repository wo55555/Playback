#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::refactor::video_editing {
class SetSubActorDetails final : public editor::IEditCommand {
public:
    SetSubActorDetails(std::string subActorId, editor::AgentDetails details);
    void execute(editor::EditorStateExt& state) override;
    void undo(editor::EditorStateExt& state) override;
    std::string label() const override;
private:
    std::string mSubActorId;
    editor::AgentDetails mDetails;
    std::optional<editor::EditorStateExt> mBefore;
};
}

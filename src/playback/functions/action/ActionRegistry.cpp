#include "Action.h"
#include <string>

namespace playback::functions {

void ActionRegistry::registerAction(std::unique_ptr<Action> action) {
    if (!action) return;

    std::string name   = action->name;
    Action*     rawPtr = action.get();

    auto [it, inserted] = mNameToAction.try_emplace(name, rawPtr);
    if (!inserted) return;

    mActions.push_back(std::move(action));
}

Action* ActionRegistry::getAction(std::string name) const {
    auto it = mNameToAction.find(name);
    return (it != mNameToAction.end()) ? it->second : nullptr;
}

} // namespace playback::functions

#pragma once

#include <string>
#include <vector>

namespace playback::refactor::video_editing {

struct Selection {
    std::vector<std::string> clipIds;
    std::vector<std::string> trackIds;
    std::vector<std::string> keyframeIds;
    std::vector<std::string> transitionIds;
    int anchorTick{0};
};

class SelectionModel {
public:
    void select(const std::string& id, bool additive = false);
    void clear();
    [[nodiscard]] bool isSelected(const std::string& id) const;
    [[nodiscard]] Selection snapshot() const;
    void setAnchor(int tick);
    [[nodiscard]] bool hasSelection() const;

private:
    Selection mSelection;
};

} // namespace playback::refactor::video_editing
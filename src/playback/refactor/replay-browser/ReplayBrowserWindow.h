#pragma once

#include "playback/screen/ReplayBrowser.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace playback::refactor::replay_browser {

class ReplayBrowserWindow {
public:
    static ReplayBrowserWindow& getInstance();

    void open();
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool ownsInput() const;
    void draw();

private:
    void refresh();
    void drawNavigation();
    void drawContent();
    void drawCard(screen::ReplaySummary const& replay, std::size_t index, float width);
    void drawActionBar();
    [[nodiscard]] std::optional<screen::ReplaySummary const*> selectedReplay() const;

    bool                                mOpen{};
    std::vector<screen::ReplaySummary>  mReplays;
    std::optional<std::size_t>           mSelectedIndex;
    std::string                         mSearch;
};

} // namespace playback::refactor::replay_browser

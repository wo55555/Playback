#include "PlayerPicker.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace playback::editor::ui {
using namespace ll::i18n_literals;

namespace {

struct PickerUi {
    char search[128]{};
    int  highlight{-1};
    bool absentOpen{false};
};

std::unordered_map<ImGuiID, PickerUi>& pickerUi() {
    static std::unordered_map<ImGuiID, PickerUi> states;
    return states;
}

struct SearchEdit {
    int move{0};
};

int searchCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    auto* edit = static_cast<SearchEdit*>(data->UserData);
    edit->move = data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
    return 0;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// ASCII folds to lower case. UTF-8 bytes stay as they are, so CJK still matches exactly.
[[nodiscard]] std::string foldAscii(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (unsigned char const character : text) {
        folded.push_back(character < 128 ? static_cast<char>(std::tolower(character)) : static_cast<char>(character));
    }
    return folded;
}

[[nodiscard]] bool containsFolded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return foldAscii(haystack).find(foldAscii(needle)) != std::string::npos;
}

[[nodiscard]] std::string uuidPrefix(std::string_view uuid) {
    return std::string(uuid.substr(0, std::min<size_t>(8, uuid.size())));
}

[[nodiscard]] std::string rawName(state::ReplayPlayerView const& player) {
    if (player.name.empty() || player.name == player.uuid) return "playback.refactorEditor.playerPicker.unnamed"_tr();
    return player.name;
}

[[nodiscard]] bool nameIsDuplicated(std::span<state::ReplayPlayerView const> players, std::string const& name) {
    int count = 0;
    for (auto const& player : players) {
        if (rawName(player) == name) ++count;
    }
    return count > 1;
}

struct Row {
    enum class Kind { Recorder, Player, Header, Note };

    Kind        kind{Kind::Player};
    std::string uuid;
    std::string label;
    std::string hint;
    bool        selectable{false};
    bool        absentHeader{false};
};

[[nodiscard]] std::vector<Row> buildRows(
    std::span<state::ReplayPlayerView const> players,
    std::string_view                          query,
    bool                                      absentOpen
) {
    std::vector<Row> rows;
    state::ReplayPlayerView const* recorder = nullptr;
    for (auto const& player : players) {
        if (!player.recorder) continue;
        recorder = &player;
        break;
    }

    auto const matches = [&](std::string_view name, std::string_view uuid) {
        return containsFolded(name, query) || containsFolded(uuid, query);
    };

    std::string const recorderLabel = recorder ? rawName(*recorder) : "playback.refactorEditor.playerPicker.recorder"_tr();
    std::string const recorderUuid  = recorder ? recorder->uuid : std::string{};
    if (matches(recorderLabel, recorderUuid) || containsFolded("playback.refactorEditor.playerPicker.recorder"_tr(), query)) {
        Row row;
        row.kind       = Row::Kind::Recorder;
        row.label      = recorder ? rawName(*recorder) : recorderLabel;
        row.selectable = true;
        if (recorder && (recorder->name.empty() || recorder->name == recorder->uuid || nameIsDuplicated(players, row.label))) {
            row.hint = uuidPrefix(recorder->uuid);
        }
        if (!recorder || recorder->name.empty() || recorder->name == recorder->uuid) {
            row.label = "playback.refactorEditor.playerPicker.recorder"_tr();
            row.hint.clear();
        }
        rows.push_back(std::move(row));
    }

    std::vector<state::ReplayPlayerView const*> present;
    std::vector<state::ReplayPlayerView const*> absent;
    for (auto const& player : players) {
        if (player.recorder) continue;
        if (player.present) present.push_back(&player);
        else absent.push_back(&player);
    }
    auto const byName = [](state::ReplayPlayerView const* left, state::ReplayPlayerView const* right) {
        return foldAscii(rawName(*left)) < foldAscii(rawName(*right));
    };
    std::ranges::sort(present, byName);
    std::ranges::sort(absent, byName);

    auto const appendPlayers = [&](std::vector<state::ReplayPlayerView const*> const& group, bool selectable) {
        for (auto const* player : group) {
            std::string const label = rawName(*player);
            if (!matches(label, player->uuid)) continue;
            Row row;
            row.kind       = Row::Kind::Player;
            row.uuid       = player->uuid;
            row.label      = label;
            row.selectable = selectable;
            if (player->name.empty() || player->name == player->uuid || nameIsDuplicated(players, label)) {
                row.hint = uuidPrefix(player->uuid);
            }
            rows.push_back(std::move(row));
        }
    };

    int const presentMatches = static_cast<int>(std::ranges::count_if(present, [&](auto const* player) {
        return matches(rawName(*player), player->uuid);
    }));
    if (presentMatches > 0) {
        Row header;
        header.kind  = Row::Kind::Header;
        header.label = "playback.refactorEditor.playerPicker.present"_tr() + "  " + std::to_string(presentMatches);
        rows.push_back(std::move(header));
        appendPlayers(present, true);
    } else if (query.empty()) {
        int presentTotal = 0;
        for (auto const& player : players) {
            if (!player.recorder && player.present) ++presentTotal;
        }
        if (presentTotal == 0) {
            Row note;
            note.kind  = Row::Kind::Note;
            note.label = "playback.refactorEditor.playerPicker.noPlayers"_tr();
            rows.push_back(std::move(note));
        }
    }

    int const absentMatches = static_cast<int>(std::ranges::count_if(absent, [&](auto const* player) {
        return matches(rawName(*player), player->uuid);
    }));
    if (absentMatches > 0) {
        Row header;
        header.kind         = Row::Kind::Header;
        header.absentHeader = true;
        header.label        = "playback.refactorEditor.playerPicker.absent"_tr() + "  " + std::to_string(absentMatches);
        rows.push_back(std::move(header));
        if (absentOpen || !query.empty()) appendPlayers(absent, false);
    }

    if (!query.empty() && std::ranges::none_of(rows, [](Row const& row) {
            return row.kind == Row::Kind::Recorder || row.kind == Row::Kind::Player;
        })) {
        rows.clear();
        Row note;
        note.kind  = Row::Kind::Note;
        note.label = "playback.refactorEditor.playerPicker.noMatch"_tr();
        rows.push_back(std::move(note));
    }
    return rows;
}

[[nodiscard]] int selectableIndex(std::vector<Row> const& rows, int highlight) {
    int seen = -1;
    for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
        if (!rows[static_cast<size_t>(index)].selectable) continue;
        ++seen;
        if (seen == highlight) return index;
    }
    return -1;
}

[[nodiscard]] int selectableCount(std::vector<Row> const& rows) {
    return static_cast<int>(std::ranges::count_if(rows, [](Row const& row) { return row.selectable; }));
}

void moveHighlight(std::vector<Row> const& rows, int& highlight, int direction) {
    int const count = selectableCount(rows);
    if (count <= 0) {
        highlight = -1;
        return;
    }
    if (highlight < 0) highlight = direction < 0 ? count - 1 : 0;
    else highlight = std::clamp(highlight + direction, 0, count - 1);
}

[[nodiscard]] bool rowIsCurrent(Row const& row, std::string_view current, std::string_view recorderUuid) {
    if (row.kind == Row::Kind::Recorder) return current.empty() || (!recorderUuid.empty() && current == recorderUuid);
    return !row.uuid.empty() && current == row.uuid;
}

} // namespace

std::string playerSubjectLabel(std::span<state::ReplayPlayerView const> players, std::string_view uuid) {
    if (uuid.empty()) return "playback.refactorEditor.playerPicker.recorder"_tr();
    for (auto const& player : players) {
        if (player.uuid != uuid) continue;
        std::string label = rawName(player);
        if (player.name.empty() || player.name == player.uuid || nameIsDuplicated(players, label)) {
            label += "  " + uuidPrefix(player.uuid);
        }
        return label;
    }
    return "playback.refactorEditor.playerPicker.unnamed"_tr() + "  " + uuidPrefix(uuid);
}

bool drawPlayerPicker(
    char const*                              popupId,
    PlayerPickerMode                         mode,
    std::span<state::ReplayPlayerView const> players,
    std::string_view                         current,
    bool                                     following,
    PlayerPickerResult&                      result
) {
    result           = {};
    ImGuiID const id = ImGui::GetID(popupId);
    auto&         ui = pickerUi()[id];

    float const font  = metrics::font();
    float const scale = metrics::scale();
    float const width = std::clamp(widgets::anchoredPopupTriggerWidth(popupId), font * 20.0f, font * 28.0f);
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, FLT_MAX});
    if (!widgets::beginAnchoredPopup(popupId)) {
        pickerUi().erase(id);
        return false;
    }

    bool const appearing = ImGui::IsWindowAppearing();
    if (appearing) {
        ui.search[0]  = '\0';
        ui.highlight  = -1;
        ImGui::SetKeyboardFocusHere();
    }

    float const rowHeight = metrics::subRow();
    float const inner     = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        origin,
        {origin.x + inner, origin.y + rowHeight},
        theme::kInputBg,
        theme::kFrameRounding
    );

    float const iconBox = metrics::iconButton();
    widgets::drawIconAtCentre(
        drawList,
        ICON_SEARCH,
        {origin.x + iconBox * 0.5f, origin.y + rowHeight * 0.5f},
        theme::kTextDim
    );

    bool const        hasQuery = ui.search[0] != '\0';
    float const       closeBox = hasQuery ? iconBox : 0.0f;
    SearchEdit        edit;
    ImGui::SetCursorScreenPos({origin.x + iconBox, origin.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f});
    ImGui::SetNextItemWidth(std::max(1.0f, inner - iconBox - closeBox));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    bool const submitted = ImGui::InputTextWithHint(
        "##player-picker-search",
        "playback.refactorEditor.playerPicker.searchHint"_tr().c_str(),
        ui.search,
        sizeof(ui.search),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
            | ImGuiInputTextFlags_EscapeClearsAll,
        searchCallback,
        &edit
    );
    ImGui::PopStyleColor(3);
    bool const escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (hasQuery && escape) {
        // EscapeClearsAll already emptied the buffer. Swallow this press so the popup stays open.
    } else if (!hasQuery && escape) {
        ImGui::CloseCurrentPopup();
    }

    if (hasQuery) {
        ImGui::SetCursorScreenPos({origin.x + inner - iconBox, origin.y});
        if (ImGui::InvisibleButton("##player-picker-clear", {iconBox, rowHeight})) {
            ui.search[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }
        widgets::drawIconAtCentre(
            drawList,
            ICON_CLOSE,
            {origin.x + inner - iconBox * 0.5f, origin.y + rowHeight * 0.5f},
            ImGui::IsItemHovered() ? theme::kIconHighlight : theme::kTextDim
        );
    }
    ImGui::SetCursorScreenPos({origin.x, origin.y + rowHeight + 4.0f * scale});

    std::string_view const query = trim(ui.search);
    bool const             showAbsent = ui.absentOpen || !query.empty();
    auto                   rows       = buildRows(players, query, showAbsent);
    std::string            recorderUuid;
    for (auto const& player : players) {
        if (!player.recorder) continue;
        recorderUuid = player.uuid;
        break;
    }
    if (appearing) {
        int seen = -1;
        for (auto const& row : rows) {
            if (!row.selectable) continue;
            ++seen;
            if (rowIsCurrent(row, current, recorderUuid)) {
                ui.highlight = seen;
                break;
            }
        }
    }
    int const choices = selectableCount(rows);
    if (ui.highlight >= choices) ui.highlight = choices - 1;
    if (edit.move != 0) moveHighlight(rows, ui.highlight, edit.move);
    int const highlightedRow = selectableIndex(rows, ui.highlight);

    bool currentMissing = !current.empty() && current != recorderUuid;
    if (currentMissing) {
        for (auto const& player : players) {
            if (player.uuid == current) currentMissing = false;
        }
    }

    float const listHeight = std::min(rowHeight * 9.0f, rowHeight * static_cast<float>(std::max<size_t>(1, rows.size() + (currentMissing ? 1 : 0))));
    ImGui::BeginChild("##player-picker-list", {inner, std::max(rowHeight, listHeight)}, false, ImGuiWindowFlags_None);
    auto* listDraw = ImGui::GetWindowDrawList();

    auto const drawRow = [&](Row const& row, int index, bool pinned) {
        ImVec2 const minimum = ImGui::GetCursorScreenPos();
        ImVec2 const maximum{minimum.x + inner, minimum.y + rowHeight};
        ImGui::PushID(index);
        bool const pressed = ImGui::InvisibleButton("##player-row", {inner, rowHeight});
        bool const hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        bool const selected = !pinned && index == highlightedRow;
        bool const active   = !pinned && rowIsCurrent(row, current, recorderUuid);
        if (selected || (hovered && row.selectable)) {
            listDraw->AddRectFilled(minimum, maximum, theme::kRowHighlight);
        }
        if (active) listDraw->AddRectFilled(minimum, {minimum.x + 3.0f * scale, maximum.y}, theme::kAccent);
        ImU32 const color = row.selectable ? theme::kText : theme::kTextDim;
        float const textY = widgets::textYForCentre(minimum.y + rowHeight * 0.5f, row.label.c_str());
        float       textX = minimum.x + 8.0f * scale;
        listDraw->PushClipRect(minimum, maximum, true);
        listDraw->AddText({textX, textY}, color, row.label.c_str());
        textX += ImGui::CalcTextSize(row.label.c_str()).x + 8.0f * scale;
        if (!row.hint.empty()) listDraw->AddText({textX, textY}, theme::kTextDim, row.hint.c_str());
        if (active) {
            widgets::drawIconAtCentre(
                listDraw,
                ICON_CHECK,
                {maximum.x - iconBox * 0.5f, minimum.y + rowHeight * 0.5f},
                theme::kAccent
            );
        }
        listDraw->PopClipRect();
        if (!pinned && !row.selectable && hovered) {
            widgets::hoverTooltip("playback.refactorEditor.playerPicker.absentHint"_tr().c_str());
        }
        if (edit.move != 0 && selected) ImGui::SetScrollHereY(0.5f);
        if (!pinned && pressed && row.selectable) {
            result.picked = true;
            result.uuid   = row.uuid;
        }
        if (pressed && row.absentHeader) ui.absentOpen = !ui.absentOpen;
    };

    if (currentMissing) {
        Row pinned;
        pinned.kind  = Row::Kind::Note;
        pinned.label = playerSubjectLabel(players, current);
        drawRow(pinned, -1, true);
    }
    for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
        Row const& row = rows[static_cast<size_t>(index)];
        if (row.kind == Row::Kind::Header) {
            ImGui::PushID(index);
            std::string header = row.label;
            if (row.absentHeader) header = std::string(showAbsent ? "-  " : "+  ") + header;
            if (ImGui::Selectable(header.c_str(), false, row.absentHeader ? 0 : ImGuiSelectableFlags_Disabled)) {
                if (row.absentHeader && query.empty()) ui.absentOpen = !ui.absentOpen;
            }
            ImGui::PopID();
            continue;
        }
        if (row.kind == Row::Kind::Note) {
            float const noteY = widgets::textYForCentre(ImGui::GetCursorScreenPos().y + rowHeight * 0.5f, row.label.c_str());
            float const noteX = ImGui::GetCursorScreenPos().x + std::max(0.0f, (inner - ImGui::CalcTextSize(row.label.c_str()).x) * 0.5f);
            ImGui::Dummy({inner, rowHeight});
            listDraw->AddText({noteX, noteY}, theme::kTextDim, row.label.c_str());
            continue;
        }
        drawRow(row, index, false);
    }
    ImGui::EndChild();

    if (mode == PlayerPickerMode::Follow && following) {
        ImGui::Separator();
        if (ImGui::Selectable("playback.refactorEditor.playerPicker.stopFollow"_tr().c_str(), false)) {
            result.stop = true;
        }
    }

    if (submitted && !result.picked && highlightedRow >= 0) {
        result.picked = true;
        result.uuid   = rows[static_cast<size_t>(highlightedRow)].uuid;
    }
    if (result.picked || result.stop) ImGui::CloseCurrentPopup();

    widgets::endAnchoredPopup();
    return result.picked || result.stop;
}

} // namespace playback::editor::ui

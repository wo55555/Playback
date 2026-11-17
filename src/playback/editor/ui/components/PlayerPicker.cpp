#include "PlayerPicker.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/replay/ReplaySession.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace playback::editor::ui {
using namespace ll::i18n_literals;

namespace {

constexpr int kEntityDisplayLimit = 100;
constexpr int kEntityMeasureLimit = 30;

struct PickerUi {
    char search[128]{};
    int  highlight{-1};
    bool entitiesOpen{true};
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

[[nodiscard]] std::string entityIdTag(std::string_view choice) {
    if (choice.starts_with(replay::ReplaySession::EntityChoicePrefix)) {
        return "#" + std::string(choice.substr(replay::ReplaySession::EntityChoicePrefix.size()));
    }
    return std::string(choice);
}

[[nodiscard]] std::string formatDistance(float distance) {
    if (!std::isfinite(distance) || distance < 0.0f) return {};
    return std::to_string(static_cast<long long>(std::llround(distance))) + "m";
}

struct Row {
    enum class Kind { Recorder, Player, Entity, Header, Note };

    Kind        kind{Kind::Player};
    std::string uuid;
    std::string label;
    std::string hint;
    bool        selectable{false};
    bool        entitiesHeader{false};
    bool        absentHeader{false};
};

[[nodiscard]] std::vector<Row> buildRows(
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities,
    std::string_view                         query,
    bool                                     entitiesOpen,
    bool                                     absentOpen,
    size_t                                   entityLimit = kEntityDisplayLimit
) {
    std::vector<Row> rows;
    state::ReplayPlayerView const* recorder = nullptr;
    for (auto const& player : players) {
        if (!player.recorder) continue;
        recorder = &player;
        break;
    }

    auto const matches = [&](std::string_view name, std::string_view id) {
        return containsFolded(name, query) || containsFolded(id, query);
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
    } else if (query.empty() && entities.empty()) {
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

    std::unordered_map<std::string_view, int> entityLabelCounts;
    entityLabelCounts.reserve(entities.size());
    std::vector<state::ReplayEntityView const*> matchingEntities;
    matchingEntities.reserve(entities.size());
    for (auto const& entity : entities) {
        ++entityLabelCounts[entity.label];
        if (matches(entity.label, entity.choice) || containsFolded(entityIdTag(entity.choice), query)) {
            matchingEntities.push_back(&entity);
        }
    }
    int const entityMatches = static_cast<int>(matchingEntities.size());
    if (entityMatches > 0) {
        Row header;
        header.kind           = Row::Kind::Header;
        header.entitiesHeader = true;
        header.label =
            "playback.refactorEditor.playerPicker.nearbyEntities"_tr() + "  " + std::to_string(entityMatches);
        rows.push_back(std::move(header));
        if (entitiesOpen || !query.empty()) {
            size_t const shown = std::min(matchingEntities.size(), entityLimit);
            for (size_t index = 0; index < shown; ++index) {
                auto const* entity = matchingEntities[index];
                Row         row;
                row.kind         = Row::Kind::Entity;
                row.uuid         = entity->choice;
                row.label        = entity->label;
                row.selectable   = true;
                std::string hint = formatDistance(entity->distance);
                if (entityLabelCounts[entity->label] > 1) {
                    if (!hint.empty()) hint += "  ";
                    hint += entityIdTag(entity->choice);
                }
                row.hint = std::move(hint);
                rows.push_back(std::move(row));
            }
            if (matchingEntities.size() > shown) {
                Row note;
                note.kind  = Row::Kind::Note;
                note.label = "playback.refactorEditor.playerPicker.moreEntities"_tr(matchingEntities.size() - shown);
                rows.push_back(std::move(note));
            }
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
            return row.kind == Row::Kind::Recorder || row.kind == Row::Kind::Player || row.kind == Row::Kind::Entity;
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

std::string playerSubjectLabel(
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities,
    std::string_view                         uuid
) {
    if (uuid.empty()) return "playback.refactorEditor.playerPicker.recorder"_tr();
    // A mob picked in the viewport or list; fall back to the session cache or id when currently unloaded.
    if (replay::ReplaySession::parseEntityChoice(uuid)) {
        for (auto const& entity : entities) {
            if (entity.choice == uuid) return entity.label;
        }
        return replay::ReplaySession::getInstance().replaySubjectLabel(std::string{uuid});
    }
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

namespace {

constexpr int kPopupRows    = 9;
constexpr int kEmbeddedRows = 10;

// Width that fits the longest row, so the popup grows to the names instead of the trigger.
[[nodiscard]] float preferredContentWidth(
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities
) {
    float const scale   = metrics::scale();
    float const iconBox = metrics::iconButton();
    float       widest =
        ImGui::CalcTextSize("playback.refactorEditor.playerPicker.searchHint"_tr().c_str()).x + iconBox * 2.0f;
    auto const rows = buildRows(players, entities, {}, true, true, kEntityMeasureLimit);
    for (auto const& row : rows) {
        float width = ImGui::CalcTextSize(row.label.c_str()).x + 16.0f * scale + iconBox;
        if (!row.hint.empty()) width += 8.0f * scale + ImGui::CalcTextSize(row.hint.c_str()).x;
        widest = std::max(widest, width);
    }
    if (static_cast<int>(rows.size()) > kPopupRows) widest += ImGui::GetStyle().ScrollbarSize;
    return widest;
}

struct ListOptions {
    PlayerPickerMode mode{PlayerPickerMode::Follow};
    bool             following{};
    bool             popup{};
    // Resets the highlight to the current subject and puts the caret in the search box.
    bool             appearing{};
    bool             focusSearch{};
    bool             mixed{};
    int              maxRows{kPopupRows};
};

// Search row plus list. Shared by the anchored popup and the inspector's embedded list.
void drawListBody(
    PickerUi&                                ui,
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities,
    std::string_view                         current,
    ListOptions const&                       options,
    PlayerPickerResult&                      result
) {
    float const scale     = metrics::scale();
    bool const  appearing = options.appearing;
    if (appearing) {
        ui.highlight = -1;
        if (options.popup) ui.search[0] = '\0';
    }
    // An inspector list only takes the keyboard when asked, so editor shortcuts keep working.
    if ((options.popup && appearing) || options.focusSearch) ImGui::SetKeyboardFocusHere();

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
    bool const escape = options.popup && ImGui::IsKeyPressed(ImGuiKey_Escape);
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

    std::string_view const query        = trim(ui.search);
    bool const             showEntities = ui.entitiesOpen || !query.empty();
    bool const             showAbsent   = ui.absentOpen || !query.empty();
    auto                   rows         = buildRows(players, entities, query, showEntities, showAbsent);
    std::string            recorderUuid;
    for (auto const& player : players) {
        if (!player.recorder) continue;
        recorderUuid = player.uuid;
        break;
    }
    if (appearing && !options.mixed) {
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

    bool currentMissing = !options.mixed && !current.empty() && current != recorderUuid;
    if (currentMissing) {
        for (auto const& player : players) {
            if (player.uuid == current) {
                currentMissing = false;
                break;
            }
        }
    }
    if (currentMissing) {
        for (auto const& entity : entities) {
            if (entity.choice == current) {
                currentMissing = false;
                break;
            }
        }
    }

    size_t const visibleRows = std::max<size_t>(1, rows.size() + (currentMissing ? 1 : 0));
    float const  listHeight =
        rowHeight * static_cast<float>(std::min<size_t>(static_cast<size_t>(options.maxRows), visibleRows));
    ImGui::BeginChild("##player-picker-list", {inner, std::max(rowHeight, listHeight)}, false, ImGuiWindowFlags_None);
    auto*       listDraw = ImGui::GetWindowDrawList();
    // The child's own width, so rows stop short of its scrollbar.
    float const rowWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);

    auto const drawRow = [&](Row const& row, int index, bool pinned) {
        ImVec2 const minimum = ImGui::GetCursorScreenPos();
        ImVec2 const maximum{minimum.x + rowWidth, minimum.y + rowHeight};
        ImGui::PushID(index);
        bool const pressed = ImGui::InvisibleButton("##player-row", {rowWidth, rowHeight});
        bool const hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        bool const selected = !pinned && index == highlightedRow;
        bool const active   = !pinned && !options.mixed && rowIsCurrent(row, current, recorderUuid);
        if (selected || (hovered && row.selectable)) {
            listDraw->AddRectFilled(minimum, maximum, theme::kRowHighlight);
        }
        if (active) listDraw->AddRectFilled(minimum, {minimum.x + 3.0f * scale, maximum.y}, theme::kAccent);
        ImU32 const color = row.selectable ? theme::kText : theme::kTextDim;
        float const textY = widgets::textYForCentre(minimum.y + rowHeight * 0.5f, row.label.c_str());
        float       textX = minimum.x + 8.0f * scale;
        // Text stops before the check mark; a name that still does not fit shows in full on hover.
        float const textRight = maximum.x - iconBox;
        float const needed    = ImGui::CalcTextSize(row.label.c_str()).x
                           + (row.hint.empty() ? 0.0f : 8.0f * scale + ImGui::CalcTextSize(row.hint.c_str()).x);
        bool const  clipped   = textX + needed > textRight;
        listDraw->PushClipRect(minimum, {textRight, maximum.y}, true);
        listDraw->AddText({textX, textY}, color, row.label.c_str());
        textX += ImGui::CalcTextSize(row.label.c_str()).x + 8.0f * scale;
        if (!row.hint.empty()) listDraw->AddText({textX, textY}, theme::kTextDim, row.hint.c_str());
        listDraw->PopClipRect();
        if (active) {
            widgets::drawIconAtCentre(
                listDraw,
                ICON_CHECK,
                {maximum.x - iconBox * 0.5f, minimum.y + rowHeight * 0.5f},
                theme::kAccent
            );
        }
        if (hovered && (clipped || (!pinned && !row.selectable))) {
            std::string tip = row.hint.empty() ? row.label : row.label + "  " + row.hint;
            if (!pinned && !row.selectable) tip += "\n" + "playback.refactorEditor.playerPicker.absentHint"_tr();
            widgets::hoverTooltip(tip.c_str());
        }
        if (edit.move != 0 && selected) ImGui::SetScrollHereY(0.5f);
        if (!pinned && pressed && row.selectable) {
            result.picked = true;
            result.uuid   = row.uuid;
        }
    };

    if (currentMissing) {
        Row pinned;
        pinned.kind  = Row::Kind::Note;
        pinned.label = playerSubjectLabel(players, entities, current);
        drawRow(pinned, -1, true);
    }
    for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
        Row const& row = rows[static_cast<size_t>(index)];
        if (row.kind == Row::Kind::Header) {
            ImGui::PushID(index);
            bool const  collapsible = row.entitiesHeader || row.absentHeader;
            bool const  open        = row.entitiesHeader ? showEntities : showAbsent;
            std::string header      = row.label;
            if (collapsible) header = std::string(open ? "-  " : "+  ") + header;
            if (ImGui::Selectable(header.c_str(), false, collapsible ? 0 : ImGuiSelectableFlags_Disabled)) {
                if (row.entitiesHeader && query.empty()) ui.entitiesOpen = !ui.entitiesOpen;
                if (row.absentHeader && query.empty()) ui.absentOpen = !ui.absentOpen;
            }
            ImGui::PopID();
            continue;
        }
        if (row.kind == Row::Kind::Note) {
            float const noteY = widgets::textYForCentre(ImGui::GetCursorScreenPos().y + rowHeight * 0.5f, row.label.c_str());
            float const noteX = ImGui::GetCursorScreenPos().x
                              + std::max(0.0f, (rowWidth - ImGui::CalcTextSize(row.label.c_str()).x) * 0.5f);
            ImGui::Dummy({rowWidth, rowHeight});
            listDraw->AddText({noteX, noteY}, theme::kTextDim, row.label.c_str());
            continue;
        }
        drawRow(row, index, false);
    }
    ImGui::EndChild();

    if (options.mode == PlayerPickerMode::Follow && options.following) {
        ImGui::Separator();
        if (ImGui::Selectable("playback.refactorEditor.playerPicker.stopFollow"_tr().c_str(), false)) {
            result.stop = true;
        }
    }

    if (submitted && !result.picked && highlightedRow >= 0) {
        result.picked = true;
        result.uuid   = rows[static_cast<size_t>(highlightedRow)].uuid;
    }
}

} // namespace

bool drawPlayerPicker(
    char const*                              popupId,
    PlayerPickerMode                         mode,
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities,
    std::string_view                         current,
    bool                                     following,
    PlayerPickerResult&                      result
) {
    result           = {};
    ImGuiID const id = ImGui::GetID(popupId);

    // Wide enough for the longest name, never narrower than the trigger, capped to a third of the screen.
    float const font     = metrics::font();
    float const viewport = ImGui::GetMainViewport()->WorkSize.x;
    float const padding  = ImGui::GetStyle().WindowPadding.x * 2.0f + 20.0f * metrics::scale();
    float const ceiling  = std::max(font * 22.0f, viewport * 0.34f);
    float const width    = std::clamp(
        std::max(widgets::anchoredPopupTriggerWidth(popupId), preferredContentWidth(players, entities) + padding),
        font * 22.0f,
        ceiling
    );
    if (!widgets::beginAnchoredPopup(popupId, width)) {
        pickerUi().erase(id);
        return false;
    }

    ListOptions options;
    options.mode      = mode;
    options.following = following;
    options.popup     = true;
    options.appearing = ImGui::IsWindowAppearing();
    options.maxRows   = kPopupRows;
    drawListBody(pickerUi()[id], players, entities, current, options, result);
    if (result.picked || result.stop) ImGui::CloseCurrentPopup();

    widgets::endAnchoredPopup();
    return result.picked || result.stop;
}

bool drawPlayerList(
    char const*                              listId,
    PlayerPickerMode                         mode,
    std::span<state::ReplayPlayerView const> players,
    std::span<state::ReplayEntityView const> entities,
    std::string_view                         current,
    bool                                     focusSearch,
    PlayerPickerResult&                      result,
    bool                                     mixed
) {
    result                 = {};
    ImGuiID const id       = ImGui::GetID(listId);
    auto [entry, inserted] = pickerUi().try_emplace(id);
    ImGui::PushID(listId);
    ListOptions options;
    options.mode        = mode;
    options.appearing   = inserted || focusSearch;
    options.focusSearch = focusSearch;
    options.mixed       = mixed;
    options.maxRows     = kEmbeddedRows;
    drawListBody(entry->second, players, entities, current, options, result);
    ImGui::PopID();
    return result.picked;
}

} // namespace playback::editor::ui

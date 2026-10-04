#include "ErrorDialog.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/MessageDialog.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

ErrorDialog& ErrorDialog::getInstance() {
    static ErrorDialog instance;
    return instance;
}

void ErrorDialog::show(std::string_view title, std::string_view msg) {
    mTitle = title;
    mMsg   = msg;
    mOpen  = true;
}

void ErrorDialog::draw() {
    if (!mOpen) return;

    constexpr char const* id = "##editor-error";
    ImGui::OpenPopup(id);
    std::string const title = mTitle.empty() ? "playback.refactorEditor.error.exportFailed"_tr() : mTitle;
    if (beginMessageDialog(id, ICON_WARNING, theme::kError, title)) {
        if (!mMsg.empty()) dialogText(mMsg);
        dialogText("playback.refactorEditor.error.detailsInConsole"_tr(), true);
        std::string const ok = "playback.refactorEditor.common.ok"_tr();
        if (dialogButtons({
                {ok, DialogButtonKind::Primary}
        })
            == 0) {
            ImGui::CloseCurrentPopup();
            mOpen = false;
        }
        endMessageDialog();
    }
}

} // namespace playback::editor::ui

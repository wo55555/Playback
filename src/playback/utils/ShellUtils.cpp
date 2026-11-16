#include "ShellUtils.h"

#include <system_error>
#include <windows.h>

#include <shellapi.h>
#include <shobjidl.h>

namespace playback::utils {

namespace {

bool shellOpen(wchar_t const* parameters, std::wstring const& directory) {
    auto const result = reinterpret_cast<intptr_t>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters, directory.c_str(), SW_SHOWNORMAL)
    );
    return result > 32;
}

} // namespace

bool revealInExplorer(std::filesystem::path const& path) {
    std::error_code ec;
    auto const      absolute = std::filesystem::absolute(path, ec);
    if (ec) return false;

    if (std::filesystem::is_directory(absolute, ec)) {
        return shellOpen((L"\"" + absolute.wstring() + L"\"").c_str(), absolute.wstring());
    }
    auto const parent = absolute.parent_path();
    if (std::filesystem::exists(absolute, ec)) {
        if (shellOpen((L"/select,\"" + absolute.wstring() + L"\"").c_str(), parent.wstring())) return true;
    }
    // The file is gone or Explorer refused the selection; the folder is still the best place to land.
    return std::filesystem::exists(parent, ec)
        && shellOpen((L"\"" + parent.wstring() + L"\"").c_str(), parent.wstring());
}

std::optional<std::filesystem::path> pickFolder(std::filesystem::path const& initial) {
    HRESULT const init     = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool const    ownsInit = SUCCEEDED(init);
    // RPC_E_CHANGED_MODE means COM is already up on this thread in another mode, which is fine for a dialog.
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return std::nullopt;

    std::optional<std::filesystem::path> picked;
    IFileOpenDialog*                     dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        std::error_code ec;
        auto const      start = std::filesystem::absolute(initial, ec);
        if (!ec && std::filesystem::is_directory(start, ec)) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result))) {
                PWSTR name = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                    picked = std::filesystem::path(name);
                    CoTaskMemFree(name);
                }
                result->Release();
            }
        }
        dialog->Release();
    }
    if (ownsInit) CoUninitialize();
    return picked;
}

} // namespace playback::utils

#include "playback/exporting/EcoModeJournal.h"

#include "playback/utils/PathUtils.h"

#include <Windows.h>

#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

namespace playback::exporting {

namespace {

constexpr std::wstring_view JournalPrefix = L"eco-mode-restore-";
constexpr std::wstring_view JournalSuffix = L".marker";

struct Journal {
    uint64_t              pid{};
    uint64_t              created{};
    std::filesystem::path host;
};

uint64_t creationTimeOf(HANDLE process) {
    FILETIME created{};
    FILETIME ignored{};
    if (!GetProcessTimes(process, &created, &ignored, &ignored, &ignored)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

std::filesystem::path hostExecutable() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        auto const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path ownJournalPath() {
    auto name = std::wstring(JournalPrefix) + std::to_wstring(GetCurrentProcessId()) + std::wstring(JournalSuffix);
    return utils::PathUtils::getSharedTempDir() / name;
}

bool isJournalName(std::wstring const& name) {
    return name.size() > JournalPrefix.size() + JournalSuffix.size() && name.starts_with(JournalPrefix)
        && name.ends_with(JournalSuffix);
}

bool readJournal(std::filesystem::path const& path, Journal& journal) {
    std::ifstream in(path, std::ios::binary);
    std::string   host;
    if (!(in >> journal.pid >> journal.created) || !std::getline(in >> std::ws, host)) return false;
    if (!host.empty() && host.back() == '\r') host.pop_back();
    journal.host = std::filesystem::path(std::u8string(host.begin(), host.end()));
    return !journal.host.empty();
}

bool isOwnerRunning(Journal const& journal) {
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(journal.pid));
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;

    bool const running = creationTimeOf(process) == journal.created && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

} // namespace

bool writeEcoModeJournal() noexcept {
    try {
        auto const host = hostExecutable();
        if (host.empty()) return false;

        auto const            journal = ownJournalPath();
        std::error_code       ec;
        std::filesystem::path partial  = journal;
        partial                       += L".partial";
        std::filesystem::create_directories(journal.parent_path(), ec);

        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            auto const    utf8 = host.u8string();
            out << GetCurrentProcessId() << ' ' << creationTimeOf(GetCurrentProcess()) << '\n';
            out.write(reinterpret_cast<char const*>(utf8.data()), static_cast<std::streamsize>(utf8.size()));
            out << '\n';
            out.flush();
            if (!out) {
                out.close();
                std::filesystem::remove(partial, ec);
                return false;
            }
        }

        std::filesystem::rename(partial, journal, ec);
        if (ec) {
            std::filesystem::remove(partial, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void clearEcoModeJournal() noexcept {
    try {
        std::error_code ec;
        std::filesystem::remove(ownJournalPath(), ec);
    } catch (...) {}
}

std::vector<std::filesystem::path> findInterruptedEcoModeJournals() noexcept {
    std::vector<std::filesystem::path> interrupted;
    try {
        auto const      host = hostExecutable();
        std::error_code ec;
        auto const      directory = utils::PathUtils::getSharedTempDir();
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            auto const path = it->path();
            if (!isJournalName(path.filename().wstring())) continue;

            Journal journal;
            if (!readJournal(path, journal)) {
                std::error_code removeEc;
                std::filesystem::remove(path, removeEc);
                continue;
            }
            // Another game install keeps its own Eco Mode setting.
            if (journal.host != host || isOwnerRunning(journal)) continue;
            interrupted.push_back(path);
        }
    } catch (...) {}
    return interrupted;
}

void discardEcoModeJournals(std::vector<std::filesystem::path> const& journals) noexcept {
    for (auto const& path : journals) {
        try {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        } catch (...) {}
    }
}

} // namespace playback::exporting

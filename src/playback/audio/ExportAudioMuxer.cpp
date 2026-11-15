#include "ExportAudioMuxer.h"

#include "playback/Playback.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

namespace playback::audio::exportmux {

namespace {

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

// Same resolution order as the video writer: bundled tool first, then PATH.
std::filesystem::path locateFfmpeg() {
    std::error_code ec;
    auto const      bundled = Playback::getInstance().getSelf().getModDir() / "tools" / "ffmpeg.exe";
    if (std::filesystem::is_regular_file(bundled, ec)) return bundled;

    DWORD const required = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 0, nullptr, nullptr);
    if (required == 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    DWORD const          copied =
        SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (copied == 0 || copied >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), copied));
}

std::wstring quote(std::wstring const& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring result      = L"\"";
    size_t       backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(c);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

// Run ffmpeg to completion, capturing stderr to a log file. Returns the process exit code.
DWORD runFfmpeg(
    std::wstring const&          commandLine,
    std::filesystem::path const& executable,
    std::filesystem::path const& logPath
) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength        = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE logRaw = CreateFileW(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr
    );

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = logRaw != INVALID_HANDLE_VALUE ? logRaw : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError  = startup.hStdOutput;

    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    BOOL const          created = CreateProcessW(
        executable.c_str(),
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &processInfo
    );
    if (logRaw != INVALID_HANDLE_VALUE) CloseHandle(logRaw);
    if (!created) return DWORD(-1);

    WaitForSingleObject(processInfo.hProcess, 120'000);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);
    return exitCode;
}

} // namespace

bool muxIntoMp4(
    std::filesystem::path const& videoPath,
    std::filesystem::path const& sidecarWav,
    exporting::AudioCodec        codec,
    uint32_t                     bitrateKbps
) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(videoPath, ec) || !std::filesystem::is_regular_file(sidecarWav, ec)) {
        logger().warn(
            "Export audio mux: video or sidecar missing (video={}, wav={})",
            videoPath.string(),
            sidecarWav.string()
        );
        return false;
    }
    auto const ffmpeg = locateFfmpeg();
    if (ffmpeg.empty()) {
        logger().warn(
            "Export audio mux: ffmpeg.exe not found; keeping silent video and sidecar {}",
            sidecarWav.string()
        );
        return false;
    }

    auto const stem      = videoPath.stem().wstring();
    auto const extension = videoPath.extension().wstring();
    auto const muxed     = videoPath.parent_path() / (stem + L".mux" + extension);
    auto const logPath   = videoPath.parent_path() / (stem + L".mux.log");
    std::filesystem::remove(muxed, ec);

    std::wstring command  = quote(ffmpeg.wstring());
    command              += L" -hide_banner -loglevel error -nostdin -y";
    command              += L" -i " + quote(videoPath.wstring());
    command              += L" -i " + quote(sidecarWav.wstring());
    command               += L" -map 0:v:0 -map 1:a:0 -c:v copy -c:a ";
    for (char const* c = exporting::ffmpegAudioEncoder(codec); *c != '\0'; ++c) command.push_back(wchar_t(*c));
    // ffmpeg's own Opus encoder, and FLAC inside MP4 on older builds, only run with the experimental flag.
    if (codec != exporting::AudioCodec::Aac) command += L" -strict -2";
    if (exporting::audioCodecUsesBitrate(codec)) {
        auto const kbps =
            std::clamp(bitrateKbps, exporting::MinExportAudioBitrateKbps, exporting::MaxExportAudioBitrateKbps);
        command += L" -b:a " + std::to_wstring(kbps) + L"k";
    }
    command += L" -shortest ";
    command += quote(muxed.wstring());

    DWORD const exitCode = runFfmpeg(command, ffmpeg, logPath);
    std::filesystem::remove(logPath, ec);
    if (exitCode != 0 || !std::filesystem::is_regular_file(muxed, ec)) {
        logger().warn(
            "Export audio mux: ffmpeg exited {}; keeping silent video and sidecar {}",
            exitCode,
            sidecarWav.string()
        );
        std::filesystem::remove(muxed, ec);
        return false;
    }

    // Atomically replace the silent video with the muxed one, then drop the sidecar.
    if (!MoveFileExW(muxed.c_str(), videoPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logger().warn(
            "Export audio mux: unable to replace {} (error {}); keeping both files",
            videoPath.string(),
            GetLastError()
        );
        return false;
    }
    std::filesystem::remove(sidecarWav, ec);
    logger().info("Export audio muxed into {}", videoPath.string());
    return true;
}

} // namespace playback::audio::exportmux

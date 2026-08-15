#include "FfmpegVideoWriter.h"

#include "FrameWriterUtils.h"
#include "playback/Playback.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playback::editor::exporting {

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : mHandle(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle const&)            = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : mHandle(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return mHandle; }
    [[nodiscard]] HANDLE release() {
        HANDLE result = mHandle;
        mHandle       = nullptr;
        return result;
    }
    void reset(HANDLE handle = nullptr) {
        if (mHandle && mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
        mHandle = handle;
    }
    [[nodiscard]] explicit operator bool() const { return mHandle && mHandle != INVALID_HANDLE_VALUE; }

private:
    HANDLE mHandle{};
};

[[nodiscard]] std::filesystem::path locateFfmpeg() {
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

[[nodiscard]] std::filesystem::path const& ffmpegPath() {
    static std::filesystem::path const path = locateFfmpeg();
    return path;
}

[[nodiscard]] std::wstring quoteWindowsArgument(std::wstring const& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring result;
    result.reserve(argument.size() + 2);
    result.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t const character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] std::string windowsErrorMessage(char const* prefix, DWORD error) {
    return std::string(prefix) + " (Windows error " + std::to_string(error) + ")";
}

[[nodiscard]] std::filesystem::path temporaryVideoPath(std::filesystem::path const& output) {
    auto const stem      = output.stem().wstring();
    auto const extension = output.extension().wstring();
    return output.parent_path() / (stem + L".part" + extension);
}

[[nodiscard]] std::filesystem::path ffmpegLogPath(std::filesystem::path const& temporary) {
    return temporary.parent_path() / (temporary.filename().wstring() + L".log");
}

[[nodiscard]] std::string readFfmpegLog(std::filesystem::path const& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    std::streamoff const size = static_cast<std::streamoff>(input.tellg());
    if (size <= 0) return {};
    std::streamoff const offset = std::max<std::streamoff>(0, size - 4096);
    input.seekg(offset, std::ios::beg);
    std::string message(static_cast<size_t>(size - offset), '\0');
    input.read(message.data(), static_cast<std::streamsize>(message.size()));
    for (char& character : message) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
    while (!message.empty() && message.back() == ' ') message.pop_back();
    return message;
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

} // namespace

struct FfmpegVideoWriter::Impl {
    explicit Impl(uint32_t queueCapacity) : capacity(std::max<uint32_t>(1, queueCapacity)) {}

    ~Impl() {
        requestCancel();
        wait();
    }

    uint32_t                                     capacity;
    mutable std::mutex                           mutex;
    std::condition_variable                      changed;
    std::deque<visuals::CapturedFrame> queue;
    std::filesystem::path                        output;
    std::filesystem::path                        temporary;
    std::filesystem::path                        log;
    std::filesystem::path                        executable;
    FrameWriterState                             state{FrameWriterState::Idle};
    uint64_t                                     submitted{};
    uint64_t                                     written{};
    uint64_t                                     nextFrameIndex{};
    uint32_t                                     frameWidth{};
    uint32_t                                     frameHeight{};
    ExportError                                  error{ExportError::None};
    std::string                                  message;
    HANDLE                                       process{};
    HANDLE                                       stdinWrite{};
    std::thread                                  worker;

    void setFailureLocked(ExportError failure, std::string text) {
        if (error == ExportError::None) {
            error   = failure;
            message = std::move(text);
        }
        queue.clear();
        state = FrameWriterState::Cancelling;
        if (process) TerminateProcess(process, 0xC000013A);
        changed.notify_all();
    }

    void requestCancel() {
        bool     requested{};
        uint64_t submittedCopy{};
        uint64_t writtenCopy{};
        {
            std::scoped_lock lock(mutex);
            if (state == FrameWriterState::Running || state == FrameWriterState::Finishing) {
                state = FrameWriterState::Cancelling;
                queue.clear();
                if (process) TerminateProcess(process, 0xC000013A);
                changed.notify_all();
                requested     = true;
                submittedCopy = submitted;
                writtenCopy   = written;
            }
        }
        if (requested) {
            getLogger().info(
                "FFmpeg video writer cancellation requested ({} submitted, {} written)",
                submittedCopy,
                writtenCopy
            );
        }
    }

    void wait() {
        if (worker.joinable()) worker.join();
    }

    [[nodiscard]] bool launchProcess(std::string& failure) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength        = sizeof(attributes);
        attributes.bInheritHandle = TRUE;

        HANDLE readHandleRaw{};
        HANDLE writeHandleRaw{};
        if (!CreatePipe(&readHandleRaw, &writeHandleRaw, &attributes, 0)) {
            failure = windowsErrorMessage("Unable to create the FFmpeg input pipe", GetLastError());
            return false;
        }
        UniqueHandle stdinRead(readHandleRaw);
        UniqueHandle writeHandle(writeHandleRaw);
        if (!SetHandleInformation(writeHandle.get(), HANDLE_FLAG_INHERIT, 0)) {
            failure = windowsErrorMessage("Unable to prepare the FFmpeg input pipe", GetLastError());
            return false;
        }

        HANDLE const logRaw = CreateFileW(
            log.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr
        );
        if (logRaw == INVALID_HANDLE_VALUE) {
            failure = windowsErrorMessage("Unable to create the FFmpeg log", GetLastError());
            return false;
        }
        UniqueHandle logHandle(logRaw);

        std::wstring const executablePath = executable.wstring();
        std::wstring       command        = quoteWindowsArgument(executablePath);
        command += L" -hide_banner -loglevel error -nostdin -f rawvideo -pixel_format rgba -video_size ";
        command += std::to_wstring(frameWidth) + L"x" + std::to_wstring(frameHeight);
        command += L" -framerate ";
        command += std::to_wstring(frameRateNumerator) + L"/" + std::to_wstring(frameRateDenominator);
        command += L" -i pipe:0 -an -c:v libx264 -preset medium -crf 18";
        command += L" -vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -pix_fmt yuv420p -movflags +faststart ";
        command += quoteWindowsArgument(temporary.wstring());
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb         = sizeof(startup);
        startup.dwFlags    = STARTF_USESTDHANDLES;
        startup.hStdInput  = stdinRead.get();
        startup.hStdOutput = logHandle.get();
        startup.hStdError  = logHandle.get();
        PROCESS_INFORMATION processInfo{};
        BOOL const          created = CreateProcessW(
            executablePath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &processInfo
        );
        if (!created) {
            failure = windowsErrorMessage("Unable to start FFmpeg", GetLastError());
            return false;
        }
        UniqueHandle processHandle(processInfo.hProcess);
        UniqueHandle threadHandle(processInfo.hThread);
        stdinRead.reset();
        logHandle.reset();

        {
            std::scoped_lock lock(mutex);
            process    = processHandle.release();
            stdinWrite = writeHandle.release();
            if (state == FrameWriterState::Cancelling) TerminateProcess(process, 0xC000013A);
        }
        getLogger().info(
            "FFmpeg process started: pid={}, executable={}, output={}, size={}x{}, fps={}/{}",
            processInfo.dwProcessId,
            executable,
            temporary,
            frameWidth,
            frameHeight,
            frameRateNumerator,
            frameRateDenominator
        );
        return true;
    }

    [[nodiscard]] bool writeBytes(std::vector<uint8_t> const& rgba, std::string& failure) {
        size_t offset = 0;
        while (offset < rgba.size()) {
            HANDLE pipe{};
            {
                std::scoped_lock lock(mutex);
                if (state == FrameWriterState::Cancelling) return false;
                pipe = stdinWrite;
            }
            if (!pipe) {
                failure = "The FFmpeg input pipe is unavailable";
                return false;
            }
            DWORD const remaining = static_cast<DWORD>(std::min<size_t>(rgba.size() - offset, 1u << 20));
            DWORD       writtenBytes{};
            if (!WriteFile(pipe, rgba.data() + offset, remaining, &writtenBytes, nullptr) || writtenBytes == 0) {
                std::scoped_lock lock(mutex);
                if (state == FrameWriterState::Cancelling) return false;
                failure = windowsErrorMessage("Unable to write a frame to FFmpeg", GetLastError());
                return false;
            }
            offset += writtenBytes;
        }
        return true;
    }

    void closeInput() {
        HANDLE handle{};
        {
            std::scoped_lock lock(mutex);
            handle     = stdinWrite;
            stdinWrite = nullptr;
        }
        if (handle) CloseHandle(handle);
    }

    [[nodiscard]] DWORD waitForProcess() {
        HANDLE handle{};
        {
            std::scoped_lock lock(mutex);
            handle = process;
        }
        if (!handle) return 1;
        DWORD result = WaitForSingleObject(handle, 60'000);
        if (result == WAIT_TIMEOUT) {
            TerminateProcess(handle, 0xC000013A);
            (void)WaitForSingleObject(handle, 5'000);
            std::scoped_lock lock(mutex);
            error   = error == ExportError::None ? ExportError::WriteFailed : error;
            message = message.empty() ? "FFmpeg did not finish within the timeout" : message;
            state   = FrameWriterState::Cancelling;
        }
        DWORD exitCode = 1;
        (void)GetExitCodeProcess(handle, &exitCode);
        {
            std::scoped_lock lock(mutex);
            if (process == handle) {
                CloseHandle(process);
                process = nullptr;
            }
        }
        return exitCode;
    }

    void cleanupFiles(bool removeOutput) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        std::filesystem::remove(log, ec);
        if (removeOutput) std::filesystem::remove(output, ec);
    }

    void workerLoop() {
        std::vector<uint8_t> rgba;
        bool                 processStarted = false;
        while (true) {
            visuals::CapturedFrame item;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return !queue.empty() || state == FrameWriterState::Finishing
                        || state == FrameWriterState::Cancelling;
                });
                if (state == FrameWriterState::Cancelling) {
                    queue.clear();
                    break;
                }
                if (queue.empty()) break;
                item = std::move(queue.front());
                queue.pop_front();
                changed.notify_all();
            }

            if (!processStarted) {
                std::string launchFailure;
                if (!launchProcess(launchFailure)) {
                    std::scoped_lock lock(mutex);
                    if (state != FrameWriterState::Cancelling) {
                        setFailureLocked(ExportError::WriterUnavailable, std::move(launchFailure));
                    }
                    break;
                }
                processStarted = true;
            }

            detail::copyPackedRgba(item, rgba);
            std::string writeFailure;
            if (!writeBytes(rgba, writeFailure)) {
                std::scoped_lock lock(mutex);
                if (state != FrameWriterState::Cancelling) {
                    setFailureLocked(ExportError::WriteFailed, std::move(writeFailure));
                }
                break;
            }
            {
                std::scoped_lock lock(mutex);
                ++written;
                changed.notify_all();
                if (state == FrameWriterState::Cancelling) break;
            }
        }

        bool        cancelled{};
        ExportError failureError{ExportError::None};
        {
            std::scoped_lock lock(mutex);
            cancelled    = state == FrameWriterState::Cancelling;
            failureError = error;
        }

        closeInput();
        DWORD const exitCode = processStarted ? waitForProcess() : 1;
        getLogger().info(
            "FFmpeg process ended (started {}, exit code {}, cancelled {}, error {})",
            processStarted,
            exitCode,
            cancelled,
            static_cast<int>(failureError)
        );
        {
            std::scoped_lock lock(mutex);
            cancelled = state == FrameWriterState::Cancelling;
            if (error != ExportError::None) failureError = error;
        }
        if (!cancelled && failureError == ExportError::None && processStarted && exitCode != 0) {
            auto const       ffmpegMessage = readFfmpegLog(log);
            std::scoped_lock lock(mutex);
            error        = ExportError::WriteFailed;
            message      = ffmpegMessage.empty() ? "FFmpeg failed to encode the video" : ffmpegMessage;
            failureError = error;
        }

        bool completed = false;
        if (!cancelled && failureError == ExportError::None && processStarted && exitCode == 0) {
            std::scoped_lock lock(mutex);
            if (state == FrameWriterState::Cancelling) {
                cancelled = true;
            } else {
                std::error_code ec;
                std::filesystem::rename(temporary, output, ec);
                if (ec) {
                    error        = ExportError::WriteFailed;
                    message      = "Unable to commit the encoded MP4 file";
                    failureError = error;
                } else {
                    completed = true;
                }
            }
        }
        cleanupFiles(!completed);

        {
            std::scoped_lock lock(mutex);
            if (completed) {
                state = FrameWriterState::Completed;
            } else if (failureError != ExportError::None || error != ExportError::None) {
                state = FrameWriterState::Faulted;
            } else {
                state = FrameWriterState::Cancelled;
            }
            changed.notify_all();
        }
    }

    void resetForOpen(CompiledExportPlan const& plan) {
        std::scoped_lock lock(mutex);
        output               = plan.outputPath;
        temporary            = temporaryVideoPath(output);
        log                  = ffmpegLogPath(temporary);
        executable           = ffmpegPath();
        frameRateNumerator   = plan.settings.frameRate.numerator;
        frameRateDenominator = plan.settings.frameRate.denominator;
        state                = FrameWriterState::Idle;
        submitted = written = nextFrameIndex = 0;
        frameWidth = frameHeight = 0;
        error                    = ExportError::None;
        message.clear();
        process    = nullptr;
        stdinWrite = nullptr;
        queue.clear();
    }

    int64_t frameRateNumerator{60};
    int64_t frameRateDenominator{1};
};

FfmpegVideoWriter::FfmpegVideoWriter(uint32_t capacity) : mImpl(std::make_unique<Impl>(capacity)) {}

FfmpegVideoWriter::~FfmpegVideoWriter() = default;

bool FfmpegVideoWriter::isAvailable() { return !ffmpegPath().empty(); }

bool FfmpegVideoWriter::open(CompiledExportPlan const& plan) {
    mImpl->requestCancel();
    mImpl->wait();
    mImpl->resetForOpen(plan);
    if (mImpl->executable.empty()) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->error   = ExportError::WriterUnavailable;
        mImpl->message = "FFmpeg was not found in the Playback tools directory or on PATH";
        mImpl->state   = FrameWriterState::Faulted;
        return false;
    }

    auto const      parent = mImpl->output.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::create_directories(parent, ec) && ec) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->error   = ExportError::DirectoryCreateFailed;
        mImpl->message = "Unable to create the video export directory";
        mImpl->state   = FrameWriterState::Faulted;
        return false;
    }
    if (std::filesystem::exists(mImpl->output, ec) || std::filesystem::exists(mImpl->temporary, ec)) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->error   = ExportError::OutputExists;
        mImpl->message = "The video export output already exists";
        mImpl->state   = FrameWriterState::Faulted;
        return false;
    }
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->state = FrameWriterState::Running;
    }
    getLogger().info(
        "FFmpeg video writer opened: output={}, capacity={}, executable={}",
        mImpl->output,
        mImpl->capacity,
        mImpl->executable
    );
    mImpl->worker = std::thread([impl = mImpl.get()] { impl->workerLoop(); });
    return true;
}

FrameWriterSubmitResult FfmpegVideoWriter::trySubmit(visuals::CapturedFrame& frame) {
    std::scoped_lock lock(mImpl->mutex);
    if (mImpl->error != ExportError::None) return FrameWriterSubmitResult::Failed;
    if (mImpl->state != FrameWriterState::Running) return FrameWriterSubmitResult::Closed;
    if (!detail::validateFrame(frame)) {
        mImpl->setFailureLocked(ExportError::InvalidFrame, "The captured frame has invalid dimensions or pixels");
        return FrameWriterSubmitResult::Failed;
    }
    if (frame.ticket.frameIndex != mImpl->nextFrameIndex) {
        mImpl->setFailureLocked(ExportError::FrameOutOfOrder, "Captured frames were submitted out of order");
        return FrameWriterSubmitResult::Failed;
    }
    if (mImpl->frameWidth == 0) {
        mImpl->frameWidth  = frame.width;
        mImpl->frameHeight = frame.height;
    } else if (frame.width != mImpl->frameWidth || frame.height != mImpl->frameHeight) {
        mImpl->setFailureLocked(ExportError::InvalidFrame, "Captured frame dimensions changed during export");
        return FrameWriterSubmitResult::Failed;
    }
    if (mImpl->queue.size() >= mImpl->capacity) return FrameWriterSubmitResult::Backpressured;
    mImpl->queue.push_back(std::move(frame));
    ++mImpl->nextFrameIndex;
    ++mImpl->submitted;
    mImpl->changed.notify_all();
    return FrameWriterSubmitResult::Accepted;
}

bool FfmpegVideoWriter::requestFinish() {
    std::scoped_lock lock(mImpl->mutex);
    if (mImpl->state == FrameWriterState::Finishing) return true;
    if (mImpl->state != FrameWriterState::Running) return false;
    mImpl->state = FrameWriterState::Finishing;
    mImpl->changed.notify_all();
    return true;
}

void FfmpegVideoWriter::requestCancel() { mImpl->requestCancel(); }

void FfmpegVideoWriter::wait() { mImpl->wait(); }

FrameWriterStatus FfmpegVideoWriter::status() const {
    std::scoped_lock lock(mImpl->mutex);
    return FrameWriterStatus{mImpl->state, mImpl->submitted, mImpl->written, mImpl->error, mImpl->message, {}};
}

} // namespace playback::editor::exporting

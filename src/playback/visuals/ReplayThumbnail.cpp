#include "ReplayThumbnail.h"

#include "playback/io/ReplayLibrary.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace playback::visuals {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t MaxThumbnailDimension = 4096;
constexpr size_t   MaxThumbnailPngBytes  = 16 * 1024 * 1024;
constexpr uint32_t MaxDecodedWidth       = 640;
constexpr uint32_t MaxDecodedHeight      = 360;
constexpr size_t   MaxQueuedThumbnails   = 64;
constexpr size_t   MaxDecodedThumbnails  = 16;

class ComInitialize {
public:
    ComInitialize() {
        HRESULT const hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mNeedsUninit     = SUCCEEDED(hr);
    }
    ~ComInitialize() {
        if (mNeedsUninit) CoUninitialize();
    }
    ComInitialize(ComInitialize const&)            = delete;
    ComInitialize& operator=(ComInitialize const&) = delete;

private:
    bool mNeedsUninit{};
};

[[nodiscard]] bool createFactory(ComPtr<IWICImagingFactory>& factory) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
}

} // namespace

struct ReplayThumbnailLoader::Impl {
    struct Request {
        std::filesystem::path path;
        uint64_t              generation;
        uint64_t              frame;
        bool                  cancelled{};
    };

    struct Cached {
        std::filesystem::path                        path;
        std::shared_ptr<ReplayThumbnailPixels const> pixels;
        uint64_t                                     frame;
        bool                                         consumed;
    };

    std::mutex                                   mutex;
    std::condition_variable                      changed;
    std::deque<Request>                          queued;
    std::optional<Request>                       active;
    std::vector<Cached>                          cache;
    std::shared_ptr<ReplayThumbnailPixels const> failure = std::make_shared<ReplayThumbnailPixels>();
    uint64_t                                     generation{};
    uint64_t                                     frame{};
    bool                                         frameOpen{};
    std::jthread                                 worker;

    Impl() { cache.reserve(MaxDecodedThumbnails); }

    auto replaceable() {
        return std::find_if(cache.begin(), cache.end(), [](auto const& entry) { return entry.consumed; });
    }

    bool hasCacheSpace() { return cache.size() < MaxDecodedThumbnails || replaceable() != cache.end(); }

    void run(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            changed.wait(lock, [&] {
                return stop.stop_requested() || (!frameOpen && !queued.empty() && hasCacheSpace());
            });
            if (stop.stop_requested()) break;
            active = std::move(queued.front());
            queued.pop_front();
            lock.unlock();

            std::shared_ptr<ReplayThumbnailPixels const> pixels = failure;
            try {
                auto png     = io::ReplayLibrary::readThumbnailPng(active->path);
                auto decoded = std::make_shared<ReplayThumbnailPixels>();
                if (decodeReplayThumbnailPng(png, *decoded)) pixels = std::move(decoded);
            } catch (...) {
                pixels = failure;
            }

            lock.lock();
            // Finish visibility collection before accepting a result from an earlier frame.
            changed.wait(lock, [&] {
                return stop.stop_requested() || active->generation != generation || active->cancelled
                    || (!frameOpen && hasCacheSpace());
            });
            if (!stop.stop_requested() && active->generation == generation && !active->cancelled
                && active->frame == frame) {
                if (cache.size() == MaxDecodedThumbnails) cache.erase(replaceable());
                bool const consumed = pixels == failure;
                cache.push_back({std::move(active->path), std::move(pixels), active->frame, consumed});
            }
            active.reset();
        }
    }
};

ReplayThumbnailLoader::ReplayThumbnailLoader() : mImpl(std::make_unique<Impl>()) {}

ReplayThumbnailLoader::~ReplayThumbnailLoader() { stop(); }

void ReplayThumbnailLoader::beginFrame() {
    std::lock_guard lock(mImpl->mutex);
    ++mImpl->frame;
    mImpl->frameOpen = true;
    if (!mImpl->worker.joinable()) {
        mImpl->worker = std::jthread([impl = mImpl.get()](std::stop_token stop) { impl->run(stop); });
    }
}

void ReplayThumbnailLoader::endFrame() {
    {
        std::lock_guard lock(mImpl->mutex);
        std::erase_if(mImpl->queued, [&](auto const& entry) { return entry.frame != mImpl->frame; });
        if (mImpl->active && mImpl->active->frame != mImpl->frame) mImpl->active->cancelled = true;
        for (auto& entry : mImpl->cache) {
            if (entry.frame != mImpl->frame) entry.consumed = true;
        }
        mImpl->frameOpen = false;
    }
    mImpl->changed.notify_all();
}

void ReplayThumbnailLoader::reset() {
    {
        std::lock_guard lock(mImpl->mutex);
        ++mImpl->generation;
        mImpl->queued.clear();
        mImpl->cache.clear();
        mImpl->frameOpen = false;
    }
    mImpl->changed.notify_all();
}

void ReplayThumbnailLoader::stop() {
    {
        std::lock_guard lock(mImpl->mutex);
        mImpl->worker.request_stop();
    }
    mImpl->changed.notify_all();
    if (mImpl->worker.joinable()) mImpl->worker.join();
    reset();
}

std::shared_ptr<ReplayThumbnailPixels const> ReplayThumbnailLoader::request(std::filesystem::path const& path) {
    std::lock_guard lock(mImpl->mutex);
    auto            found =
        std::find_if(mImpl->cache.begin(), mImpl->cache.end(), [&](auto const& entry) { return entry.path == path; });
    if (found != mImpl->cache.end()) {
        auto pixels     = found->pixels;
        found->frame    = mImpl->frame;
        found->consumed = true;
        std::rotate(found, found + 1, mImpl->cache.end());
        mImpl->changed.notify_all();
        return pixels;
    }
    if (mImpl->active && mImpl->active->generation == mImpl->generation && !mImpl->active->cancelled
        && mImpl->active->path == path) {
        mImpl->active->frame = mImpl->frame;
        return nullptr;
    }
    for (auto& entry : mImpl->queued) {
        if (entry.path == path) {
            entry.frame = mImpl->frame;
            return nullptr;
        }
    }
    if (mImpl->queued.size() < MaxQueuedThumbnails) {
        mImpl->queued.push_back({path, mImpl->generation, mImpl->frame});
    }
    return nullptr;
}

bool writeRgbaPng(
    std::filesystem::path const& output,
    uint32_t                     width,
    uint32_t                     height,
    uint8_t const*               rgba,
    uint32_t                     rowPitch
) {
    if (width == 0 || height == 0 || !rgba || rowPitch < width * 4) return false;
    ComInitialize                 com;
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapEncoder>     encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         properties;
    if (!createFactory(factory) || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE))
        || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        || FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get()))
        || FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    uint64_t const size = static_cast<uint64_t>(rowPitch) * height;
    if (size > std::numeric_limits<UINT>::max()) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat32bppRGBA,
            rowPitch,
            static_cast<UINT>(size),
            const_cast<BYTE*>(rgba),
            &bitmap
        ))) {
        return false;
    }
    return SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit())
        && SUCCEEDED(encoder->Commit());
}

bool writeReplayThumbnailPng(
    std::filesystem::path const& output,
    uint32_t                     width,
    uint32_t                     height,
    uint8_t const*               rgba,
    uint32_t                     rowPitch
) {
    return writeRgbaPng(output, width, height, rgba, rowPitch);
}

bool writeReplayThumbnailPng(
    std::filesystem::path const& output,
    CapturedFrame const&         frame,
    uint32_t                     targetWidth,
    uint32_t                     targetHeight
) {
    if (frame.width == 0 || frame.height == 0 || frame.rowPitch < frame.width * 4 || targetWidth == 0
        || targetHeight == 0) {
        return false;
    }
    auto const requiredBytes = static_cast<uint64_t>(frame.rowPitch) * frame.height;
    if (requiredBytes > frame.pixels.size()) return false;

    auto const targetBytes = static_cast<uint64_t>(targetWidth) * targetHeight * 4;
    if (targetBytes > std::numeric_limits<size_t>::max()) return false;
    std::vector<uint8_t> rgba(static_cast<size_t>(targetBytes));
    for (uint32_t y = 0; y < targetHeight; ++y) {
        auto const  sourceY = static_cast<uint32_t>((static_cast<uint64_t>(y) * frame.height) / targetHeight);
        auto const* row =
            reinterpret_cast<uint8_t const*>(frame.pixels.data()) + static_cast<size_t>(sourceY) * frame.rowPitch;
        for (uint32_t x = 0; x < targetWidth; ++x) {
            auto const  sourceX = static_cast<uint32_t>((static_cast<uint64_t>(x) * frame.width) / targetWidth);
            auto const* source  = row + static_cast<size_t>(sourceX) * 4;
            auto*       target  = rgba.data() + (static_cast<size_t>(y) * targetWidth + x) * 4;
            if (frame.pixelFormat == FramePixelFormat::Bgra8) {
                target[0] = source[2];
                target[1] = source[1];
                target[2] = source[0];
                target[3] = source[3];
            } else {
                std::memcpy(target, source, 4);
            }
        }
    }
    return writeRgbaPng(output, targetWidth, targetHeight, rgba.data(), targetWidth * 4);
}

bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output) {
    output = {};
    if (png.empty() || png.size() > MaxThumbnailPngBytes) return false;
    ComInitialize                 com;
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler>      scaler;
    ComPtr<IWICFormatConverter>   converter;
    if (!createFactory(factory) || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<WICInProcPointer>(const_cast<char*>(png.data())),
            static_cast<DWORD>(png.size())
        ))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }
    UINT width{}, height{};
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 || width > MaxThumbnailDimension
        || height > MaxThumbnailDimension) {
        return false;
    }
    IWICBitmapSource* source = frame.Get();
    if (width > MaxDecodedWidth || height > MaxDecodedHeight) {
        if (width * MaxDecodedHeight > height * MaxDecodedWidth) {
            height = std::max(1u, height * MaxDecodedWidth / width);
            width  = MaxDecodedWidth;
        } else {
            width  = std::max(1u, width * MaxDecodedHeight / height);
            height = MaxDecodedHeight;
        }
        if (FAILED(factory->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) {
            return false;
        }
        source = scaler.Get();
    }
    if (FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            source,
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return false;
    }
    ReplayThumbnailPixels decoded{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
    if (FAILED(
            converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(decoded.rgba.size()), decoded.rgba.data())
        )) {
        return false;
    }
    output = std::move(decoded);
    return true;
}

} // namespace playback::visuals

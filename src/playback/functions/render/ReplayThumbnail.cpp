#include "ReplayThumbnail.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <vector>

namespace playback::functions::render {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t MaxThumbnailDimension = 4096;
constexpr uint64_t MaxThumbnailBytes     = 64ull * 1024 * 1024;

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

bool writeReplayThumbnailPng(
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
    return writeReplayThumbnailPng(output, targetWidth, targetHeight, rgba.data(), targetWidth * 4);
}

bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output) {
    output = {};
    if (png.empty() || png.size() > std::numeric_limits<DWORD>::max()) return false;
    ComInitialize                 com;
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    if (!createFactory(factory) || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<WICInProcPointer>(const_cast<char*>(png.data())),
            static_cast<DWORD>(png.size())
        ))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return false;
    }
    if (FAILED(converter->GetSize(&output.width, &output.height)) || output.width == 0 || output.height == 0
        || output.width > MaxThumbnailDimension || output.height > MaxThumbnailDimension) {
        return false;
    }
    uint64_t const byteCount = static_cast<uint64_t>(output.width) * output.height * 4;
    if (byteCount > MaxThumbnailBytes || byteCount > std::numeric_limits<size_t>::max()
        || byteCount > std::numeric_limits<UINT>::max()) {
        return false;
    }
    output.rgba.resize(static_cast<size_t>(byteCount));
    return SUCCEEDED(
        converter->CopyPixels(nullptr, output.width * 4, static_cast<UINT>(output.rgba.size()), output.rgba.data())
    );
}

} // namespace playback::functions::render

#include "ReplayThumbnail.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>

namespace playback::functions::render {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t MaxThumbnailDimension = 4096;
constexpr uint64_t MaxThumbnailBytes     = 64ull * 1024 * 1024;

// 确保当前线程 COM 已初始化（WIC 依赖 COM）。游戏主线程通常已初始化；
// 若在某辅助线程首次调用，这里完成初始化并在函数结束时清理。
class ComInitialize {
public:
    ComInitialize() {
        HRESULT const hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mNeedsUninit      = SUCCEEDED(hr);
        // S_OK and S_FALSE both increment the thread's COM initialization count.
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
    ComInitialize com;
    ComPtr<IWICImagingFactory>   factory;
    ComPtr<IWICStream>           stream;
    ComPtr<IWICBitmapEncoder>    encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>        properties;
    if (!createFactory(factory)
        || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE))
        || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        || FAILED(encoder->CreateNewFrame(&frame, &properties))
        || FAILED(frame->Initialize(properties.Get()))
        || FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    uint64_t const size = static_cast<uint64_t>(rowPitch) * height;
    if (size > std::numeric_limits<UINT>::max()) return false;
    // PNG 编码器会把 SetPixelFormat 请求的格式改写为其首选格式（如 32bppBGRA），因此不能
    // 严格校验 format 是否保持 32bppRGBA，也不能直接 WritePixels（会按改写后的格式解释数据，
    // 造成 R/B 通道错乱）。改用 IWICBitmap 包装像素并经 WriteSource 写入，让 WIC 自动转换。
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
    return SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output) {
    output = {};
    if (png.empty() || png.size() > std::numeric_limits<DWORD>::max()) return false;
    ComInitialize com;
    ComPtr<IWICImagingFactory>   factory;
    ComPtr<IWICStream>           stream;
    ComPtr<IWICBitmapDecoder>    decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>  converter;
    if (!createFactory(factory)
        || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<WICInProcPointer>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size())
        ))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame))
        || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom
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
    return SUCCEEDED(converter->CopyPixels(
        nullptr, output.width * 4, static_cast<UINT>(output.rgba.size()), output.rgba.data()
    ));
}

} // namespace playback::functions::render

#include "ReplayThumbnail.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>

namespace playback::functions::render {

namespace {

using Microsoft::WRL::ComPtr;

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
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
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
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppRGBA) return false;
    uint64_t const size = static_cast<uint64_t>(rowPitch) * height;
    if (size > std::numeric_limits<UINT>::max()) return false;
    return SUCCEEDED(frame->WritePixels(height, rowPitch, static_cast<UINT>(size), const_cast<BYTE*>(rgba)))
        && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool decodeReplayThumbnailPng(std::string_view png, ReplayThumbnailPixels& output) {
    output = {};
    if (png.empty() || png.size() > std::numeric_limits<DWORD>::max()) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
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
    if (FAILED(converter->GetSize(&output.width, &output.height)) || output.width == 0 || output.height == 0) return false;
    uint64_t const byteCount = static_cast<uint64_t>(output.width) * output.height * 4;
    if (byteCount > std::numeric_limits<size_t>::max() || byteCount > std::numeric_limits<UINT>::max()) return false;
    output.rgba.resize(static_cast<size_t>(byteCount));
    return SUCCEEDED(converter->CopyPixels(
        nullptr, output.width * 4, static_cast<UINT>(output.rgba.size()), output.rgba.data()
    ));
}

} // namespace playback::functions::render

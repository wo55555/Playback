#include "SaveableFramebuffer.h"

#include <d3d11_4.h>

#include <array>
#include <cstring>
#include <format>
#include <limits>

namespace playback::exporting {

namespace {

bool isSupported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

visuals::FramePixelFormat pixelFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ? visuals::FramePixelFormat::Bgra8 : visuals::FramePixelFormat::Rgba8;
}

void addBarrier(
    std::array<D3D12_RESOURCE_BARRIER, 6>& barriers,
    UINT&                                  count,
    ID3D12Resource*                        resource,
    D3D12_RESOURCE_STATES                  before,
    D3D12_RESOURCE_STATES                  after
) {
    if (before == after) return;
    auto& barrier                  = barriers[count++];
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
}

} // namespace

SaveableFramebuffer::~SaveableFramebuffer() {
    if (!waitForCompletion()) abandonInFlightResources();
    if (mFenceEvent) CloseHandle(mFenceEvent);
}

bool SaveableFramebuffer::prepare(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& sourceDesc, std::string& error) {
    if (!device || sourceDesc.Width == 0 || sourceDesc.Height == 0 || sourceDesc.ArraySize != 1
        || sourceDesc.MipLevels != 1 || !isSupported(sourceDesc.Format)
        || (sourceDesc.SampleDesc.Count != 1 && sourceDesc.SampleDesc.Count != 2 && sourceDesc.SampleDesc.Count != 4
            && sourceDesc.SampleDesc.Count != 8)) {
        error = "Unsupported D3D11 framebuffer format";
        return false;
    }

    bool const reusable = mBackend == Backend::D3D11 && mD3D11Device.Get() == device && mD3D11FlipTexture
                       && mD3D11Readback && mD3D11CompletionQuery && mWidth == sourceDesc.Width
                       && mHeight == sourceDesc.Height && mFormat == sourceDesc.Format;
    if (!reusable) {
        reset();

        D3D11_TEXTURE2D_DESC flipDesc     = sourceDesc;
        flipDesc.SampleDesc.Count         = 1;
        flipDesc.SampleDesc.Quality       = 0;
        flipDesc.Usage                    = D3D11_USAGE_DEFAULT;
        flipDesc.BindFlags                = 0;
        flipDesc.CPUAccessFlags           = 0;
        flipDesc.MiscFlags                = 0;
        D3D11_TEXTURE2D_DESC readbackDesc = flipDesc;
        readbackDesc.Usage                = D3D11_USAGE_STAGING;
        readbackDesc.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        if (FAILED(device->CreateTexture2D(&flipDesc, nullptr, &mD3D11FlipTexture))
            || FAILED(device->CreateTexture2D(&readbackDesc, nullptr, &mD3D11Readback))
            || FAILED(device->CreateQuery(&queryDesc, &mD3D11CompletionQuery))) {
            error = "Unable to allocate D3D11 framebuffer download resources";
            reset();
            return false;
        }
        mD3D11Device = device;
    }

    mBackend           = Backend::D3D11;
    mWidth             = sourceDesc.Width;
    mHeight            = sourceDesc.Height;
    mFormat            = sourceDesc.Format;
    mSourceSampleCount = sourceDesc.SampleDesc.Count;
    return true;
}

bool SaveableFramebuffer::prepare(ID3D12Device* device, D3D12_RESOURCE_DESC const& sourceDesc, std::string& error) {
    if (!device || sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.Width == 0
        || sourceDesc.Height == 0 || sourceDesc.DepthOrArraySize != 1 || sourceDesc.MipLevels != 1
        || !isSupported(sourceDesc.Format)
        || (sourceDesc.SampleDesc.Count != 1 && sourceDesc.SampleDesc.Count != 2 && sourceDesc.SampleDesc.Count != 4
            && sourceDesc.SampleDesc.Count != 8)) {
        error = "Unsupported D3D12 framebuffer format";
        return false;
    }

    D3D12_RESOURCE_DESC flipDesc = sourceDesc;
    flipDesc.Alignment           = 0;
    flipDesc.SampleDesc.Count    = 1;
    flipDesc.SampleDesc.Quality  = 0;
    flipDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t                           readbackBytes{};
    device->GetCopyableFootprints(&flipDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
    if (readbackBytes == 0) {
        error = "D3D12 framebuffer readback footprint is empty";
        return false;
    }

    bool const reusable = mDevice.Get() == device && mFlipTexture && mReadback && mCommandAllocator && mCommandList
                       && mFence && mWidth == sourceDesc.Width && mHeight == sourceDesc.Height
                       && mFormat == sourceDesc.Format && mReadbackBytes == readbackBytes;
    if (!reusable) {
        reset();
        mDevice = device;

        D3D12_HEAP_PROPERTIES const
            defaultHeap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        if (FAILED(device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &flipDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&mFlipTexture)
            ))) {
            error = "Unable to allocate the D3D12 flip texture";
            reset();
            return false;
        }

        D3D12_HEAP_PROPERTIES const
            readbackHeap{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        D3D12_RESOURCE_DESC const readbackDesc{
            D3D12_RESOURCE_DIMENSION_BUFFER,
            0,
            readbackBytes,
            1,
            1,
            1,
            DXGI_FORMAT_UNKNOWN,
            {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            D3D12_RESOURCE_FLAG_NONE
        };
        if (FAILED(device->CreateCommittedResource(
                &readbackHeap,
                D3D12_HEAP_FLAG_NONE,
                &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&mReadback)
            ))
            || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)))
            || FAILED(device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                mCommandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&mCommandList)
            ))
            || FAILED(mCommandList->Close())
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)))) {
            error = "Unable to allocate D3D12 framebuffer download resources";
            reset();
            return false;
        }
        mCommandListClosed = true;
        if (!mFenceEvent) mFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!mFenceEvent) {
            error = "Unable to create the D3D12 framebuffer fence event";
            reset();
            return false;
        }
    }

    mFootprint         = footprint;
    mReadbackBytes     = readbackBytes;
    mWidth             = static_cast<uint32_t>(sourceDesc.Width);
    mHeight            = sourceDesc.Height;
    mFormat            = sourceDesc.Format;
    mSourceSampleCount = sourceDesc.SampleDesc.Count;
    mBackend           = Backend::D3D12;
    return true;
}

bool SaveableFramebuffer::submit(
    ID3D11Device*               device,
    ID3D11DeviceContext*        context,
    ID3D11Texture2D*            source,
    ID3D11Texture2D*            previewTexture,
    visuals::FrameTicket const& ticket,
    std::string&                error
) {
    if (mState == State::Submitted || !device || !context || !source || !previewTexture) {
        error = "The D3D11 framebuffer slot is unavailable";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
    Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
    Microsoft::WRL::ComPtr<ID3D11Device> previewDevice;
    context->GetDevice(&contextDevice);
    source->GetDevice(&sourceDevice);
    previewTexture->GetDevice(&previewDevice);
    if (contextDevice.Get() != device || sourceDevice.Get() != device || previewDevice.Get() != device) {
        error = "The D3D11 framebuffer resources belong to different devices";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        error = "The D3D11 immediate context does not support multithread protection";
        return false;
    }
    multithread->SetMultithreadProtected(TRUE);

    D3D11_TEXTURE2D_DESC sourceDesc{};
    D3D11_TEXTURE2D_DESC previewDesc{};
    source->GetDesc(&sourceDesc);
    previewTexture->GetDesc(&previewDesc);
    if (!prepare(device, sourceDesc, error)) return false;
    if (previewDesc.Width != sourceDesc.Width || previewDesc.Height != sourceDesc.Height
        || previewDesc.Format != sourceDesc.Format || previewDesc.SampleDesc.Count != 1) {
        error = "The D3D11 framebuffer preview texture is incompatible";
        return false;
    }

    if (sourceDesc.SampleDesc.Count > 1) {
        context->ResolveSubresource(mD3D11FlipTexture.Get(), 0, source, 0, sourceDesc.Format);
    } else {
        context->CopyResource(mD3D11FlipTexture.Get(), source);
    }
    context->CopyResource(previewTexture, mD3D11FlipTexture.Get());
    context->CopyResource(mD3D11Readback.Get(), mD3D11FlipTexture.Get());
    context->End(mD3D11CompletionQuery.Get());

    mD3D11Context  = context;
    mTicket        = ticket;
    mSourceAddress = reinterpret_cast<uintptr_t>(source);
    mCommandQueue  = context;
    mSourceState   = 0;
    mFenceValue    = 0;
    mState         = State::Submitted;
    return true;
}

bool SaveableFramebuffer::submit(
    ID3D12Device*               device,
    ID3D12CommandQueue*         queue,
    ID3D12Resource*             source,
    D3D12_RESOURCE_STATES       sourceState,
    ID3D12Resource*             previewTexture,
    visuals::FrameTicket const& ticket,
    std::string&                error
) {
    if (mState == State::Submitted || !queue || !source || !previewTexture) {
        error = "The D3D12 framebuffer slot is unavailable";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> sourceDevice;
    Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
    Microsoft::WRL::ComPtr<ID3D12Device> previewDevice;
    if (!device || FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice))) || sourceDevice.Get() != device
        || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device
        || FAILED(previewTexture->GetDevice(IID_PPV_ARGS(&previewDevice))) || previewDevice.Get() != device) {
        error = "The D3D12 framebuffer resources belong to different devices";
        return false;
    }

    auto const sourceDesc  = source->GetDesc();
    auto const previewDesc = previewTexture->GetDesc();
    if (!prepare(device, sourceDesc, error)) return false;
    if (previewDesc.Width != sourceDesc.Width || previewDesc.Height != sourceDesc.Height
        || previewDesc.Format != sourceDesc.Format || previewDesc.SampleDesc.Count != 1) {
        error = "The D3D12 framebuffer preview texture is incompatible";
        return false;
    }

    HRESULT const allocatorResult = mCommandAllocator->Reset();
    HRESULT const listResult =
        SUCCEEDED(allocatorResult) ? mCommandList->Reset(mCommandAllocator.Get(), nullptr) : allocatorResult;
    if (FAILED(allocatorResult) || FAILED(listResult)) {
        error  = "Unable to reset the D3D12 framebuffer command list";
        mState = State::Faulted;
        return false;
    }
    mCommandListClosed = false;

    std::array<D3D12_RESOURCE_BARRIER, 6> barriers{};
    UINT                                  barrierCount{};
    bool const                            multisampled = sourceDesc.SampleDesc.Count > 1;
    if (multisampled) {
        addBarrier(barriers, barrierCount, source, sourceState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        addBarrier(
            barriers,
            barrierCount,
            mFlipTexture.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_RESOLVE_DEST
        );
        mCommandList->ResourceBarrier(barrierCount, barriers.data());
        mCommandList->ResolveSubresource(mFlipTexture.Get(), 0, source, 0, sourceDesc.Format);
        barrierCount = 0;
        addBarrier(
            barriers,
            barrierCount,
            mFlipTexture.Get(),
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
    } else {
        addBarrier(barriers, barrierCount, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        addBarrier(
            barriers,
            barrierCount,
            mFlipTexture.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST
        );
        mCommandList->ResourceBarrier(barrierCount, barriers.data());
        mCommandList->CopyResource(mFlipTexture.Get(), source);
        barrierCount = 0;
        addBarrier(
            barriers,
            barrierCount,
            mFlipTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE
        );
    }
    addBarrier(
        barriers,
        barrierCount,
        previewTexture,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST
    );
    mCommandList->ResourceBarrier(barrierCount, barriers.data());

    D3D12_TEXTURE_COPY_LOCATION readbackDestination{};
    readbackDestination.pResource       = mReadback.Get();
    readbackDestination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readbackDestination.PlacedFootprint = mFootprint;
    D3D12_TEXTURE_COPY_LOCATION flipSource{};
    flipSource.pResource        = mFlipTexture.Get();
    flipSource.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    flipSource.SubresourceIndex = 0;
    mCommandList->CopyTextureRegion(&readbackDestination, 0, 0, 0, &flipSource, nullptr);
    mCommandList->CopyResource(previewTexture, mFlipTexture.Get());

    barrierCount = 0;
    addBarrier(
        barriers,
        barrierCount,
        previewTexture,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    addBarrier(
        barriers,
        barrierCount,
        mFlipTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COMMON
    );
    addBarrier(
        barriers,
        barrierCount,
        source,
        multisampled ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
        sourceState
    );
    mCommandList->ResourceBarrier(barrierCount, barriers.data());

    if (FAILED(mCommandList->Close())) {
        error  = "Unable to close the D3D12 framebuffer command list";
        mState = State::Faulted;
        return false;
    }
    mCommandListClosed = true;

    mTicket        = ticket;
    mSourceAddress = reinterpret_cast<uintptr_t>(source);
    mCommandQueue  = queue;
    mSourceState   = static_cast<uint32_t>(sourceState);
    mFenceValue    = mFenceValue + 1;
    mState         = State::Submitted;
    ID3D12CommandList* lists[]{mCommandList.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(mFence.Get(), mFenceValue))) {
        error = "Unable to signal the D3D12 framebuffer fence";
        abandonInFlightResources();
        mState = State::Faulted;
        return false;
    }
    return true;
}

bool SaveableFramebuffer::isComplete() const {
    if (mState != State::Submitted) return false;
    if (mBackend == Backend::D3D11) {
        if (!mD3D11Context || !mD3D11CompletionQuery) return false;
        BOOL          complete{};
        HRESULT const result =
            mD3D11Context
                ->GetData(mD3D11CompletionQuery.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        return result == S_OK && complete != FALSE;
    }
    return mState == State::Submitted && mFence && mFence->GetCompletedValue() >= mFenceValue;
}

std::optional<visuals::CapturedFrame> SaveableFramebuffer::finish(std::string& error) {
    if (!isComplete()) return std::nullopt;

    if (mBackend == Backend::D3D11) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT const            result = mD3D11Context->Map(mD3D11Readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        uint64_t const           packedRowBytes = static_cast<uint64_t>(mWidth) * 4;
        if (FAILED(result) || !mapped.pData || mapped.RowPitch < packedRowBytes
            || packedRowBytes * mHeight > std::numeric_limits<size_t>::max()) {
            error = std::format(
                "Unable to map the D3D11 framebuffer readback (HRESULT=0x{:08X})",
                static_cast<uint32_t>(result)
            );
            mState = State::Faulted;
            return std::nullopt;
        }

        visuals::CapturedFrame frame;
        frame.ticket                          = mTicket;
        frame.submission.width                = mWidth;
        frame.submission.height               = mHeight;
        frame.submission.sampleCount          = mSourceSampleCount;
        frame.submission.pixelFormat          = pixelFormat(mFormat);
        frame.submission.exportResource       = mD3D11FlipTexture.Get();
        frame.submission.commandQueue         = mD3D11Context.Get();
        frame.submission.completionFence      = mD3D11CompletionQuery.Get();
        frame.submission.completionFenceValue = 0;
        frame.submission.sourceState          = 0;
        frame.width                           = mWidth;
        frame.height                          = mHeight;
        frame.rowPitch                        = mWidth * 4;
        frame.pixelFormat                     = pixelFormat(mFormat);
        frame.pixels.resize(static_cast<size_t>(frame.rowPitch) * frame.height);
        for (uint32_t y = 0; y < mHeight; ++y) {
            auto const* source = static_cast<std::byte const*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitch, source, frame.rowPitch);
        }
        mD3D11Context->Unmap(mD3D11Readback.Get(), 0);
        mState = State::Available;
        return frame;
    }

    uint64_t const packedRowBytes = static_cast<uint64_t>(mWidth) * 4;
    uint64_t       lastByte       = mFootprint.Offset;
    bool valid                    = mHeight != 0 && mFootprint.Footprint.RowPitch >= packedRowBytes
                                 && (mHeight - 1) <= (std::numeric_limits<uint64_t>::max() - lastByte) / mFootprint.Footprint.RowPitch;
    if (valid) {
        lastByte += static_cast<uint64_t>(mHeight - 1) * mFootprint.Footprint.RowPitch;
        valid     = packedRowBytes <= std::numeric_limits<uint64_t>::max() - lastByte
                 && lastByte + packedRowBytes <= mReadbackBytes;
    }
    if (!valid || packedRowBytes * mHeight > std::numeric_limits<size_t>::max()) {
        error  = "The D3D12 framebuffer readback footprint is invalid";
        mState = State::Faulted;
        return std::nullopt;
    }

    void*         mapped{};
    D3D12_RANGE   readRange{0, static_cast<SIZE_T>(mReadbackBytes)};
    HRESULT const result = mReadback->Map(0, &readRange, &mapped);
    if (FAILED(result)) {
        error = std::format(
            "Unable to map the D3D12 framebuffer readback (HRESULT=0x{:08X})",
            static_cast<uint32_t>(result)
        );
        mState = State::Faulted;
        return std::nullopt;
    }

    visuals::CapturedFrame frame;
    frame.ticket                          = mTicket;
    frame.submission.width                = mWidth;
    frame.submission.height               = mHeight;
    frame.submission.sampleCount          = mSourceSampleCount;
    frame.submission.pixelFormat          = pixelFormat(mFormat);
    frame.submission.exportResource       = mFlipTexture.Get();
    frame.submission.commandQueue         = mCommandQueue;
    frame.submission.completionFence      = mFence.Get();
    frame.submission.completionFenceValue = mFenceValue;
    frame.submission.sourceState          = mSourceState;
    frame.width                           = mWidth;
    frame.height                          = mHeight;
    frame.rowPitch                        = mWidth * 4;
    frame.pixelFormat                     = pixelFormat(mFormat);
    frame.pixels.resize(static_cast<size_t>(frame.rowPitch) * frame.height);
    for (uint32_t y = 0; y < mHeight; ++y) {
        auto const* source = static_cast<std::byte const*>(mapped) + mFootprint.Offset
                           + static_cast<size_t>(y) * mFootprint.Footprint.RowPitch;
        std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitch, source, frame.rowPitch);
    }
    D3D12_RANGE const writtenRange{0, 0};
    mReadback->Unmap(0, &writtenRange);
    mState = State::Available;
    return frame;
}

bool SaveableFramebuffer::waitForCompletion() {
    if (mState != State::Submitted) return true;
    if (mBackend == Backend::D3D11) {
        if (!mD3D11Context || !mD3D11CompletionQuery) return false;
        mD3D11Context->Flush();
        for (;;) {
            BOOL          complete{};
            HRESULT const result = mD3D11Context->GetData(mD3D11CompletionQuery.Get(), &complete, sizeof(complete), 0);
            if (result == S_OK) return complete != FALSE;
            if (result != S_FALSE) return false;
            SwitchToThread();
        }
    }
    if (mState != State::Submitted || !mFence || mFenceValue == 0 || mFence->GetCompletedValue() >= mFenceValue)
        return true;
    if (!mFenceEvent || FAILED(mFence->SetEventOnCompletion(mFenceValue, mFenceEvent))) return false;
    return WaitForSingleObject(mFenceEvent, INFINITE) == WAIT_OBJECT_0;
}

void SaveableFramebuffer::abandonInFlightResources() {
    (void)mD3D11Device.Detach();
    (void)mD3D11Context.Detach();
    (void)mD3D11FlipTexture.Detach();
    (void)mD3D11Readback.Detach();
    (void)mD3D11CompletionQuery.Detach();
    (void)mDevice.Detach();
    (void)mCommandAllocator.Detach();
    (void)mCommandList.Detach();
    (void)mFlipTexture.Detach();
    (void)mReadback.Detach();
    (void)mFence.Detach();
}

void SaveableFramebuffer::reset() {
    if (!waitForCompletion()) {
        abandonInFlightResources();
    }
    mD3D11Device.Reset();
    mD3D11Context.Reset();
    mD3D11FlipTexture.Reset();
    mD3D11Readback.Reset();
    mD3D11CompletionQuery.Reset();
    mDevice.Reset();
    mCommandAllocator.Reset();
    mCommandList.Reset();
    mFlipTexture.Reset();
    mReadback.Reset();
    mFence.Reset();
    mFootprint         = {};
    mReadbackBytes     = 0;
    mFenceValue        = 0;
    mTicket            = {};
    mWidth             = 0;
    mHeight            = 0;
    mSourceSampleCount = 1;
    mSourceAddress     = 0;
    mCommandQueue      = nullptr;
    mSourceState       = 0;
    mFormat            = DXGI_FORMAT_UNKNOWN;
    mCommandListClosed = false;
    mBackend           = Backend::None;
    mState             = State::Available;
}

} // namespace playback::exporting

#pragma once

#include "playback/editor/graphics/D3D12Compat.h"
#include "playback/visuals/FrameCaptureTypes.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>

namespace playback::exporting {

class SaveableFramebuffer {
public:
    enum class Backend : uint8_t { None, D3D11, D3D12 };
    enum class State : uint8_t { Available, Submitted, Faulted };

    SaveableFramebuffer() = default;
    ~SaveableFramebuffer();

    SaveableFramebuffer(SaveableFramebuffer const&)            = delete;
    SaveableFramebuffer& operator=(SaveableFramebuffer const&) = delete;

    [[nodiscard]] bool submit(
        ID3D11Device*               device,
        ID3D11DeviceContext*        context,
        ID3D11Texture2D*            source,
        ID3D11Texture2D*            previewTexture,
        visuals::FrameTicket const& ticket,
        std::string&                error
    );
    [[nodiscard]] bool submit(
        ID3D12Device*               device,
        ID3D12CommandQueue*         queue,
        ID3D12Resource*             source,
        D3D12_RESOURCE_STATES       sourceState,
        ID3D12Resource*             previewTexture,
        visuals::FrameTicket const& ticket,
        std::string&                error
    );
    [[nodiscard]] bool                                  isComplete() const;
    [[nodiscard]] std::optional<visuals::CapturedFrame> finish(std::string& error);
    [[nodiscard]] bool                                  waitForCompletion();
    void                                                reset();

    [[nodiscard]] Backend                     backend() const { return mBackend; }
    [[nodiscard]] State                       state() const { return mState; }
    [[nodiscard]] ID3D11Texture2D*            d3d11FlipTexture() const { return mD3D11FlipTexture.Get(); }
    [[nodiscard]] ID3D11Texture2D*            d3d11ReadbackTexture() const { return mD3D11Readback.Get(); }
    [[nodiscard]] ID3D11Query*                d3d11CompletionQuery() const { return mD3D11CompletionQuery.Get(); }
    [[nodiscard]] ID3D12Resource*             flipTexture() const { return mFlipTexture.Get(); }
    [[nodiscard]] ID3D12Resource*             readbackBuffer() const { return mReadback.Get(); }
    [[nodiscard]] ID3D12Fence*                fence() const { return mFence.Get(); }
    [[nodiscard]] uint64_t                    fenceValue() const { return mFenceValue; }
    [[nodiscard]] uint32_t                    width() const { return mWidth; }
    [[nodiscard]] uint32_t                    height() const { return mHeight; }
    [[nodiscard]] DXGI_FORMAT                 format() const { return mFormat; }
    [[nodiscard]] uint32_t                    sampleCount() const { return mSourceSampleCount; }
    [[nodiscard]] uintptr_t                   sourceAddress() const { return mSourceAddress; }
    [[nodiscard]] visuals::FrameTicket const& ticket() const { return mTicket; }

private:
    [[nodiscard]] bool prepare(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& sourceDesc, std::string& error);
    [[nodiscard]] bool prepare(ID3D12Device* device, D3D12_RESOURCE_DESC const& sourceDesc, std::string& error);
    void               abandonInFlightResources();

    Microsoft::WRL::ComPtr<ID3D11Device>              mD3D11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>       mD3D11Context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>           mD3D11FlipTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>           mD3D11Readback;
    Microsoft::WRL::ComPtr<ID3D11Query>               mD3D11CompletionQuery;
    Microsoft::WRL::ComPtr<ID3D12Device>              mDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    mCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;
    Microsoft::WRL::ComPtr<ID3D12Resource>            mFlipTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>            mReadback;
    Microsoft::WRL::ComPtr<ID3D12Fence>               mFence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT                mFootprint{};
    uint64_t                                          mReadbackBytes{};
    uint64_t                                          mFenceValue{};
    visuals::FrameTicket                              mTicket;
    uint32_t                                          mWidth{};
    uint32_t                                          mHeight{};
    uint32_t                                          mSourceSampleCount{1};
    uintptr_t                                         mSourceAddress{};
    void*                                             mCommandQueue{};
    uint32_t                                          mSourceState{};
    DXGI_FORMAT                                       mFormat{DXGI_FORMAT_UNKNOWN};
    HANDLE                                            mFenceEvent{};
    bool                                              mCommandListClosed{};
    Backend                                           mBackend{Backend::None};
    State                                             mState{State::Available};
};

} // namespace playback::exporting

#pragma once

#include "playback/visuals/FrameTap.h"

#include <cstdint>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace playback::editor::renderer {

class D3D12FrameTapBackend {
public:
    explicit D3D12FrameTapBackend(visuals::FrameTap& frameTap);
    ~D3D12FrameTapBackend();

    D3D12FrameTapBackend(D3D12FrameTapBackend const&)            = delete;
    D3D12FrameTapBackend& operator=(D3D12FrameTapBackend const&) = delete;

    bool capture(
        ID3D12Device*              device,
        ID3D12CommandQueue*        queue,
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            source,
        uint32_t                   sourceState
    );
    bool
    captureSubmitted(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source, uint32_t sourceState);
    void submitted(ID3D12Fence* fence, uint64_t fenceValue);
    void submissionFailed(visuals::FrameTapError error, std::string message);
    void reset(visuals::FrameTapError error, std::string message);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::renderer

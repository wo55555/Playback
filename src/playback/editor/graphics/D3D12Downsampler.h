#pragma once

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace playback::editor::graphics {

// Averages each factor x factor block on the GPU so the readback only carries output-sized pixels.
class D3D12Downsampler {
public:
    D3D12Downsampler();
    ~D3D12Downsampler();

    D3D12Downsampler(D3D12Downsampler const&)            = delete;
    D3D12Downsampler& operator=(D3D12Downsampler const&) = delete;

    [[nodiscard]] bool ready(ID3D12Device* device);

    // Source must be in PIXEL_SHADER_RESOURCE, destination in UNORDERED_ACCESS; both are left that way.
    bool dispatch(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            source,
        ID3D12Resource*            destination,
        uint32_t                   destinationWidth,
        uint32_t                   destinationHeight,
        uint32_t                   factor
    );

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::graphics

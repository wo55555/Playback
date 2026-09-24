#include "D3D12Downsampler.h"

#include "playback/editor/graphics/D3D12Compat.h"

#include "playback/Playback.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstring>

namespace playback::editor::graphics {
namespace {

using Microsoft::WRL::ComPtr;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr uint32_t ThreadGroupSize = 8;

// A box average matches the CPU path exactly, so switching to the GPU does not change the output.
constexpr char const* DownsampleSource = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);

cbuffer Params : register(b0) {
    uint2 DestinationSize;
    uint Factor;
    uint Padding;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DestinationSize.x || id.y >= DestinationSize.y) return;
    uint2 origin = id.xy * Factor;
    float4 total = 0.0f;
    for (uint y = 0; y < Factor; ++y) {
        for (uint x = 0; x < Factor; ++x) {
            total += Source.Load(int3(origin + uint2(x, y), 0));
        }
    }
    Destination[id.xy] = total / (float)(Factor * Factor);
}
)";

struct Params {
    uint32_t width{};
    uint32_t height{};
    uint32_t factor{};
    uint32_t padding{};
};

} // namespace

struct D3D12Downsampler::Impl {
    ComPtr<ID3D12Device>         device;
    ComPtr<ID3D12RootSignature>  rootSignature;
    ComPtr<ID3D12PipelineState>  pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    uint32_t                     descriptorSize{};
    bool                         failed{};

    bool build(ID3D12Device* target) {
        if (failed) return false;
        if (pipeline && device.Get() == target) return true;

        rootSignature.Reset();
        pipeline.Reset();
        descriptorHeap.Reset();
        device = target;

        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        HRESULT const    compileResult = D3DCompile(
            DownsampleSource,
            std::strlen(DownsampleSource),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            0,
            0,
            &shader,
            &errors
        );
        if (FAILED(compileResult)) {
            getLogger().error(
                "Downsample shader compilation failed (hr=0x{:08X}): {}",
                static_cast<uint32_t>(compileResult),
                errors ? static_cast<char const*>(errors->GetBufferPointer()) : "no diagnostics"
            );
            failed = true;
            return false;
        }

        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 1;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
        parameters[0].DescriptorTable.pDescriptorRanges   = ranges.data();
        parameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.ShaderRegister            = 0;
        parameters[1].Constants.Num32BitValues            = sizeof(Params) / sizeof(uint32_t);
        parameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = static_cast<UINT>(parameters.size());
        rootDesc.pParameters   = parameters.data();

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> rootErrors;
        HRESULT const    serializeResult =
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
        if (FAILED(serializeResult)) {
            getLogger().error(
                "Downsample root signature serialization failed (hr=0x{:08X})",
                static_cast<uint32_t>(serializeResult)
            );
            failed = true;
            return false;
        }
        HRESULT const rootResult = device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)
        );
        if (FAILED(rootResult)) {
            getLogger().error(
                "Downsample root signature creation failed (hr=0x{:08X})",
                static_cast<uint32_t>(rootResult)
            );
            failed = true;
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature     = rootSignature.Get();
        pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
        pipelineDesc.CS.BytecodeLength  = shader->GetBufferSize();
        HRESULT const pipelineResult    = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline));
        if (FAILED(pipelineResult)) {
            getLogger().error(
                "Downsample pipeline creation failed (hr=0x{:08X})",
                static_cast<uint32_t>(pipelineResult)
            );
            failed = true;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type            = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors  = 2;
        heapDesc.Flags           = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT const heapResult = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
        if (FAILED(heapResult)) {
            getLogger().error(
                "Downsample descriptor heap creation failed (hr=0x{:08X})",
                static_cast<uint32_t>(heapResult)
            );
            failed = true;
            return false;
        }
        descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        getLogger().info("Downsample compute pipeline ready");
        return true;
    }
};

D3D12Downsampler::D3D12Downsampler() : mImpl(std::make_unique<Impl>()) {}

D3D12Downsampler::~D3D12Downsampler() = default;

bool D3D12Downsampler::ready(ID3D12Device* device) { return device && mImpl->build(device); }

bool D3D12Downsampler::dispatch(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource*            source,
    ID3D12Resource*            destination,
    uint32_t                   destinationWidth,
    uint32_t                   destinationHeight,
    uint32_t                   factor
) {
    if (!commandList || !source || !destination || factor < 2 || !mImpl->pipeline) return false;

    auto const base = mImpl->descriptorHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = source->GetDesc().Format;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    mImpl->device->CreateShaderResourceView(source, &srv, base);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = destination->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{base.ptr + mImpl->descriptorSize};
    mImpl->device->CreateUnorderedAccessView(destination, nullptr, &uav, uavHandle);

    Params const params{destinationWidth, destinationHeight, factor, 0};

    ID3D12DescriptorHeap* heaps[]{mImpl->descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mImpl->rootSignature.Get());
    commandList->SetPipelineState(mImpl->pipeline.Get());
    commandList->SetComputeRootDescriptorTable(0, mImpl->descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRoot32BitConstants(1, sizeof(Params) / sizeof(uint32_t), &params, 0);
    commandList->Dispatch(
        (destinationWidth + ThreadGroupSize - 1) / ThreadGroupSize,
        (destinationHeight + ThreadGroupSize - 1) / ThreadGroupSize,
        1
    );
    return true;
}

} // namespace playback::editor::graphics

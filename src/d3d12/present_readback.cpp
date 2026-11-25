#include "present_readback.h"

#include <limits>
#include <utility>

namespace muxivo::d3d12 {
namespace {

constexpr UINT protectedSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY
    | DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT
    | DXGI_SWAP_CHAIN_FLAG_RESTRICT_SHARED_RESOURCE_DRIVER
    | DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;

bool supported(const D3D12_RESOURCE_DESC& desc) noexcept {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        && desc.Width > 0 && desc.Width <= 3840 && desc.Height > 0 && desc.Height <= 2160
        && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1
        && (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
            || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
            || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}

bool isBlueFirst(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

void transition(ID3D12GraphicsCommandList& commands, ID3D12Resource& source,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {&source, 0, before, after};
    commands.ResourceBarrier(1, &barrier);
}

} // namespace

HRESULT PresentReadback::record(IDXGISwapChain3& swapChain, DXGI_COLOR_SPACE_TYPE colorSpace,
    ID3D12GraphicsCommandList& commands, ID3D12Fence& fence, UINT64 completionValue) noexcept {
    if (source_) return S_FALSE;
    if (commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    const auto completed = fence.GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completionValue <= completed || completionValue == UINT64_MAX) return E_INVALIDARG;

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    HRESULT hr = swapChain.GetDesc1(&swapDesc);
    if (FAILED(hr)) return hr;
    if (swapDesc.Flags & protectedSwapChainFlags) return E_ACCESSDENIED;
    if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return DXGI_ERROR_UNSUPPORTED;

    Microsoft::WRL::ComPtr<ID3D12Resource> source;
    hr = swapChain.GetBuffer(swapChain.GetCurrentBackBufferIndex(), IID_PPV_ARGS(&source));
    if (FAILED(hr)) return hr;
    const auto desc = source->GetDesc();
    if (!supported(desc)) return DXGI_ERROR_UNSUPPORTED;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IUnknown> sourceDevice;
    Microsoft::WRL::ComPtr<IUnknown> commandDevice;
    Microsoft::WRL::ComPtr<IUnknown> fenceDevice;
    if (FAILED(hr = source->GetDevice(IID_PPV_ARGS(&device)))
        || FAILED(hr = device.As(&sourceDevice))
        || FAILED(hr = commands.GetDevice(IID_PPV_ARGS(&commandDevice)))
        || FAILED(hr = fence.GetDevice(IID_PPV_ARGS(&fenceDevice)))) return hr;
    if (sourceDevice.Get() != commandDevice.Get() || sourceDevice.Get() != fenceDevice.Get()) return E_INVALIDARG;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    if (!bytes || bytes == UINT64_MAX || bytes > std::numeric_limits<SIZE_T>::max()) return E_INVALIDARG;

    if (!buffer_ || bytes > bufferBytes_ || bufferDevice_.Get() != sourceDevice.Get()) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC layout{};
        layout.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        layout.Width = bytes;
        layout.Height = layout.DepthOrArraySize = layout.MipLevels = 1;
        layout.SampleDesc.Count = 1;
        layout.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &layout,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) return hr;
        buffer_ = std::move(buffer);
        bufferDevice_ = std::move(sourceDevice);
        bufferBytes_ = bytes;
    }

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = buffer_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION input{};
    input.pResource = source.Get();
    input.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    transition(commands, *source.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.CopyTextureRegion(&destination, 0, 0, 0, &input, nullptr);
    transition(commands, *source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    source_ = std::move(source);
    fence_ = &fence;
    footprint_ = footprint;
    completionValue_ = completionValue;
    return S_OK;
}

HRESULT PresentReadback::releaseCompleted() noexcept {
    if (!source_) return S_FALSE;
    const auto completed = fence_->GetCompletedValue();
    if (completed < completionValue_) return S_FALSE;
    source_.Reset();
    fence_.Reset();
    return completed == UINT64_MAX ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

HRESULT PresentReadback::discardCompleted() noexcept {
    return releaseCompleted();
}

void PresentReadback::forceReleaseSource() noexcept {
    source_.Reset();
    fence_.Reset();
}

HRESULT PresentReadback::collect(SdrFrame& frame) {
    const auto completion = releaseCompleted();
    if (completion != S_OK) return completion;

    const auto& layout = footprint_.Footprint;
    const auto stride = static_cast<std::size_t>(layout.Width) * 4;
    if (layout.Width == 0 || layout.Height == 0 || layout.RowPitch < stride) {
        return E_UNEXPECTED;
    }
    const auto lastByte = footprint_.Offset + (layout.Height - 1) * UINT64(layout.RowPitch) + stride;
    if (lastByte > bufferBytes_) {
        return E_UNEXPECTED;
    }
    frame.rgba.reserve(stride * layout.Height);
    const D3D12_RANGE reads = {static_cast<SIZE_T>(footprint_.Offset), static_cast<SIZE_T>(lastByte)};
    void* mapped = nullptr;
    const auto hr = buffer_->Map(0, &reads, &mapped);
    if (FAILED(hr) || !mapped) return FAILED(hr) ? hr : E_POINTER;
    frame.rgba.resize(stride * layout.Height);
    const bool blueFirst = isBlueFirst(layout.Format);
    const auto* base = static_cast<const std::uint8_t*>(mapped) + footprint_.Offset;
    for (UINT y = 0; y < layout.Height; ++y) {
        const auto* source = base + static_cast<std::size_t>(y) * layout.RowPitch;
        auto* target = frame.rgba.data() + static_cast<std::size_t>(y) * stride;
        for (UINT x = 0; x < layout.Width; ++x, source += 4, target += 4) {
            target[0] = source[blueFirst ? 2 : 0];
            target[1] = source[1];
            target[2] = source[blueFirst ? 0 : 2];
            target[3] = 255;
        }
    }
    const D3D12_RANGE noWrites{};
    buffer_->Unmap(0, &noWrites);
    frame.width = layout.Width;
    frame.height = layout.Height;
    return S_OK;
}

} // namespace muxivo::d3d12

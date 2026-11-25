#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace muxivo::d3d12 {

struct SdrFrame {
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> rgba;
};

class PresentReadback {
public:
    PresentReadback() = default;
    PresentReadback(const PresentReadback&) = delete;
    PresentReadback& operator=(const PresentReadback&) = delete;

    HRESULT record(IDXGISwapChain3& swapChain, DXGI_COLOR_SPACE_TYPE colorSpace,
        ID3D12GraphicsCommandList& commands, ID3D12Fence& fence, UINT64 completionValue) noexcept;

    HRESULT collect(SdrFrame& frame);
    HRESULT discardCompleted() noexcept;
    void forceReleaseSource() noexcept;

private:
    HRESULT releaseCompleted() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> source_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    Microsoft::WRL::ComPtr<IUnknown> bufferDevice_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT64 bufferBytes_ = 0;
    UINT64 completionValue_ = 0;
};

} // namespace muxivo::d3d12

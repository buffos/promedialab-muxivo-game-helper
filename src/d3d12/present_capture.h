#pragma once

#include "present_readback.h"

#include <chrono>
#include <memory>

namespace muxivo { class OverlappedFramePipeWriter; }

namespace muxivo::d3d12 {

inline constexpr wchar_t framePipePrefix[] = L"Muxivo.GameCapture.D3D12.v1";

class PresentCapture {
public:
    PresentCapture(HWND window, IDXGISwapChain3& swapChain) noexcept;
    ~PresentCapture();
    PresentCapture(const PresentCapture&) = delete;
    PresentCapture& operator=(const PresentCapture&) = delete;

    HRESULT prepare(DXGI_COLOR_SPACE_TYPE colorSpace, UINT flags,
        ID3D12GraphicsCommandList& commands, ID3D12Fence& fence, UINT64 completionValue) noexcept;
    void complete(HRESULT presentResult) noexcept;
    void publishCompleted() noexcept;
    void resetReadbackForResize() noexcept;
    void cancelUnqueuedCopy() noexcept;
    bool retire() noexcept;
    bool hasPendingCopy() const noexcept { return copy_ != Copy::None; }

private:
    enum class Copy { None, Recorded, Presented, Discarded };

    const HWND window_;
    IDXGISwapChain3& swapChain_;
    PresentReadback readback_;
    SdrFrame frame_;
    Copy copy_ = Copy::None;
    std::chrono::steady_clock::time_point nextCapture_{};
    std::unique_ptr<OverlappedFramePipeWriter> pipe_;
};

} // namespace muxivo::d3d12

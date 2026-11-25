#pragma once

#include "present_readback.h"

#include <chrono>
#include <memory>

namespace muxivo { class OverlappedFramePipeWriter; }

namespace muxivo::d3d9 {

inline constexpr wchar_t framePipePrefix[] = L"Muxivo.GameCapture.D3D9.v1";

class PresentCapture {
public:
    explicit PresentCapture(HWND window) noexcept;
    ~PresentCapture();
    PresentCapture(const PresentCapture&) = delete;
    PresentCapture& operator=(const PresentCapture&) = delete;

    void prepare(IDirect3DDevice9& device, IDirect3DSwapChain9& swapChain,
        HWND presentWindow, bool hasWindowOverride, DWORD presentFlags) noexcept;
    void complete(HRESULT result) noexcept;
    void resetForDeviceReset() noexcept;
    void retire() noexcept;

private:
    const HWND window_;
    PresentReadback readback_;
    SdrFrame staged_;
    bool prepared_ = false;
    std::chrono::steady_clock::time_point nextCapture_{};
    std::unique_ptr<OverlappedFramePipeWriter> pipe_;
};

} // namespace muxivo::d3d9

#include "present_capture.h"

#include "sdr_frame_packet.h"
#include "window_capture_affinity.h"

#include <utility>

namespace muxivo::d3d9 {

PresentCapture::PresentCapture(HWND window) noexcept : window_(window) {}
PresentCapture::~PresentCapture() = default;

void PresentCapture::prepare(IDirect3DDevice9& device, IDirect3DSwapChain9& swapChain,
    HWND presentWindow, bool hasWindowOverride, DWORD presentFlags) noexcept {
    prepared_ = false;
    if (presentWindow != window_ || !presentWindowAllowsCapture(window_)
        || (presentFlags & D3DPRESENT_LINEAR_CONTENT)) {
        retire();
        return;
    }
    if (presentFlags & D3DPRESENT_DONOTFLIP) return;
    try {
        const auto now = std::chrono::steady_clock::now();
        if (pipe_) {
            if (!pipe_->wantsFrame() || now < nextCapture_) {
                if (device.TestCooperativeLevel() != D3D_OK) retire();
                return;
            }
            nextCapture_ = now + std::chrono::milliseconds(12);
        }
        if (readback_.read(device, swapChain, window_, hasWindowOverride, staged_) != S_OK) {
            retire();
            return;
        }
        prepared_ = true;
    } catch (...) {
        retire();
    }
}

void PresentCapture::complete(HRESULT result) noexcept {
    const bool prepared = std::exchange(prepared_, false);
    if (!presentWindowAllowsCapture(window_)) {
        retire();
        return;
    }
    if (result == D3DERR_WASSTILLDRAWING) return;
    if (result != D3D_OK) {
        retire();
        return;
    }
    if (!prepared) return;
    try {
        if (!pipe_) {
            pipe_ = std::make_unique<OverlappedFramePipeWriter>(window_, framePipePrefix, sdrFramePipeCapacity);
            return;
        }
        publishSdrFrame(*pipe_, 3, staged_.width, staged_.height, staged_.rgba);
    } catch (...) {
        retire();
    }
}

void PresentCapture::resetForDeviceReset() noexcept {
    prepared_ = false;
    nextCapture_ = {};
    readback_.reset();
    staged_ = {};
}

void PresentCapture::retire() noexcept {
    prepared_ = false;
    nextCapture_ = {};
    pipe_.reset();
    readback_.reset();
    staged_ = {};
}

} // namespace muxivo::d3d9

#include "present_capture.h"

#include "sdr_frame_packet.h"
#include "window_capture_affinity.h"

namespace muxivo::d3d12 {

PresentCapture::PresentCapture(HWND window, IDXGISwapChain3& swapChain) noexcept
    : window_(window), swapChain_(swapChain) {}
PresentCapture::~PresentCapture() {
    readback_.forceReleaseSource();
}

HRESULT PresentCapture::prepare(DXGI_COLOR_SPACE_TYPE colorSpace, UINT flags,
    ID3D12GraphicsCommandList& commands, ID3D12Fence& fence, UINT64 completionValue) noexcept {
    HWND swapWindow = nullptr;
    if (FAILED(swapChain_.GetHwnd(&swapWindow)) || swapWindow != window_
        || !presentWindowAllowsCapture(window_)) {
        retire();
        return E_ACCESSDENIED;
    }
    if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        retire();
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE)) return S_FALSE;

    publishCompleted();
    if (copy_ != Copy::None) return S_FALSE;

    const auto now = std::chrono::steady_clock::now();
    if (pipe_) {
        if (!pipe_->wantsFrame() || now < nextCapture_) return S_FALSE;
        nextCapture_ = now + std::chrono::milliseconds(12);
    }

    const auto result = readback_.record(swapChain_, colorSpace, commands, fence, completionValue);
    if (result == S_OK) copy_ = Copy::Recorded;
    else if (FAILED(result)) retire();
    return result;
}

void PresentCapture::complete(HRESULT presentResult) noexcept {
    if (!presentWindowAllowsCapture(window_)) {
        retire();
    } else if (presentResult == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (copy_ == Copy::Recorded) copy_ = Copy::Discarded;
    } else if (presentResult != S_OK) {
        retire();
    } else if (copy_ == Copy::Recorded) {
        copy_ = Copy::Presented;
    }
}

void PresentCapture::publishCompleted() noexcept {
    if (!presentWindowAllowsCapture(window_)) {
        retire();
        return;
    }
    if (copy_ == Copy::Discarded) {
        if (readback_.discardCompleted() != S_FALSE) copy_ = Copy::None;
        return;
    }
    if (copy_ != Copy::Presented) return;
    try {
        const auto result = readback_.collect(frame_);
        if (result == S_FALSE) return;
        copy_ = Copy::None;
        if (FAILED(result) || !presentWindowAllowsCapture(window_)) {
            retire();
            return;
        }
        if (!pipe_) {
            pipe_ = std::make_unique<OverlappedFramePipeWriter>(window_, framePipePrefix, sdrFramePipeCapacity);
            return;
        }
        if (!pipe_->wantsFrame()) return;
        publishSdrFrame(*pipe_, 4, frame_.width, frame_.height, frame_.rgba);
    } catch (...) {
        retire();
    }
}

void PresentCapture::resetReadbackForResize() noexcept {
    nextCapture_ = {};
    frame_ = {};
    if (copy_ != Copy::None && readback_.discardCompleted() == S_FALSE) {
        readback_.forceReleaseSource();
    }
    copy_ = Copy::None;
}

void PresentCapture::cancelUnqueuedCopy() noexcept {
    if (copy_ == Copy::Recorded) {
        readback_.forceReleaseSource();
        copy_ = Copy::None;
    }
}

bool PresentCapture::retire() noexcept {
    nextCapture_ = {};
    pipe_.reset();
    frame_ = {};
    if (copy_ != Copy::None && readback_.discardCompleted() == S_FALSE) {
        copy_ = Copy::Discarded;
    } else {
        copy_ = Copy::None;
    }
    return copy_ == Copy::None;
}

} // namespace muxivo::d3d12

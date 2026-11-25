#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace muxivo { class OverlappedFramePipeWriter; }

namespace muxivo::opengl {

inline constexpr wchar_t framePipePrefix[] = L"Muxivo.GameCapture.OpenGL.v1";

// Owns one capture session on the rendering thread of a selected window.
// Call prepare before its main-plane SwapBuffers and complete with the swap result.
class SwapCapture {
public:
    explicit SwapCapture(HWND window) noexcept;
    ~SwapCapture();
    SwapCapture(const SwapCapture&) = delete;
    SwapCapture& operator=(const SwapCapture&) = delete;

    bool prepare(HDC dc) noexcept;
    void complete(BOOL swapped) noexcept;
    void retire() noexcept;

private:
    const HWND window_;
    HDC dc_ = nullptr;
    HGLRC context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    bool prepared_ = false;
    std::chrono::steady_clock::time_point nextCapture_{};
    std::vector<std::uint8_t> rgba_;
    std::unique_ptr<OverlappedFramePipeWriter> pipe_;
};

} // namespace muxivo::opengl

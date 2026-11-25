#include "swap_capture.h"

#include "sdr_frame_packet.h"
#include "window_capture_affinity.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

namespace muxivo::opengl {
namespace {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLubyte = unsigned char;

constexpr GLenum glPackSwapBytes = 0x0D00;
constexpr GLenum glPackLsbFirst = 0x0D01;
constexpr GLenum glPackRowLength = 0x0D02;
constexpr GLenum glPackSkipRows = 0x0D03;
constexpr GLenum glPackSkipPixels = 0x0D04;
constexpr GLenum glPackAlignment = 0x0D05;
constexpr GLenum glMapColor = 0x0D10;
constexpr GLenum glRedScale = 0x0D14;
constexpr GLenum glRedBias = 0x0D15;
constexpr GLenum glGreenScale = 0x0D18;
constexpr GLenum glGreenBias = 0x0D19;
constexpr GLenum glBlueScale = 0x0D1A;
constexpr GLenum glBlueBias = 0x0D1B;
constexpr GLenum glAlphaScale = 0x0D1C;
constexpr GLenum glAlphaBias = 0x0D1D;
constexpr GLenum glReadBuffer = 0x0C02;
constexpr GLenum glBack = 0x0405;
constexpr GLenum glRgba = 0x1908;
constexpr GLenum glUnsignedByte = 0x1401;
constexpr GLenum glVersion = 0x1F02;
constexpr GLenum glExtensions = 0x1F03;
constexpr GLenum glMajorVersion = 0x821B;
constexpr GLenum glMinorVersion = 0x821C;
constexpr GLenum glContextProfileMask = 0x9126;
constexpr GLint glCoreProfileBit = 1;
constexpr GLint glCompatibilityProfileBit = 2;
constexpr GLenum glReadFramebuffer = 0x8CA8;
constexpr GLenum glReadFramebufferBinding = 0x8CAA;
constexpr GLenum glPixelPackBuffer = 0x88EB;
constexpr GLenum glPixelPackBufferBinding = 0x88ED;

struct GlApi {
    HGLRC(WINAPI* wglGetCurrentContext)() = nullptr;
    HDC(WINAPI* wglGetCurrentDC)() = nullptr;
    PROC(WINAPI* wglGetProcAddress)(LPCSTR) = nullptr;

    void(APIENTRY* glGetIntegerv)(GLenum, GLint*) = nullptr;
    void(APIENTRY* glGetBooleanv)(GLenum, GLboolean*) = nullptr;
    void(APIENTRY* glGetFloatv)(GLenum, GLfloat*) = nullptr;
    GLboolean(APIENTRY* glIsEnabled)(GLenum) = nullptr;
    const GLubyte*(APIENTRY* glGetString)(GLenum) = nullptr;
    void(APIENTRY* glReadBuffer)(GLenum) = nullptr;
    void(APIENTRY* glPixelStorei)(GLenum, GLint) = nullptr;
    void(APIENTRY* glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    bool loaded = false;

    bool loadCore() noexcept {
        static std::once_flag once;
        std::call_once(once, [this]() noexcept {
            const HMODULE gl = GetModuleHandleW(L"opengl32.dll");
            if (!gl) return;
            wglGetCurrentContext = reinterpret_cast<decltype(wglGetCurrentContext)>(GetProcAddress(gl, "wglGetCurrentContext"));
            wglGetCurrentDC = reinterpret_cast<decltype(wglGetCurrentDC)>(GetProcAddress(gl, "wglGetCurrentDC"));
            wglGetProcAddress = reinterpret_cast<decltype(wglGetProcAddress)>(GetProcAddress(gl, "wglGetProcAddress"));
            glGetIntegerv = reinterpret_cast<decltype(glGetIntegerv)>(GetProcAddress(gl, "glGetIntegerv"));
            glGetBooleanv = reinterpret_cast<decltype(glGetBooleanv)>(GetProcAddress(gl, "glGetBooleanv"));
            glGetFloatv = reinterpret_cast<decltype(glGetFloatv)>(GetProcAddress(gl, "glGetFloatv"));
            glIsEnabled = reinterpret_cast<decltype(glIsEnabled)>(GetProcAddress(gl, "glIsEnabled"));
            glGetString = reinterpret_cast<decltype(glGetString)>(GetProcAddress(gl, "glGetString"));
            glReadBuffer = reinterpret_cast<decltype(glReadBuffer)>(GetProcAddress(gl, "glReadBuffer"));
            glPixelStorei = reinterpret_cast<decltype(glPixelStorei)>(GetProcAddress(gl, "glPixelStorei"));
            glReadPixels = reinterpret_cast<decltype(glReadPixels)>(GetProcAddress(gl, "glReadPixels"));
            loaded = wglGetCurrentContext && wglGetCurrentDC && wglGetProcAddress
                && glGetIntegerv && glGetBooleanv && glGetFloatv && glIsEnabled
                && glGetString && glReadBuffer && glPixelStorei && glReadPixels;
        });
        return loaded;
    }

    template <typename Entry>
    Entry entry(const char* name) const noexcept {
        if (!wglGetProcAddress) return nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(wglGetProcAddress(name));
        if (address <= 3 || address == UINTPTR_MAX) return nullptr;
        return reinterpret_cast<Entry>(address);
    }
};

GlApi& gl() noexcept {
    static GlApi api;
    return api;
}

using BindObject = void (APIENTRY*)(GLenum, GLuint);
using GetReadDc = HDC (WINAPI*)();

struct LastErrorGuard {
    const DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
};

bool sameReadDrawable(HDC dc) noexcept {
    auto currentReadDc = gl().entry<GetReadDc>("wglGetCurrentReadDCARB");
    if (!currentReadDc) currentReadDc = gl().entry<GetReadDc>("wglGetCurrentReadDCEXT");
    return !currentReadDc || currentReadDc() == dc;
}

bool identityImagingTransfer() noexcept {
    constexpr std::array<GLenum, 8> transforms = {
        0x80d0, 0x80d1, 0x80d2,
        0x8010, 0x8011, 0x8012,
        0x8024, 0x802e,
    };
    if (std::any_of(transforms.begin(), transforms.end(), [](GLenum state) { return gl().glIsEnabled(state) != 0; })) {
        return false;
    }
    constexpr GLenum colorMatrix = 0x80b1;
    constexpr std::array<GLfloat, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<GLfloat, 16> matrix{};
    gl().glGetFloatv(colorMatrix, matrix.data());
    if (matrix != identity) return false;
    for (GLenum parameter = 0x80b4; parameter <= 0x80bb; ++parameter) {
        GLfloat value = -1;
        gl().glGetFloatv(parameter, &value);
        if (value != (parameter < 0x80b8 ? 1.f : 0.f)) return false;
    }
    return true;
}

bool hasLegacyExtension(const char* ext) noexcept {
    const auto* extensions = reinterpret_cast<const char*>(gl().glGetString(glExtensions));
    if (!extensions || !ext || !ext[0]) return false;
    const std::size_t extLen = std::strlen(ext);
    const char* cursor = extensions;
    while ((cursor = std::strstr(cursor, ext)) != nullptr) {
        const bool startOk = (cursor == extensions || cursor[-1] == ' ');
        const char tail = cursor[extLen];
        const bool endOk = (tail == '\0' || tail == ' ');
        if (startOk && endOk) return true;
        cursor += extLen;
    }
    return false;
}

bool identityPixelTransfer() noexcept {
    GLboolean mapColor = 1;
    gl().glGetBooleanv(glMapColor, &mapColor);
    if (mapColor) return false;
    for (const GLenum parameter : {glRedScale, glGreenScale, glBlueScale, glAlphaScale}) {
        GLfloat value = 0;
        gl().glGetFloatv(parameter, &value);
        if (value != 1.f) return false;
    }
    for (const GLenum parameter : {glRedBias, glGreenBias, glBlueBias, glAlphaBias}) {
        GLfloat value = 1;
        gl().glGetFloatv(parameter, &value);
        if (value != 0.f) return false;
    }
    return !hasLegacyExtension("GL_ARB_imaging") || identityImagingTransfer();
}

bool supportedContext() noexcept {
    const auto* version = reinterpret_cast<const char*>(gl().glGetString(glVersion));
    if (!version || version[0] < '1' || version[0] > '9') return false;
    // OpenGL 1.x / 2.x uses fixed-function / compatibility pixel transfer.
    if (version[0] == '1' || version[0] == '2') {
        return identityPixelTransfer();
    }
    GLint major = 0, minor = 0;
    gl().glGetIntegerv(glMajorVersion, &major);
    gl().glGetIntegerv(glMinorVersion, &minor);
    if (major < 3 || (major == 3 && minor < 2)) {
        return identityPixelTransfer();
    }
    GLint profile = 0;
    gl().glGetIntegerv(glContextProfileMask, &profile);
    if ((profile & glCoreProfileBit) != 0) return true;
    return ((profile & glCompatibilityProfileBit) != 0 || profile == 0) && identityPixelTransfer();
}

bool supportedPixelFormat(HDC dc) noexcept {
    PIXELFORMATDESCRIPTOR format{};
    const int index = GetPixelFormat(dc);
    constexpr DWORD required = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    return index && DescribePixelFormat(dc, index, sizeof(format), &format)
        && (format.dwFlags & required) == required && !(format.dwFlags & PFD_STEREO)
        && !(format.dwFlags & PFD_GENERIC_FORMAT) && format.iPixelType == PFD_TYPE_RGBA
        && format.cRedBits == 8 && format.cGreenBits == 8 && format.cBlueBits == 8;
}

class ReadbackState {
public:
    ReadbackState(BindObject bindFramebuffer, BindObject bindBuffer) noexcept
        : bindFramebuffer_(bindFramebuffer), bindBuffer_(bindBuffer) {
        if (bindFramebuffer_) {
            gl().glGetIntegerv(glReadFramebufferBinding, &framebuffer_);
            bindFramebuffer_(glReadFramebuffer, 0);
        }
        gl().glGetIntegerv(glReadBuffer, &defaultReadBuffer_);
        gl().glReadBuffer(glBack);
        if (bindBuffer_) {
            gl().glGetIntegerv(glPixelPackBufferBinding, &buffer_);
            bindBuffer_(glPixelPackBuffer, 0);
        }
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            gl().glGetIntegerv(parameters[i], &pack_[i]);
            gl().glPixelStorei(parameters[i], i == 0 ? 1 : 0);
        }
    }

    ~ReadbackState() {
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            gl().glPixelStorei(parameters[i], pack_[i]);
        }
        if (bindBuffer_) {
            bindBuffer_(glPixelPackBuffer, static_cast<GLuint>(buffer_));
        }
        gl().glReadBuffer(static_cast<GLenum>(defaultReadBuffer_));
        if (bindFramebuffer_) {
            bindFramebuffer_(glReadFramebuffer, static_cast<GLuint>(framebuffer_));
        }
    }

private:
    static constexpr std::array<GLenum, 6> parameters = {
        glPackAlignment, glPackRowLength, glPackSkipRows, glPackSkipPixels, glPackSwapBytes, glPackLsbFirst};
    const BindObject bindFramebuffer_;
    const BindObject bindBuffer_;
    GLint framebuffer_ = 0;
    GLint buffer_ = 0;
    GLint defaultReadBuffer_ = glBack;
    std::array<GLint, parameters.size()> pack_{};
};

bool drawableSize(HWND window, UINT& width, UINT& height) noexcept {
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.right > 3840
        || client.bottom <= 0 || client.bottom > 2160) return false;
    width = static_cast<UINT>(client.right);
    height = static_cast<UINT>(client.bottom);
    return true;
}

} // namespace

SwapCapture::SwapCapture(HWND window) noexcept : window_(window) {}
SwapCapture::~SwapCapture() = default;

bool SwapCapture::prepare(HDC dc) noexcept {
    const LastErrorGuard lastError;
    prepared_ = false;
    if (!gl().loadCore()) {
        retire();
        return false;
    }

    UINT width = 0, height = 0;
    const HGLRC context = gl().wglGetCurrentContext();
    if (!dc || !context || gl().wglGetCurrentDC() != dc || WindowFromDC(dc) != window_
        || !presentWindowAllowsCapture(window_) || !drawableSize(window_, width, height)
        || !sameReadDrawable(dc) || !supportedPixelFormat(dc) || !supportedContext()) {
        retire();
        return false;
    }
    if (dc_ != dc || context_ != context) {
        retire();
        dc_ = dc;
        context_ = context;
        width_ = width;
        height_ = height;
    } else if (width_ != width || height_ != height) {
        // Preserve the connected frame pipe across window resizes.
        nextCapture_ = {};
        width_ = width;
        height_ = height;
    }
    try {
        const auto now = std::chrono::steady_clock::now();
        if (pipe_) {
            if (!pipe_->wantsFrame() || now < nextCapture_) return false;
            nextCapture_ = now + std::chrono::milliseconds(12);
        }
        const auto bindFramebuffer = gl().entry<BindObject>("glBindFramebuffer");
        const auto bindBuffer = gl().entry<BindObject>("glBindBuffer");
        const auto stride = static_cast<std::size_t>(width) * 4;
        rgba_.resize(stride * height);
        {
            const ReadbackState state(bindFramebuffer, bindBuffer);
            gl().glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                glRgba, glUnsignedByte, rgba_.data());
        }
        for (UINT row = 0; row < height / 2; ++row) {
            auto* begin = rgba_.data() + row * stride;
            std::swap_ranges(begin, begin + stride, rgba_.data() + (height - row - 1) * stride);
        }
        for (std::size_t alpha = 3; alpha < rgba_.size(); alpha += 4) rgba_[alpha] = 255;
        prepared_ = true;
        return true;
    } catch (...) {
        retire();
        return false;
    }
}

void SwapCapture::complete(BOOL swapped) noexcept {
    const LastErrorGuard lastError;
    const bool prepared = std::exchange(prepared_, false);
    if (!gl().loadCore()) {
        retire();
        return;
    }
    UINT width = 0, height = 0;
    if (!swapped || !presentWindowAllowsCapture(window_) || !drawableSize(window_, width, height)
        || gl().wglGetCurrentContext() != context_ || gl().wglGetCurrentDC() != dc_
        || !sameReadDrawable(dc_)) {
        retire();
        return;
    }
    if (width != width_ || height != height_) {
        // Window resized between prepare and complete: drop stale frame without breaking pipe.
        nextCapture_ = {};
        return;
    }
    if (!prepared) return;
    try {
        if (!pipe_) {
            pipe_ = std::make_unique<OverlappedFramePipeWriter>(window_, framePipePrefix, sdrFramePipeCapacity);
            return;
        }
        publishSdrFrame(*pipe_, 5, width, height, rgba_);
    } catch (...) {
        retire();
    }
}

void SwapCapture::retire() noexcept {
    prepared_ = false;
    nextCapture_ = {};
    dc_ = nullptr;
    context_ = nullptr;
    width_ = height_ = 0;
    pipe_.reset();
    rgba_ = {};
}

} // namespace muxivo::opengl

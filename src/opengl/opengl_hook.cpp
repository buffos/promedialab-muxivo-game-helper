#include "opengl_hook.h"

#include <windows.h>
#include <detours.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include "adapter_dll_bootstrap.h"
#include "adapter_session_config.h"
#include "detours_threads.h"
#include "swap_capture.h"

namespace muxivo::capture::opengl {
namespace {

using muxivo::opengl::SwapCapture;

using PfnSwapBuffers = BOOL(WINAPI*)(HDC);
using PfnWglSwapBuffers = BOOL(WINAPI*)(HDC);
using PfnWglSwapLayerBuffers = BOOL(WINAPI*)(HDC, UINT);

PfnSwapBuffers realGdiSwapBuffers = nullptr;
PfnWglSwapBuffers realWglSwapBuffers = nullptr;
PfnWglSwapLayerBuffers realWglSwapLayerBuffers = nullptr;

struct HookState {
    std::mutex mutex;
    SessionConfigClient config{ CaptureApi::OpenGL };
    HANDLE loadedEvent = nullptr;

    std::uint32_t lastGeneration = 0;
    HWND targetWindow = nullptr;
    std::unique_ptr<SwapCapture> capture;

    void retireCapture() noexcept {
        if (capture) capture->retire();
        capture.reset();
        targetWindow = nullptr;
    }
};

HookState& state() noexcept {
    // Intentionally immortal: this hook DLL stays pinned until process exit, and releasing
    // WGL state or waiting on overlapped I/O inside DLL_PROCESS_DETACH risks loader-lock deadlock/crash.
    static HookState* instance = new HookState();
    return *instance;
}

thread_local int g_swapDepth = 0;
std::atomic<bool> g_hooksReady{ false };

bool fastPathIdle() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return !s.capture && s.config.isIdleCached(std::chrono::steady_clock::now());
}

void clearOpenGlRealPointers() noexcept {
    realGdiSwapBuffers = nullptr;
    realWglSwapBuffers = nullptr;
    realWglSwapLayerBuffers = nullptr;
}

struct PreparedContext {
    bool active = false;
    std::uint32_t generation = 0;
};

PreparedContext prepareLocked(HDC hdc) noexcept {
    PreparedContext ctx;
    if (!hdc || fastPathIdle()) return ctx;
    const HWND window = WindowFromDC(hdc);
    if (!window) return ctx;

    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    bool justStopped = false;
    if (!s.config.checkHostRunning(now, justStopped)) {
        if (justStopped || s.capture) s.retireCapture();
        if (justStopped) s.lastGeneration = 0;
        return ctx;
    }

    ConfigSnapshot cfg = {};
    if (!s.config.readSnapshot(cfg) || !cfg.active || !cfg.targetHwnd) {
        s.retireCapture();
        return ctx;
    }

    if (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)) != cfg.targetHwnd) {
        if (cfg.generation != s.lastGeneration) {
            s.retireCapture();
            s.lastGeneration = cfg.generation;
        }
        return ctx;
    }

    if (cfg.generation != s.lastGeneration) {
        s.lastGeneration = cfg.generation;
        if (s.targetWindow != window) {
            s.retireCapture();
        }
    }

    try {
        if (!s.capture || s.targetWindow != window) {
            s.retireCapture();
            s.targetWindow = window;
            s.capture = std::make_unique<SwapCapture>(window);
        }

        s.capture->prepare(hdc);
        ctx.active = true;
        ctx.generation = cfg.generation;
    } catch (...) {
        s.retireCapture();
    }
    return ctx;
}

void finishLocked(const PreparedContext& ctx, BOOL swapped) noexcept {
    if (!ctx.active) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.capture || s.lastGeneration != ctx.generation) return;
    s.capture->complete(swapped);
}

BOOL WINAPI hookWglSwapBuffers(HDC hdc) {
    const bool topLevel = (g_swapDepth == 0);
    ReentrancyGuard guard(g_swapDepth);
    PreparedContext ctx;
    if (topLevel) ctx = prepareLocked(hdc);
    const BOOL swapped = realWglSwapBuffers(hdc);
    if (topLevel) finishLocked(ctx, swapped);
    return swapped;
}

BOOL WINAPI hookWglSwapLayerBuffers(HDC hdc, UINT planes) {
    const bool topLevel = (g_swapDepth == 0);
    ReentrancyGuard guard(g_swapDepth);
    PreparedContext ctx;
    if (topLevel && (planes & WGL_SWAP_MAIN_PLANE) != 0) ctx = prepareLocked(hdc);
    const BOOL swapped = realWglSwapLayerBuffers(hdc, planes);
    if (topLevel && (planes & WGL_SWAP_MAIN_PLANE) != 0) finishLocked(ctx, swapped);
    return swapped;
}

BOOL WINAPI hookGdiSwapBuffers(HDC hdc) {
    const bool topLevel = (g_swapDepth == 0);
    ReentrancyGuard guard(g_swapDepth);
    PreparedContext ctx;
    if (topLevel) ctx = prepareLocked(hdc);
    const BOOL swapped = realGdiSwapBuffers(hdc);
    if (topLevel) finishLocked(ctx, swapped);
    return swapped;
}

} // namespace

bool hooksReady() noexcept {
    return g_hooksReady.load(std::memory_order_acquire);
}

bool bootstrapHooks() noexcept {
    if (hooksReady()) return true;

    HANDLE loaded = nullptr;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.loadedEvent) {
            g_hooksReady.store(true, std::memory_order_release);
            return true;
        }
        if (!s.config.openConfigMapping()) return false;
        loaded = s.config.createLoadedEvent();
        if (!loaded) return false;
    }

    const HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
    const HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!opengl32) {
        CloseHandle(loaded);
        return false;
    }

    void* wglSwapAddr = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapBuffers"));
    void* wglSwapLayerAddr = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapLayerBuffers"));
    void* gdiSwapAddr = gdi32 ? reinterpret_cast<void*>(GetProcAddress(gdi32, "SwapBuffers")) : nullptr;
    if (!wglSwapAddr && !gdiSwapAddr) {
        CloseHandle(loaded);
        return false;
    }

    realWglSwapBuffers = reinterpret_cast<PfnWglSwapBuffers>(wglSwapAddr);
    realWglSwapLayerBuffers = reinterpret_cast<PfnWglSwapLayerBuffers>(wglSwapLayerAddr);
    realGdiSwapBuffers = reinterpret_cast<PfnSwapBuffers>(gdiSwapAddr);

    if (DetourTransactionBegin() != NO_ERROR) {
        clearOpenGlRealPointers();
        CloseHandle(loaded);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    const auto threads = updateOtherProcessThreads();

    bool attached = true;
    if (realWglSwapBuffers) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realWglSwapBuffers), reinterpret_cast<PVOID>(hookWglSwapBuffers)) == NO_ERROR;
    }
    if (attached && realWglSwapLayerBuffers && wglSwapLayerAddr != wglSwapAddr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realWglSwapLayerBuffers), reinterpret_cast<PVOID>(hookWglSwapLayerBuffers)) == NO_ERROR;
    } else {
        realWglSwapLayerBuffers = nullptr;
    }
    if (attached && realGdiSwapBuffers && gdiSwapAddr != wglSwapAddr && gdiSwapAddr != wglSwapLayerAddr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realGdiSwapBuffers), reinterpret_cast<PVOID>(hookGdiSwapBuffers)) == NO_ERROR;
    } else {
        realGdiSwapBuffers = nullptr;
    }

    const LONG commit = attached ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!attached) DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (commit != NO_ERROR) {
        clearOpenGlRealPointers();
        CloseHandle(loaded);
        return false;
    }

    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.loadedEvent = loaded;
        SetEvent(loaded);
        g_hooksReady.store(true, std::memory_order_release);
    }
    return true;
}

} // namespace muxivo::capture::opengl

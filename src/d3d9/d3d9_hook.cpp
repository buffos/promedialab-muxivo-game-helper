#include "d3d9_hook.h"

#include <windows.h>
#include <d3d9.h>
#include <detours.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include "adapter_dll_bootstrap.h"
#include "adapter_session_config.h"
#include "detours_threads.h"
#include "module_image.h"
#include "present_capture.h"

namespace muxivo::capture::d3d9 {
namespace {

using Microsoft::WRL::ComPtr;
using muxivo::d3d9::PresentCapture;

using PfnDeviceReset = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PfnDevicePresent = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PfnDevicePresentEx = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using PfnDeviceResetEx = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using PfnSwapChainPresent = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

PfnDeviceReset realDeviceReset = nullptr;
PfnDevicePresent realDevicePresent = nullptr;
PfnDevicePresentEx realDevicePresentEx = nullptr;
PfnDeviceResetEx realDeviceResetEx = nullptr;
PfnSwapChainPresent realSwapChainPresent = nullptr;

struct HookState {
    std::mutex mutex;
    SessionConfigClient config{ CaptureApi::D3D9 };
    HANDLE loadedEvent = nullptr;

    std::uint32_t lastGeneration = 0;
    HWND targetWindow = nullptr;
    std::unique_ptr<PresentCapture> capture;

    void retireCapture() noexcept {
        if (capture) capture->retire();
        capture.reset();
        targetWindow = nullptr;
    }
};

void clearD3D9RealPointers() noexcept {
    realDeviceReset = nullptr;
    realDevicePresent = nullptr;
    realDevicePresentEx = nullptr;
    realDeviceResetEx = nullptr;
    realSwapChainPresent = nullptr;
}

HookState& state() noexcept {
    // Intentionally immortal: this hook DLL stays pinned until process exit, and releasing
    // D3D COM objects or waiting on overlapped I/O inside DLL_PROCESS_DETACH risks loader-lock deadlock/crash.
    static HookState* instance = new HookState();
    return *instance;
}

thread_local int g_presentDepth = 0;
std::atomic<bool> g_hooksReady{ false };
std::mutex g_d3d9ResetHookMutex;
std::atomic<bool> g_resetHookChecked{ false };

bool fastPathIdle() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return !s.capture && s.config.isIdleCached(std::chrono::steady_clock::now());
}

HWND resolveSwapChainWindow(IDirect3DDevice9* device, IDirect3DSwapChain9* swapChain, HWND overrideWindow) noexcept {
    if (overrideWindow) return overrideWindow;
    if (swapChain) {
        D3DPRESENT_PARAMETERS params = {};
        if (SUCCEEDED(swapChain->GetPresentParameters(&params)) && params.hDeviceWindow) {
            return params.hDeviceWindow;
        }
    }
    if (device) {
        D3DDEVICE_CREATION_PARAMETERS creation = {};
        if (SUCCEEDED(device->GetCreationParameters(&creation))) {
            return creation.hFocusWindow;
        }
    }
    return nullptr;
}

struct PreparedContext {
    bool active = false;
    std::uint32_t generation = 0;
};

PreparedContext prepareLocked(
    IDirect3DDevice9* device,
    IDirect3DSwapChain9* swapChain,
    HWND overrideWindow,
    DWORD presentFlags) noexcept {
    PreparedContext ctx;
    if (!device || !swapChain) return ctx;

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

    const HWND presentWindow = resolveSwapChainWindow(device, swapChain, overrideWindow);
    if (!presentWindow
        || static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(presentWindow)) != cfg.targetHwnd) {
        if (cfg.generation != s.lastGeneration) {
            s.retireCapture();
            s.lastGeneration = cfg.generation;
        }
        return ctx;
    }

    if (cfg.generation != s.lastGeneration) {
        s.lastGeneration = cfg.generation;
        if (s.targetWindow != presentWindow) {
            s.retireCapture();
        }
    }

    try {
        if (!s.capture || s.targetWindow != presentWindow) {
            s.retireCapture();
            s.targetWindow = presentWindow;
            s.capture = std::make_unique<PresentCapture>(presentWindow);
        }

        s.capture->prepare(*device, *swapChain, presentWindow, overrideWindow != nullptr, presentFlags);
        ctx.active = true;
        ctx.generation = cfg.generation;
    } catch (...) {
        s.retireCapture();
    }
    return ctx;
}

void finishLocked(const PreparedContext& ctx, HRESULT presentResult) noexcept {
    if (!ctx.active) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.capture || s.lastGeneration != ctx.generation) return;
    s.capture->complete(presentResult);
}

void prepareForDeviceReset() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.capture) s.capture->resetForDeviceReset();
}

void retireAfterFailedReset() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.retireCapture();
}

HRESULT STDMETHODCALLTYPE hookDeviceReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    prepareForDeviceReset();
    const HRESULT hr = realDeviceReset(device, params);
    if (FAILED(hr)) retireAfterFailedReset();
    return hr;
}

HRESULT STDMETHODCALLTYPE hookDeviceResetEx(
    IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* fullscreenMode) {
    prepareForDeviceReset();
    const HRESULT hr = realDeviceResetEx(device, params, fullscreenMode);
    if (FAILED(hr)) retireAfterFailedReset();
    return hr;
}

void ensureResetHooksOnDevice(IDirect3DDevice9* device) noexcept {
    if (!device || g_resetHookChecked.load(std::memory_order_acquire)) return;
    std::lock_guard resetLock(g_d3d9ResetHookMutex);
    if (g_resetHookChecked.load(std::memory_order_relaxed)) return;

    const auto* vt = *reinterpret_cast<const std::uintptr_t* const*>(device);
    if (!vt) return;

    void* resetAddr = realDeviceReset ? nullptr : reinterpret_cast<void*>(vt[16]);
    void* resetExAddr = nullptr;
    ComPtr<IDirect3DDevice9Ex> devEx;
    if (!realDeviceResetEx && SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
            reinterpret_cast<void**>(devEx.GetAddressOf()))) && devEx) {
        const auto* exVt = *reinterpret_cast<const std::uintptr_t* const*>(devEx.Get());
        if (exVt) resetExAddr = reinterpret_cast<void*>(exVt[132]);
    }
    if (!resetAddr && !resetExAddr) {
        g_resetHookChecked.store(true, std::memory_order_release);
        return;
    }

    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    const auto threads = updateOtherProcessThreads();

    bool ok = true;
    bool addedReset = false;
    bool addedResetEx = false;
    if (resetAddr) {
        realDeviceReset = reinterpret_cast<PfnDeviceReset>(resetAddr);
        ok = DetourAttach(&reinterpret_cast<PVOID&>(realDeviceReset), reinterpret_cast<PVOID>(hookDeviceReset)) == NO_ERROR;
        addedReset = ok;
    }
    if (ok && resetExAddr && resetExAddr != reinterpret_cast<void*>(vt[16]) && resetExAddr != resetAddr) {
        realDeviceResetEx = reinterpret_cast<PfnDeviceResetEx>(resetExAddr);
        ok = DetourAttach(&reinterpret_cast<PVOID&>(realDeviceResetEx), reinterpret_cast<PVOID>(hookDeviceResetEx)) == NO_ERROR;
        addedResetEx = ok;
    }
    const LONG commit = ok ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!ok || commit != NO_ERROR) {
        if (!ok) DetourTransactionAbort();
        if (addedReset || resetAddr) realDeviceReset = nullptr;
        if (addedResetEx || resetExAddr) realDeviceResetEx = nullptr;
    }
    if (commit == NO_ERROR) {
        g_resetHookChecked.store(true, std::memory_order_release);
    }
    for (HANDLE thread : threads) CloseHandle(thread);
}

HRESULT STDMETHODCALLTYPE hookDevicePresent(
    IDirect3DDevice9* device,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND destWindowOverride,
    const RGNDATA* dirtyRegion) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && device) {
        ensureResetHooksOnDevice(device);
        if (!fastPathIdle()) {
            ComPtr<IDirect3DSwapChain9> swapChain;
            if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
                ctx = prepareLocked(device, swapChain.Get(), destWindowOverride, 0);
            }
        }
    }
    const HRESULT hr = realDevicePresent(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
    if (topLevel) finishLocked(ctx, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookDevicePresentEx(
    IDirect3DDevice9Ex* device,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND destWindowOverride,
    const RGNDATA* dirtyRegion,
    DWORD flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && device) {
        ensureResetHooksOnDevice(device);
        if (!fastPathIdle()) {
            ComPtr<IDirect3DSwapChain9> swapChain;
            if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
                ctx = prepareLocked(device, swapChain.Get(), destWindowOverride, flags);
            }
        }
    }
    const HRESULT hr = realDevicePresentEx(device, sourceRect, destRect, destWindowOverride, dirtyRegion, flags);
    if (topLevel) finishLocked(ctx, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookSwapChainPresent(
    IDirect3DSwapChain9* swapChain,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND destWindowOverride,
    const RGNDATA* dirtyRegion,
    DWORD flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && swapChain && !fastPathIdle()) {
        ComPtr<IDirect3DDevice9> device;
        if (SUCCEEDED(swapChain->GetDevice(&device)) && device) {
            ensureResetHooksOnDevice(device.Get());
            ctx = prepareLocked(device.Get(), swapChain, destWindowOverride, flags);
        }
    }
    const HRESULT hr = realSwapChainPresent(swapChain, sourceRect, destRect, destWindowOverride, dirtyRegion, flags);
    if (topLevel) finishLocked(ctx, hr);
    return hr;
}

} // namespace

bool hooksReady() noexcept {
    return g_hooksReady.load(std::memory_order_acquire);
}

bool bootstrapHooks() noexcept {
    if (hooksReady()) return true;

    std::uint32_t resetRva = 0;
    std::uint32_t presentRva = 0;
    std::uint32_t presentExRva = 0;
    std::uint32_t resetExRva = 0;
    std::uint32_t swapPresentRva = 0;
    HANDLE loaded = nullptr;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.loadedEvent) {
            g_hooksReady.store(true, std::memory_order_release);
            return true;
        }
        if (!s.config.openConfigMapping()) return false;
        resetRva = s.config.readRva(0);
        presentRva = s.config.readRva(1);
        presentExRva = s.config.readRva(2);
        resetExRva = s.config.readRva(3);
        swapPresentRva = s.config.readRva(4);
        loaded = s.config.createLoadedEvent();
        if (!loaded) return false;
    }

    const HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    ModuleImageRange d3d9Range = {};
    if (!readModuleImageRange(d3d9, d3d9Range)) {
        CloseHandle(loaded);
        return false;
    }

    auto validateRva = [&](std::uint32_t rva) noexcept -> void* {
        if (rva == 0 || rva >= d3d9Range.size) return nullptr;
        return reinterpret_cast<void*>(d3d9Range.base + rva);
    };

    void* presentAddr = validateRva(presentRva);
    void* swapPresentAddr = validateRva(swapPresentRva);
    void* resetAddr = resetRva ? validateRva(resetRva) : nullptr;
    void* presentExAddr = presentExRva ? validateRva(presentExRva) : nullptr;
    void* resetExAddr = resetExRva ? validateRva(resetExRva) : nullptr;
    if (!presentAddr || !swapPresentAddr) {
        CloseHandle(loaded);
        return false;
    }

    realDeviceReset = reinterpret_cast<PfnDeviceReset>(resetAddr);
    realDevicePresent = reinterpret_cast<PfnDevicePresent>(presentAddr);
    realSwapChainPresent = reinterpret_cast<PfnSwapChainPresent>(swapPresentAddr);
    realDevicePresentEx = reinterpret_cast<PfnDevicePresentEx>(presentExAddr);
    realDeviceResetEx = reinterpret_cast<PfnDeviceResetEx>(resetExAddr);

    if (DetourTransactionBegin() != NO_ERROR) {
        clearD3D9RealPointers();
        CloseHandle(loaded);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    const auto threads = updateOtherProcessThreads();

    bool attached =
        DetourAttach(&reinterpret_cast<PVOID&>(realDevicePresent), reinterpret_cast<PVOID>(hookDevicePresent)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realSwapChainPresent), reinterpret_cast<PVOID>(hookSwapChainPresent)) == NO_ERROR;

    if (attached && realDeviceReset) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realDeviceReset), reinterpret_cast<PVOID>(hookDeviceReset)) == NO_ERROR;
    }

    if (attached && realDevicePresentEx && presentExAddr != presentAddr && presentExAddr != swapPresentAddr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realDevicePresentEx), reinterpret_cast<PVOID>(hookDevicePresentEx)) == NO_ERROR;
    } else {
        realDevicePresentEx = nullptr;
    }

    if (attached && realDeviceResetEx && resetExAddr != resetAddr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realDeviceResetEx), reinterpret_cast<PVOID>(hookDeviceResetEx)) == NO_ERROR;
    } else {
        realDeviceResetEx = nullptr;
    }

    const LONG commit = attached ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!attached) DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (commit != NO_ERROR) {
        clearD3D9RealPointers();
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

} // namespace muxivo::capture::d3d9

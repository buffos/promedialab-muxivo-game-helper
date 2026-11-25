#include "dxgi_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <detours.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "d3d11_protocol.h"
#include "d3d11_readback.h"
#include "detours_threads.h"
#include "module_image.h"
#include "overlapped_frame_pipe_writer.h"
#include "security_descriptor.h"

namespace muxivo::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT protectedSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY
    | DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT
    | DXGI_SWAP_CHAIN_FLAG_RESTRICT_SHARED_RESOURCE_DRIVER
    | DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;

using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PfnResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

PfnPresent realPresent = nullptr;
PfnPresent realSecondaryPresent = nullptr;
PfnPresent1 realPresent1 = nullptr;
PfnResizeBuffers realResizeBuffers = nullptr;
PfnResizeBuffers1 realResizeBuffers1 = nullptr;

void clearRealPointers() noexcept {
    realPresent = nullptr;
    realSecondaryPresent = nullptr;
    realPresent1 = nullptr;
    realResizeBuffers = nullptr;
    realResizeBuffers1 = nullptr;
}

struct HookState {
    std::mutex mutex;
    HANDLE configMapping = nullptr;
    const D3D11HookConfig* configView = nullptr;
    HANDLE loadedEvent = nullptr;

    bool hostRunning = false;
    std::chrono::steady_clock::time_point nextPresenceCheck;
    std::chrono::steady_clock::time_point nextCapture;

    std::uint32_t lastGeneration = 0;
    HWND targetWindow = nullptr;
    void* activeSwap = nullptr;
    std::unordered_set<void*> destructionNotified;
    bool permissionDiagnostic = false;

    std::unique_ptr<OverlappedFramePipeWriter> pipe;
    D3D11Readback readback;

    void suspendCapture() noexcept {
        pipe.reset();
        readback.reset();
        activeSwap = nullptr;
        targetWindow = nullptr;
    }

    void closeConfig() noexcept {
        if (configView) {
            UnmapViewOfFile(configView);
            configView = nullptr;
        }
        if (configMapping) {
            CloseHandle(configMapping);
            configMapping = nullptr;
        }
        lastGeneration = 0;
    }
};

HookState& state() noexcept {
    // Intentionally immortal: this hook DLL stays pinned until process exit, and releasing
    // D3D COM objects or waiting on overlapped I/O inside DLL_PROCESS_DETACH risks loader-lock deadlock/crash.
    static HookState* instance = new HookState();
    return *instance;
}

thread_local int g_presentDepth = 0;
std::atomic<bool> g_hooksReady{ false };

struct ReentrancyGuard {
    int& depth;
    explicit ReentrancyGuard(int& d) noexcept : depth(d) { ++depth; }
    ~ReentrancyGuard() { --depth; }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
};

bool traceEnabled() noexcept {
    static const bool enabled = GetEnvironmentVariableW(L"MUXIVO_D3D11_CAPTURE_TRACE", nullptr, 0) != 0;
    return enabled;
}

void traceLog(const char* fmt, ...) noexcept {
    if (!traceEnabled()) return;
    char msg[512] = {};
    va_list args;
    va_start(args, fmt);
    const int len = std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (len <= 0) return;
    const std::size_t clamped = std::min<std::size_t>(static_cast<std::size_t>(len), sizeof(msg) - 1);
    msg[clamped] = '\0';
    OutputDebugStringA(msg);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(err, msg, static_cast<DWORD>(clamped), &written, nullptr);
    }
}

bool windowPermitsCapture(HWND window) noexcept {
    DWORD owner = 0;
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)
        || !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId()) {
        return false;
    }
    const HWND root = GetAncestor(window, GA_ROOT);
    if (!root || IsIconic(root)) return false;
    DWORD affinity = WDA_NONE;
    return GetWindowDisplayAffinity(root, &affinity) && affinity == WDA_NONE;
}

bool swapChainAllowsCapture(IDXGISwapChain* swap) noexcept {
    if (!swap) return false;
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(swap1.GetAddressOf())))) {
        DXGI_SWAP_CHAIN_DESC1 desc1 = {};
        return SUCCEEDED(swap1->GetDesc1(&desc1)) && (desc1.Flags & protectedSwapChainFlags) == 0;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    return SUCCEEDED(swap->GetDesc(&desc)) && (desc.Flags & protectedSwapChainFlags) == 0;
}

bool openConfigMappingLocked(HookState& s) noexcept {
    if (s.configView) return true;
    const std::wstring name = d3d11ConfigMappingName(GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return false;
    const auto* view = static_cast<const D3D11HookConfig*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(D3D11HookConfig)));
    if (!view || view->magic != muxivoD3D11ConfigMagic
        || view->version != muxivoD3D11ConfigVersion
        || view->targetPid != GetCurrentProcessId()) {
        if (view) UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }
    s.configMapping = mapping;
    s.configView = view;
    return true;
}

bool readConfigSnapshot(
    const D3D11HookConfig& cfg,
    bool& activeOut,
    std::uint64_t& hwndOut,
    std::uint32_t& generationOut) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t genBefore = readConfigU32(cfg.generation);
        const std::uint32_t active = readConfigU32(cfg.active);
        const std::uint64_t hwnd = readConfigU64(cfg.targetHwnd);
        const std::uint32_t genAfter = readConfigU32(cfg.generation);
        if (genBefore == genAfter) {
            activeOut = (active != 0);
            hwndOut = hwnd;
            generationOut = genBefore;
            return true;
        }
    }
    return false;
}

bool hostIsRunningLocked(HookState& s, std::chrono::steady_clock::time_point now) noexcept {
    if (now < s.nextPresenceCheck) return s.hostRunning;
    const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, muxivoD3D11PresenceEvent);
    const bool running = event != nullptr;
    if (event) CloseHandle(event);

    s.nextPresenceCheck = now + std::chrono::milliseconds(running ? 250 : 50);
    if (running && !s.configView) {
        openConfigMappingLocked(s);
    } else if (!running && s.hostRunning) {
        s.suspendCapture();
        s.closeConfig();
        s.permissionDiagnostic = false;
    } else if (!running) {
        s.closeConfig();
    }
    s.hostRunning = running;
    return s.hostRunning;
}

HWND swapChainHwnd(IDXGISwapChain* swap) noexcept {
    if (!swap) return nullptr;
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(swap1.GetAddressOf())))) {
        HWND hwnd = nullptr;
        if (SUCCEEDED(swap1->GetHwnd(&hwnd))) return hwnd;
        // DirectComposition or CoreWindow swap chains have no HWND target.
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swap->GetDesc(&desc))) return desc.OutputWindow;
    return nullptr;
}

void STDMETHODCALLTYPE onSwapChainDestroyed(void* data) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.destructionNotified.erase(data);
    if (s.activeSwap == data) {
        s.readback.reset();
        s.activeSwap = nullptr;
    }
}

void ensureDestructionCallbackLocked(HookState& s, IDXGISwapChain* swap) noexcept {
    if (s.destructionNotified.count(swap)) return;
    ComPtr<ID3DDestructionNotifier> notifier;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(ID3DDestructionNotifier), reinterpret_cast<void**>(notifier.GetAddressOf())))) {
        UINT cookie = 0;
        if (SUCCEEDED(notifier->RegisterDestructionCallback(&onSwapChainDestroyed, swap, &cookie))) {
            try { s.destructionNotified.insert(swap); }
            catch (...) {}
        }
    }
}

void prepareAndCapture(
    IDXGISwapChain* swap,
    HWND& outWindow,
    std::shared_ptr<std::vector<std::uint8_t>>& outPacket) {
    outWindow = nullptr;
    outPacket.reset();

    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!hostIsRunningLocked(s, now) || !s.configView) return;

    bool active = false;
    std::uint64_t expectedHwnd = 0;
    std::uint32_t generation = 0;
    if (!readConfigSnapshot(*s.configView, active, expectedHwnd, generation) || !active || !expectedHwnd) {
        s.suspendCapture();
        return;
    }

    const HWND hwnd = swapChainHwnd(swap);
    if (!hwnd || reinterpret_cast<std::uint64_t>(hwnd) != expectedHwnd) {
        if (generation != s.lastGeneration) {
            s.suspendCapture();
            s.lastGeneration = generation;
            s.nextCapture = {};
            s.permissionDiagnostic = false;
        }
        return;
    }

    if (generation != s.lastGeneration) {
        s.lastGeneration = generation;
        if (s.targetWindow != hwnd) {
            s.suspendCapture();
        }
        s.nextCapture = {};
        s.permissionDiagnostic = false;
    }

    if (!windowPermitsCapture(hwnd) || !swapChainAllowsCapture(swap)) {
        if (!s.permissionDiagnostic) {
            s.permissionDiagnostic = true;
            DWORD affinity = 0;
            const HWND root = GetAncestor(hwnd, GA_ROOT);
            const BOOL readable = root ? GetWindowDisplayAffinity(root, &affinity) : FALSE;
            traceLog("[Muxivo D3D11] Capture guard HWND=%p visible=%d iconic=%d affinity=%lx err=%lu\n",
                static_cast<void*>(hwnd), IsWindowVisible(hwnd), IsIconic(hwnd), affinity,
                readable ? ERROR_SUCCESS : GetLastError());
        }
        s.suspendCapture();
        return;
    }
    s.permissionDiagnostic = false;

    ComPtr<ID3D11Device> device;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()))) || !device) {
        traceLog("[Muxivo D3D11] Swapchain has no ID3D11Device\n");
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return;

    if (s.activeSwap != swap || s.targetWindow != hwnd) {
        traceLog("[Muxivo D3D11] Swapchain pointer changed %p -> %p\n", s.activeSwap, static_cast<void*>(swap));
        s.readback.reset();
        s.activeSwap = swap;
        s.targetWindow = hwnd;
    }
    ensureDestructionCallbackLocked(s, swap);

    if (!s.pipe) {
        try {
            s.pipe = std::make_unique<OverlappedFramePipeWriter>(hwnd, muxivoD3D11PipeEndpoint, 12 * 1024 * 1024);
            traceLog("[Muxivo D3D11] Frame endpoint ready for HWND %p\n", static_cast<void*>(hwnd));
        } catch (const std::exception& error) {
            traceLog("[Muxivo D3D11] OverlappedFramePipeWriter failed: %s\n", error.what());
            s.suspendCapture();
            return;
        } catch (...) {
            s.suspendCapture();
            return;
        }
    }

    outWindow = hwnd;
    if (!s.pipe->wantsFrame() || now < s.nextCapture) return;
    s.nextCapture = now + std::chrono::milliseconds(12);

    ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT bufHr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()));
    if (FAILED(bufHr) || !backBuffer) {
        traceLog("[Muxivo D3D11] GetBuffer(0) failed hr=0x%lx\n", static_cast<unsigned long>(bufHr));
        s.readback.reset();
        return;
    }

    auto buffer = s.pipe->acquireFrameBuffer();
    if (!s.readback.copy(device.Get(), context.Get(), backBuffer.Get(), *buffer)) {
        return;
    }
    outPacket = std::move(buffer);
}

void finishCapture(
    HWND targetWindow,
    HRESULT presentResult,
    const std::shared_ptr<const std::vector<std::uint8_t>>& packet) noexcept {
    if (!targetWindow) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (FAILED(presentResult)) {
        traceLog("[Muxivo D3D11] Present failed hr=0x%lx\n", static_cast<unsigned long>(presentResult));
        s.readback.reset();
        return;
    }
    if (packet && !packet->empty() && s.pipe) {
        try {
            if (windowPermitsCapture(targetWindow)) {
                s.pipe->publish(packet);
            } else {
                traceLog("[Muxivo D3D11] finishCapture windowPermitsCapture=false\n");
                s.suspendCapture();
            }
        } catch (...) {
            s.suspendCapture();
        }
    }
}

void onResize(IDXGISwapChain* swap) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    traceLog("[Muxivo D3D11] onResize swap=%p active=%p\n", static_cast<void*>(swap), s.activeSwap);
    if (!s.activeSwap || s.activeSwap == swap) {
        s.readback.reset();
    }
}

HRESULT STDMETHODCALLTYPE hookPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    HWND targetWindow = nullptr;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        prepareAndCapture(swap, targetWindow, packet);
    }
    const HRESULT hr = realPresent(swap, syncInterval, flags);
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(targetWindow, hr, packet);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookSecondaryPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    HWND targetWindow = nullptr;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        prepareAndCapture(swap, targetWindow, packet);
    }
    const HRESULT hr = realSecondaryPresent(swap, syncInterval, flags);
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(targetWindow, hr, packet);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookPresent1(
    IDXGISwapChain1* swap, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    HWND targetWindow = nullptr;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        prepareAndCapture(swap, targetWindow, packet);
    }
    const HRESULT hr = realPresent1(swap, syncInterval, flags, params);
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(targetWindow, hr, packet);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookResizeBuffers(
    IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    onResize(swap);
    return realResizeBuffers(swap, bufferCount, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE hookResizeBuffers1(
    IDXGISwapChain3* swap, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
    const UINT* creationNodeMask, IUnknown* const* presentQueue) {
    onResize(swap);
    return realResizeBuffers1(swap, bufferCount, width, height, format, flags, creationNodeMask, presentQueue);
}

} // namespace

bool hooksReady() noexcept {
    return g_hooksReady.load(std::memory_order_acquire);
}

bool bootstrapHooks() noexcept {
    if (hooksReady()) return true;
    std::uint32_t presentRva = 0;
    std::uint32_t present1Rva = 0;
    std::uint32_t resizeRva = 0;
    std::uint32_t resize1Rva = 0;
    std::uint32_t secondaryPresentRva = 0;
    HANDLE loaded = nullptr;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.loadedEvent) {
            g_hooksReady.store(true, std::memory_order_release);
            return true;
        }
        if (!openConfigMappingLocked(s)) return false;
        presentRva = readConfigU32(s.configView->presentRva);
        present1Rva = readConfigU32(s.configView->present1Rva);
        resizeRva = readConfigU32(s.configView->resizeBuffersRva);
        resize1Rva = readConfigU32(s.configView->resizeBuffers1Rva);
        secondaryPresentRva = readConfigU32(s.configView->secondaryPresentRva);

        const auto security = tryCreateCurrentUserSecurityDescriptor();
        if (!security) return false;
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        const std::wstring loadedName = d3d11LoadedEventName(GetCurrentProcessId());
        loaded = CreateEventW(&attributes, TRUE, FALSE, loadedName.c_str());
        if (!loaded) return false;
    }

    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    muxivo::capture::ModuleImageRange dxgiRange = {};
    if (!muxivo::capture::readModuleImageRange(dxgi, dxgiRange)) {
        CloseHandle(loaded);
        return false;
    }

    auto validateRva = [&](std::uint32_t rva) noexcept -> void* {
        if (rva == 0 || rva >= dxgiRange.size) return nullptr;
        return reinterpret_cast<void*>(dxgiRange.base + rva);
    };

    void* presentAddr = validateRva(presentRva);
    void* present1Addr = validateRva(present1Rva);
    void* resizeAddr = validateRva(resizeRva);
    void* resize1Addr = resize1Rva ? validateRva(resize1Rva) : nullptr;
    void* secondaryPresentAddr = secondaryPresentRva ? validateRva(secondaryPresentRva) : nullptr;
    if (!presentAddr || !present1Addr || !resizeAddr) {
        CloseHandle(loaded);
        return false;
    }

    realPresent = reinterpret_cast<PfnPresent>(presentAddr);
    realPresent1 = reinterpret_cast<PfnPresent1>(present1Addr);
    realResizeBuffers = reinterpret_cast<PfnResizeBuffers>(resizeAddr);
    realResizeBuffers1 = reinterpret_cast<PfnResizeBuffers1>(resize1Addr);
    realSecondaryPresent = reinterpret_cast<PfnPresent>(secondaryPresentAddr);

    if (DetourTransactionBegin() != NO_ERROR) {
        clearRealPointers();
        CloseHandle(loaded);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    const auto threads = muxivo::capture::updateOtherProcessThreads();

    bool attached =
        DetourAttach(&reinterpret_cast<PVOID&>(realPresent), reinterpret_cast<PVOID>(hookPresent)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realPresent1), reinterpret_cast<PVOID>(hookPresent1)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realResizeBuffers), reinterpret_cast<PVOID>(hookResizeBuffers)) == NO_ERROR;

    if (attached && realResizeBuffers1 && resize1Addr != resizeAddr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realResizeBuffers1), reinterpret_cast<PVOID>(hookResizeBuffers1)) == NO_ERROR;
    } else {
        realResizeBuffers1 = nullptr;
    }

    if (attached && realSecondaryPresent && secondaryPresentAddr != presentAddr && secondaryPresentAddr != present1Addr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realSecondaryPresent), reinterpret_cast<PVOID>(hookSecondaryPresent)) == NO_ERROR;
    } else {
        realSecondaryPresent = nullptr;
    }

    const LONG commit = attached ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!attached) DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (commit != NO_ERROR) {
        clearRealPointers();
        CloseHandle(loaded);
        traceLog("[Muxivo D3D11] DetourTransactionCommit failed (%ld)\n", commit);
        return false;
    }

    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.loadedEvent = loaded;
    SetEvent(loaded);
    g_hooksReady.store(true, std::memory_order_release);
    traceLog("[Muxivo D3D11] Installed DXGI hooks for PID %lu (present=%x present1=%x secondary=%x)\n",
        GetCurrentProcessId(), presentRva, present1Rva, secondaryPresentRva);
    return true;
}

} // namespace muxivo::d3d11

#include "d3d12_hook.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <detours.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "adapter_dll_bootstrap.h"
#include "adapter_session_config.h"
#include "detours_threads.h"
#include "module_image.h"
#include "present_capture.h"
#include "security_descriptor.h"

namespace muxivo::capture::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;
using muxivo::d3d12::PresentCapture;

constexpr UINT maxSwapBuffers = 16;
constexpr std::size_t maxTrackedCmdLists = 4096;

// ID3D12CommandQueue COM slots (Windows SDK 10.0.26100.0 d3d12.h)
constexpr std::size_t slotQueueExecuteCommandLists = 10;
constexpr std::size_t slotQueueSignal = 14;

// ID3D12GraphicsCommandList COM slots
constexpr std::size_t slotCmdListReset = 10;
constexpr std::size_t slotCmdListCopyTextureRegion = 16;
constexpr std::size_t slotCmdListCopyResource = 17;
constexpr std::size_t slotCmdListResourceBarrier = 26;
constexpr std::size_t slotCmdList7Barrier = 80;

using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PfnResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using PfnSetColorSpace1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

using PfnExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PfnQueueSignal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using PfnCmdListReset = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PfnCmdListCopyTextureRegion = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using PfnCmdListCopyResource = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PfnCmdListResourceBarrier = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PfnCmdList7Barrier = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);

PfnPresent realPresent = nullptr;
PfnPresent realSecondaryPresent = nullptr;
PfnPresent1 realPresent1 = nullptr;
PfnResizeBuffers realResizeBuffers = nullptr;
PfnResizeBuffers1 realResizeBuffers1 = nullptr;
PfnSetColorSpace1 realSetColorSpace1 = nullptr;

PfnExecuteCommandLists realExecute = nullptr;
PfnQueueSignal realSignal = nullptr;
PfnCmdListReset realReset = nullptr;
PfnCmdListCopyTextureRegion realCopyRegion = nullptr;
PfnCmdListCopyResource realCopyResource = nullptr;
PfnCmdListResourceBarrier realResourceBarrier = nullptr;
PfnCmdList7Barrier realBarrier7 = nullptr;

struct HookState {
    std::mutex mutex;
    SessionConfigClient config{ CaptureApi::D3D12 };
    HANDLE loadedEvent = nullptr;

    std::uint32_t lastGeneration = 0;

    HWND targetWindow = nullptr;
    IDXGISwapChain3* activeSwap = nullptr;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::unordered_set<void*> destructionNotified;

    ComPtr<IUnknown> deviceIdentity;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyCommands;
    ComPtr<ID3D12Fence> copyFence;
    UINT64 nextFenceValue = 1;
    UINT64 submittedFenceValue = 0;

    std::uint32_t bufferCount = 0;
    std::array<void*, maxSwapBuffers> backBufferPtrs{};
    std::array<ComPtr<ID3D12CommandQueue>, maxSwapBuffers> verifiedQueues{};
    ComPtr<ID3D12CommandQueue> lastVerifiedQueue;

    std::unordered_map<void*, std::uint32_t> cmdListTouchedBuffer;
    std::unique_ptr<PresentCapture> capture;

    void waitForFenceLocked(std::unique_lock<std::mutex>& lock) noexcept;
    void waitAndReleaseGpuCopyLocked(std::unique_lock<std::mutex>& lock) noexcept;

    void clearBackBufferTrackingLocked() noexcept {
        bufferCount = 0;
        backBufferPtrs.fill(nullptr);
        cmdListTouchedBuffer.clear();
    }

    void resetQueuesLocked() noexcept {
        for (auto& q : verifiedQueues) q.Reset();
        lastVerifiedQueue.Reset();
    }

    void releaseCopyInfraLocked() noexcept {
        copyCommands.Reset();
        copyAllocator.Reset();
        copyFence.Reset();
        nextFenceValue = 1;
        submittedFenceValue = 0;
        device.Reset();
        deviceIdentity.Reset();
    }
};

void clearD3D12SwapRealPointers() noexcept {
    realPresent = nullptr;
    realSecondaryPresent = nullptr;
    realPresent1 = nullptr;
    realResizeBuffers = nullptr;
    realResizeBuffers1 = nullptr;
    realSetColorSpace1 = nullptr;
}

HookState& state() noexcept {
    // Intentionally immortal: this hook DLL stays pinned until process exit, and releasing
    // D3D COM objects or waiting on overlapped I/O inside DLL_PROCESS_DETACH risks loader-lock deadlock/crash.
    static HookState* instance = new HookState();
    return *instance;
}

thread_local int g_presentDepth = 0;
thread_local bool g_inInternalSubmit = false;
thread_local IDXGISwapChain* g_presentingSwap = nullptr;
std::atomic<bool> g_hooksReady{ false };
std::atomic<bool> g_trackingActive{ false };
std::mutex g_d3d12InstallMutex;
std::atomic<bool> g_d3d12CmdHooksInstalled{ false };

struct InternalSubmitGuard {
    bool prev;
    InternalSubmitGuard() noexcept : prev(g_inInternalSubmit) { g_inInternalSubmit = true; }
    ~InternalSubmitGuard() { g_inInternalSubmit = prev; }

    InternalSubmitGuard(const InternalSubmitGuard&) = delete;
    InternalSubmitGuard& operator=(const InternalSubmitGuard&) = delete;
};

void HookState::waitForFenceLocked(std::unique_lock<std::mutex>& lock) noexcept {
    if (!copyFence || submittedFenceValue == 0) return;
    if (copyFence->GetCompletedValue() >= submittedFenceValue) return;
    const ComPtr<ID3D12Fence> fence = copyFence;
    const UINT64 target = submittedFenceValue;
    const muxivo::UniqueHandle waitEv(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!waitEv) return;
    lock.unlock();
    if (SUCCEEDED(fence->SetEventOnCompletion(target, waitEv.get()))) {
        WaitForSingleObject(waitEv.get(), 2000);
    }
    lock.lock();
}

void HookState::waitAndReleaseGpuCopyLocked(std::unique_lock<std::mutex>& lock) noexcept {
    if (!capture) return;
    capture->publishCompleted();
    if (capture->hasPendingCopy()) {
        waitForFenceLocked(lock);
    }
    if (capture) {
        capture->publishCompleted();
        capture->retire();
    }
}

void suspendCaptureLocked(std::unique_lock<std::mutex>& lock) noexcept {
    auto& s = state();
    g_trackingActive.store(false, std::memory_order_release);
    s.waitAndReleaseGpuCopyLocked(lock);
    s.capture.reset();
    s.activeSwap = nullptr;
    s.targetWindow = nullptr;
    s.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    s.clearBackBufferTrackingLocked();
    s.resetQueuesLocked();
    s.releaseCopyInfraLocked();
}

bool fastPathIdle() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return !s.activeSwap && !s.capture && s.config.isIdleCached(std::chrono::steady_clock::now());
}

void STDMETHODCALLTYPE onSwapChainDestroyed(void* data) {
    auto& s = state();
    std::unique_lock lock(s.mutex);
    s.destructionNotified.erase(data);
    if (static_cast<void*>(s.activeSwap) == data) {
        suspendCaptureLocked(lock);
    }
}

void ensureDestructionCallbackLocked(HookState& s, IDXGISwapChain3* swap) noexcept {
    if (s.destructionNotified.count(swap)) return;
    ComPtr<ID3DDestructionNotifier> notifier;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&notifier))) && notifier) {
        UINT cookie = 0;
        if (SUCCEEDED(notifier->RegisterDestructionCallback(&onSwapChainDestroyed, swap, &cookie))) {
            try { s.destructionNotified.insert(swap); }
            catch (...) {}
        }
    }
}

bool sameDirectQueueDeviceLocked(const HookState& s, ID3D12CommandQueue* queue) noexcept {
    if (!queue || !s.deviceIdentity) return false;
    const auto desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    ComPtr<ID3D12Device> qdev;
    ComPtr<IUnknown> qid;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&qdev))) || FAILED(qdev.As(&qid))) return false;
    return qid.Get() == s.deviceIdentity.Get();
}

void snapshotSwapBackBuffersLocked(HookState& s, IDXGISwapChain3* swap3) noexcept {
    s.clearBackBufferTrackingLocked();
    if (!swap3) return;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(swap3->GetDesc1(&desc))) return;
    const std::uint32_t count = std::min<std::uint32_t>(desc.BufferCount, maxSwapBuffers);
    for (std::uint32_t i = 0; i < count; ++i) {
        ComPtr<ID3D12Resource> res;
        if (SUCCEEDED(swap3->GetBuffer(i, IID_PPV_ARGS(&res))) && res) {
            s.backBufferPtrs[i] = res.Get();
        }
    }
    s.bufferCount = count;
    g_trackingActive.store(s.activeSwap != nullptr && s.bufferCount > 0, std::memory_order_release);
}

void recordResourceTouch(ID3D12GraphicsCommandList* cmdList, const void* resource) noexcept {
    if (!cmdList || !resource || g_inInternalSubmit || !g_trackingActive.load(std::memory_order_acquire)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.activeSwap || s.bufferCount == 0) return;
    for (std::uint32_t i = 0; i < s.bufferCount; ++i) {
        if (s.backBufferPtrs[i] == resource) {
            try {
                if (s.cmdListTouchedBuffer.size() >= maxTrackedCmdLists) {
                    s.cmdListTouchedBuffer.clear();
                }
                s.cmdListTouchedBuffer[cmdList] = i;
            } catch (...) {}
            return;
        }
    }
}

HRESULT STDMETHODCALLTYPE hookCmdListReset(
    ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    if (!g_inInternalSubmit && list && g_trackingActive.load(std::memory_order_acquire)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.cmdListTouchedBuffer.erase(list);
    }
    return realReset(list, allocator, pso);
}

void STDMETHODCALLTYPE hookCmdListResourceBarrier(
    ID3D12GraphicsCommandList* list, UINT numBarriers, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!g_inInternalSubmit && list && barriers) {
        for (UINT i = 0; i < numBarriers; ++i) {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                recordResourceTouch(list, barriers[i].Transition.pResource);
            } else if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                recordResourceTouch(list, barriers[i].Aliasing.pResourceBefore);
                recordResourceTouch(list, barriers[i].Aliasing.pResourceAfter);
            }
        }
    }
    realResourceBarrier(list, numBarriers, barriers);
}

void STDMETHODCALLTYPE hookCmdList7Barrier(
    ID3D12GraphicsCommandList7* list, UINT32 numGroups, const D3D12_BARRIER_GROUP* groups) {
    if (!g_inInternalSubmit && list && groups) {
        for (UINT32 g = 0; g < numGroups; ++g) {
            if (groups[g].Type == D3D12_BARRIER_TYPE_TEXTURE && groups[g].pTextureBarriers) {
                for (UINT32 t = 0; t < groups[g].NumBarriers; ++t) {
                    recordResourceTouch(list, groups[g].pTextureBarriers[t].pResource);
                }
            }
        }
    }
    realBarrier7(list, numGroups, groups);
}

void STDMETHODCALLTYPE hookCmdListCopyResource(
    ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!g_inInternalSubmit && list) {
        recordResourceTouch(list, dst);
        recordResourceTouch(list, src);
    }
    realCopyResource(list, dst, src);
}

void STDMETHODCALLTYPE hookCmdListCopyTextureRegion(
    ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY, UINT dstZ,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* srcBox) {
    if (!g_inInternalSubmit && list) {
        if (dst) recordResourceTouch(list, dst->pResource);
        if (src) recordResourceTouch(list, src->pResource);
    }
    realCopyRegion(list, dst, dstX, dstY, dstZ, src, srcBox);
}

void STDMETHODCALLTYPE hookExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* ppLists) {
    if (!g_inInternalSubmit && queue && g_trackingActive.load(std::memory_order_acquire)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.activeSwap && sameDirectQueueDeviceLocked(s, queue)) {
            if (g_presentingSwap && static_cast<IDXGISwapChain*>(s.activeSwap) == g_presentingSwap) {
                s.lastVerifiedQueue = queue;
            }
            if (ppLists) {
                for (UINT i = 0; i < numLists; ++i) {
                    const auto it = s.cmdListTouchedBuffer.find(ppLists[i]);
                    if (it != s.cmdListTouchedBuffer.end()) {
                        const std::uint32_t bufIdx = it->second;
                        s.cmdListTouchedBuffer.erase(it);
                        if (bufIdx < maxSwapBuffers) {
                            s.verifiedQueues[bufIdx] = queue;
                            s.lastVerifiedQueue = queue;
                        }
                    }
                }
            }
        }
    }
    realExecute(queue, numLists, ppLists);
}

HRESULT STDMETHODCALLTYPE hookQueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    if (!g_inInternalSubmit && queue && g_presentingSwap && g_trackingActive.load(std::memory_order_acquire)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.activeSwap && static_cast<IDXGISwapChain*>(s.activeSwap) == g_presentingSwap
            && sameDirectQueueDeviceLocked(s, queue)) {
            s.lastVerifiedQueue = queue;
        }
    }
    return realSignal(queue, fence, value);
}

template <typename Ptr>
Ptr vtableSlot(const void* obj, std::size_t slot) noexcept {
    const auto* vt = *reinterpret_cast<const std::uintptr_t* const*>(obj);
    return reinterpret_cast<Ptr>(vt[slot]);
}

void clearD3D12RealPointers() noexcept {
    realExecute = nullptr;
    realSignal = nullptr;
    realReset = nullptr;
    realCopyRegion = nullptr;
    realCopyResource = nullptr;
    realResourceBarrier = nullptr;
    realBarrier7 = nullptr;
}

bool ensureD3D12CommandHooks(ID3D12Device* device) noexcept {
    if (!device) return false;
    if (g_d3d12CmdHooksInstalled.load(std::memory_order_acquire)) return true;

    std::lock_guard installLock(g_d3d12InstallMutex);
    if (g_d3d12CmdHooksInstalled.load(std::memory_order_relaxed)) return true;

    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> probeQueue;
    ComPtr<ID3D12CommandAllocator> probeAlloc;
    ComPtr<ID3D12GraphicsCommandList> probeList;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&probeQueue))) || !probeQueue) return false;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probeAlloc))) || !probeAlloc) {
        return false;
    }
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, probeAlloc.Get(), nullptr, IID_PPV_ARGS(&probeList))) || !probeList) {
        return false;
    }
    probeList->Close();

    realExecute = vtableSlot<PfnExecuteCommandLists>(probeQueue.Get(), slotQueueExecuteCommandLists);
    realSignal = vtableSlot<PfnQueueSignal>(probeQueue.Get(), slotQueueSignal);
    realReset = vtableSlot<PfnCmdListReset>(probeList.Get(), slotCmdListReset);
    realCopyRegion = vtableSlot<PfnCmdListCopyTextureRegion>(probeList.Get(), slotCmdListCopyTextureRegion);
    realCopyResource = vtableSlot<PfnCmdListCopyResource>(probeList.Get(), slotCmdListCopyResource);
    realResourceBarrier = vtableSlot<PfnCmdListResourceBarrier>(probeList.Get(), slotCmdListResourceBarrier);

    ComPtr<ID3D12GraphicsCommandList7> probeList7;
    if (SUCCEEDED(probeList.As(&probeList7)) && probeList7) {
        realBarrier7 = vtableSlot<PfnCmdList7Barrier>(probeList7.Get(), slotCmdList7Barrier);
    }

    probeList7.Reset();
    probeList.Reset();
    probeAlloc.Reset();
    probeQueue.Reset();

    if (DetourTransactionBegin() != NO_ERROR) {
        clearD3D12RealPointers();
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    const auto threads = updateOtherProcessThreads();

    bool ok =
        DetourAttach(&reinterpret_cast<PVOID&>(realExecute), reinterpret_cast<PVOID>(hookExecuteCommandLists)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realSignal), reinterpret_cast<PVOID>(hookQueueSignal)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realReset), reinterpret_cast<PVOID>(hookCmdListReset)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realCopyRegion), reinterpret_cast<PVOID>(hookCmdListCopyTextureRegion)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realCopyResource), reinterpret_cast<PVOID>(hookCmdListCopyResource)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realResourceBarrier), reinterpret_cast<PVOID>(hookCmdListResourceBarrier)) == NO_ERROR;

    if (ok && realBarrier7) {
        ok = DetourAttach(&reinterpret_cast<PVOID&>(realBarrier7), reinterpret_cast<PVOID>(hookCmdList7Barrier)) == NO_ERROR;
    }

    const LONG commit = ok ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!ok) DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (commit != NO_ERROR) {
        clearD3D12RealPointers();
        return false;
    }

    g_d3d12CmdHooksInstalled.store(true, std::memory_order_release);
    return true;
}

bool ensureCopyInfraLocked(HookState& s, ID3D12Device* dev) noexcept {
    ComPtr<IUnknown> devId;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&devId))) || !devId) return false;
    if (s.deviceIdentity && s.deviceIdentity.Get() == devId.Get()
        && s.copyAllocator && s.copyCommands && s.copyFence) {
        return true;
    }
    s.releaseCopyInfraLocked();
    s.device = dev;
    s.deviceIdentity = devId;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.copyAllocator)))) {
        s.releaseCopyInfraLocked();
        return false;
    }
    if (FAILED(dev->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.copyAllocator.Get(), nullptr, IID_PPV_ARGS(&s.copyCommands)))) {
        s.releaseCopyInfraLocked();
        return false;
    }
    if (FAILED(s.copyCommands->Close())) {
        s.releaseCopyInfraLocked();
        return false;
    }
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.copyFence)))) {
        s.releaseCopyInfraLocked();
        return false;
    }
    s.nextFenceValue = 1;
    s.submittedFenceValue = 0;
    return true;
}

struct PreparedContext {
    bool active = false;
    std::uint32_t generation = 0;
    IDXGISwapChain* rawSwap = nullptr;
};

PreparedContext prepareAndRecord(IDXGISwapChain* rawSwap, UINT flags) noexcept {
    PreparedContext ctx;
    if (!rawSwap || fastPathIdle()) return ctx;

    ComPtr<IDXGISwapChain3> swap3;
    if (FAILED(rawSwap->QueryInterface(IID_PPV_ARGS(&swap3))) || !swap3) return ctx;
    HWND hwnd = nullptr;
    if (FAILED(swap3->GetHwnd(&hwnd)) || !hwnd) return ctx;

    ComPtr<ID3D12Device> d3d12Device;
    if (FAILED(rawSwap->GetDevice(IID_PPV_ARGS(&d3d12Device))) || !d3d12Device) {
        return ctx;
    }

    if (!ensureD3D12CommandHooks(d3d12Device.Get())) {
        return ctx;
    }

    auto& s = state();
    std::unique_lock lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    bool justStopped = false;
    if (!s.config.checkHostRunning(now, justStopped)) {
        if (justStopped || s.activeSwap || s.capture) suspendCaptureLocked(lock);
        if (justStopped) s.lastGeneration = 0;
        return ctx;
    }

    ConfigSnapshot cfg = {};
    if (!s.config.readSnapshot(cfg) || !cfg.active || !cfg.targetHwnd) {
        suspendCaptureLocked(lock);
        return ctx;
    }

    if (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)) != cfg.targetHwnd) {
        if (cfg.generation != s.lastGeneration) {
            suspendCaptureLocked(lock);
            s.lastGeneration = cfg.generation;
        }
        return ctx;
    }

    if (cfg.generation != s.lastGeneration) {
        s.lastGeneration = cfg.generation;
        if (s.targetWindow != hwnd) {
            suspendCaptureLocked(lock);
        }
    }

    try {
        if (s.activeSwap != swap3.Get() || s.targetWindow != hwnd) {
            suspendCaptureLocked(lock);
            if (!s.config.readSnapshot(cfg)
                || !cfg.active
                || static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)) != cfg.targetHwnd) {
                return ctx;
            }
            s.lastGeneration = cfg.generation;
            s.activeSwap = swap3.Get();
            s.targetWindow = hwnd;
            s.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            ensureDestructionCallbackLocked(s, swap3.Get());
            s.capture = std::make_unique<PresentCapture>(hwnd, *swap3.Get());
        }
    } catch (...) {
        suspendCaptureLocked(lock);
        return ctx;
    }

    if (!ensureCopyInfraLocked(s, d3d12Device.Get())) {
        return ctx;
    }
    if (s.bufferCount == 0) {
        snapshotSwapBackBuffersLocked(s, swap3.Get());
    }

    ctx.active = true;
    ctx.generation = cfg.generation;
    ctx.rawSwap = rawSwap;

    if (!s.capture || (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE))) {
        return ctx;
    }

    s.capture->publishCompleted();
    if (s.capture->hasPendingCopy()) {
        return ctx;
    }
    if (s.submittedFenceValue > 0 && s.copyFence->GetCompletedValue() < s.submittedFenceValue) {
        return ctx;
    }

    const UINT backIdx = swap3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12CommandQueue> presentQueue;
    if (backIdx < maxSwapBuffers && s.verifiedQueues[backIdx]) {
        presentQueue = s.verifiedQueues[backIdx];
    } else if (s.lastVerifiedQueue) {
        presentQueue = s.lastVerifiedQueue;
    }
    if (!presentQueue) {
        return ctx;
    }

    if (FAILED(s.copyAllocator->Reset())) {
        return ctx;
    }
    const UINT64 fenceVal = s.nextFenceValue++;
    {
        InternalSubmitGuard submitGuard;
        if (FAILED(s.copyCommands->Reset(s.copyAllocator.Get(), nullptr))) {
            return ctx;
        }
        const HRESULT prepHr = s.capture->prepare(
            s.colorSpace, flags, *s.copyCommands.Get(), *s.copyFence.Get(), fenceVal);
        if (prepHr != S_OK) {
            s.copyCommands->Close();
            return ctx;
        }
        if (FAILED(s.copyCommands->Close())) {
            s.capture->cancelUnqueuedCopy();
            s.capture->retire();
            return ctx;
        }
        ID3D12CommandList* lists[] = { s.copyCommands.Get() };
        presentQueue->ExecuteCommandLists(1, lists);
        const HRESULT sigHr = presentQueue->Signal(s.copyFence.Get(), fenceVal);
        if (SUCCEEDED(sigHr)) {
            s.submittedFenceValue = fenceVal;
        } else {
            s.capture->cancelUnqueuedCopy();
            s.capture->retire();
            s.releaseCopyInfraLocked();
        }
    }
    return ctx;
}

void finishCapture(const PreparedContext& ctx, HRESULT presentResult) noexcept {
    if (!ctx.active) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.capture || s.lastGeneration != ctx.generation) return;
    s.capture->complete(presentResult);
    s.capture->publishCompleted();
}

void prepareResize(IDXGISwapChain* swap) noexcept {
    if (!swap) return;
    auto& s = state();
    std::unique_lock lock(s.mutex);
    if (static_cast<IDXGISwapChain*>(s.activeSwap) == swap) {
        g_trackingActive.store(false, std::memory_order_release);
        if (s.capture) {
            s.capture->publishCompleted();
            if (s.capture->hasPendingCopy()) {
                s.waitForFenceLocked(lock);
            }
            if (s.capture) {
                s.capture->publishCompleted();
                s.capture->resetReadbackForResize();
            }
        }
        s.clearBackBufferTrackingLocked();
    }
}

void finishResize(IDXGISwapChain* swap, HRESULT hr, IUnknown* const* presentQueues, UINT count) noexcept {
    if (!swap || FAILED(hr)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (static_cast<IDXGISwapChain*>(s.activeSwap) != swap) return;
    snapshotSwapBackBuffersLocked(s, s.activeSwap);
    if (presentQueues != nullptr) {
        const UINT effectiveCount = count > 0 ? count : s.bufferCount;
        if (effectiveCount > 0) {
            s.resetQueuesLocked();
            const UINT capped = std::min<UINT>(effectiveCount, maxSwapBuffers);
            for (UINT i = 0; i < capped; ++i) {
                if (!presentQueues[i]) continue;
                ComPtr<ID3D12CommandQueue> queue;
                if (SUCCEEDED(presentQueues[i]->QueryInterface(IID_PPV_ARGS(&queue)))
                    && sameDirectQueueDeviceLocked(s, queue.Get())) {
                    s.verifiedQueues[i] = queue;
                    s.lastVerifiedQueue = queue;
                }
            }
        }
    }
}

HRESULT STDMETHODCALLTYPE hookPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        ctx = prepareAndRecord(swap, flags);
    }
    IDXGISwapChain* const prevSwap = g_presentingSwap;
    if (ctx.active) g_presentingSwap = swap;
    const HRESULT hr = realPresent(swap, syncInterval, flags);
    g_presentingSwap = prevSwap;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(ctx, hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookSecondaryPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        ctx = prepareAndRecord(swap, flags);
    }
    IDXGISwapChain* const prevSwap = g_presentingSwap;
    if (ctx.active) g_presentingSwap = swap;
    const HRESULT hr = realSecondaryPresent(swap, syncInterval, flags);
    g_presentingSwap = prevSwap;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(ctx, hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookPresent1(
    IDXGISwapChain1* swap, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    const bool topLevel = (g_presentDepth == 0);
    ReentrancyGuard guard(g_presentDepth);
    PreparedContext ctx;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        ctx = prepareAndRecord(swap, flags);
    }
    IDXGISwapChain* const prevSwap = g_presentingSwap;
    if (ctx.active) g_presentingSwap = swap;
    const HRESULT hr = realPresent1(swap, syncInterval, flags, params);
    g_presentingSwap = prevSwap;
    if (topLevel && !(flags & DXGI_PRESENT_TEST)) {
        finishCapture(ctx, hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookResizeBuffers(
    IDXGISwapChain* swap, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    prepareResize(swap);
    const HRESULT hr = realResizeBuffers(swap, bufferCount, width, height, format, flags);
    finishResize(swap, hr, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookResizeBuffers1(
    IDXGISwapChain3* swap, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
    const UINT* creationNodeMask, IUnknown* const* presentQueue) {
    prepareResize(swap);
    const HRESULT hr = realResizeBuffers1(swap, bufferCount, width, height, format, flags, creationNodeMask, presentQueue);
    finishResize(swap, hr, presentQueue, bufferCount);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookSetColorSpace1(IDXGISwapChain3* swap, DXGI_COLOR_SPACE_TYPE colorSpace) {
    const HRESULT hr = realSetColorSpace1(swap, colorSpace);
    if (SUCCEEDED(hr) && swap) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.activeSwap == swap) {
            s.colorSpace = colorSpace;
        }
    }
    return hr;
}

} // namespace

bool hooksReady() noexcept {
    return g_hooksReady.load(std::memory_order_acquire);
}

bool bootstrapHooks() noexcept {
    if (hooksReady()) return true;

    std::uint32_t presentRva = 0;
    std::uint32_t resizeRva = 0;
    std::uint32_t present1Rva = 0;
    std::uint32_t resize1Rva = 0;
    std::uint32_t colorSpace1Rva = 0;
    std::uint32_t secondaryPresentRva = 0;
    HANDLE loaded = nullptr;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.loadedEvent) {
            g_hooksReady.store(true, std::memory_order_release);
            return true;
        }
        if (!s.config.openConfigMapping()) return false;
        presentRva = s.config.readRva(0);
        resizeRva = s.config.readRva(1);
        present1Rva = s.config.readRva(2);
        resize1Rva = s.config.readRva(3);
        colorSpace1Rva = s.config.readRva(4);
        secondaryPresentRva = s.config.readRva(5);
        loaded = s.config.createLoadedEvent();
        if (!loaded) return false;
    }

    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    ModuleImageRange dxgiRange = {};
    if (!readModuleImageRange(dxgi, dxgiRange)) {
        CloseHandle(loaded);
        return false;
    }

    auto validateRva = [&](std::uint32_t rva) noexcept -> void* {
        if (rva == 0 || rva >= dxgiRange.size) return nullptr;
        return reinterpret_cast<void*>(dxgiRange.base + rva);
    };

    void* presentAddr = validateRva(presentRva);
    void* resizeAddr = validateRva(resizeRva);
    void* present1Addr = validateRva(present1Rva);
    void* resize1Addr = validateRva(resize1Rva);
    void* colorSpace1Addr = colorSpace1Rva ? validateRva(colorSpace1Rva) : nullptr;
    void* secondaryAddr = secondaryPresentRva ? validateRva(secondaryPresentRva) : nullptr;
    if (!presentAddr || !resizeAddr || !present1Addr || !resize1Addr) {
        CloseHandle(loaded);
        return false;
    }

    realPresent = reinterpret_cast<PfnPresent>(presentAddr);
    realResizeBuffers = reinterpret_cast<PfnResizeBuffers>(resizeAddr);
    realPresent1 = reinterpret_cast<PfnPresent1>(present1Addr);
    realResizeBuffers1 = reinterpret_cast<PfnResizeBuffers1>(resize1Addr);
    realSetColorSpace1 = reinterpret_cast<PfnSetColorSpace1>(colorSpace1Addr);
    realSecondaryPresent = reinterpret_cast<PfnPresent>(secondaryAddr);

    if (DetourTransactionBegin() != NO_ERROR) {
        clearD3D12SwapRealPointers();
        CloseHandle(loaded);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    const auto threads = updateOtherProcessThreads();

    bool attached =
        DetourAttach(&reinterpret_cast<PVOID&>(realPresent), reinterpret_cast<PVOID>(hookPresent)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realResizeBuffers), reinterpret_cast<PVOID>(hookResizeBuffers)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realPresent1), reinterpret_cast<PVOID>(hookPresent1)) == NO_ERROR
        && DetourAttach(&reinterpret_cast<PVOID&>(realResizeBuffers1), reinterpret_cast<PVOID>(hookResizeBuffers1)) == NO_ERROR;

    if (attached && realSetColorSpace1) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realSetColorSpace1), reinterpret_cast<PVOID>(hookSetColorSpace1)) == NO_ERROR;
    } else {
        realSetColorSpace1 = nullptr;
    }

    if (attached && realSecondaryPresent && secondaryAddr != presentAddr && secondaryAddr != present1Addr) {
        attached = DetourAttach(
            &reinterpret_cast<PVOID&>(realSecondaryPresent), reinterpret_cast<PVOID>(hookSecondaryPresent)) == NO_ERROR;
    } else {
        realSecondaryPresent = nullptr;
    }

    const LONG commit = attached ? DetourTransactionCommit() : ERROR_INVALID_PARAMETER;
    if (!attached) DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (commit != NO_ERROR) {
        clearD3D12SwapRealPointers();
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

} // namespace muxivo::capture::d3d12

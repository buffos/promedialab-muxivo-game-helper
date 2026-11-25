#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {

using Microsoft::WRL::ComPtr;
using Color = std::array<std::uint8_t, 4>;

void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

void succeeded(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed 0x%08lx\n", msg, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

void waitQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE ev, UINT64& fenceVal) {
    const UINT64 target = ++fenceVal;
    succeeded(queue->Signal(fence, target), "Signal fence");
    if (fence->GetCompletedValue() < target) {
        succeeded(fence->SetEventOnCompletion(target, ev), "SetEventOnCompletion");
        check(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "Fence wait timeout");
    }
}

} // namespace

int main() {
    try {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        const WNDCLASSW wc{ CS_OWNDC, DefWindowProcW, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"MuxivoD3D12Sim" };
        RegisterClassW(&wc);
        const HWND hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE, L"MuxivoD3D12Sim", L"Muxivo D3D12 MultiQueue Sim",
            WS_POPUP | WS_VISIBLE, 16, 16, 192, 128, nullptr, nullptr, inst, nullptr);
        check(hwnd != nullptr, "Create D3D12 sim window");

        ComPtr<IDXGIFactory6> factory;
        succeeded(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            succeeded(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        }
        ComPtr<ID3D12Device> device;
        succeeded(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC qdesc = {};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        // Queue A: Decoy direct queue executing non-swapchain work every frame before Present.
        // Queue B: Actual swapchain creation queue.
        // Queue C: Replacement queue activated mid-run via ResizeBuffers1.
        ComPtr<ID3D12CommandQueue> decoyQueue;
        ComPtr<ID3D12CommandQueue> queueB;
        ComPtr<ID3D12CommandQueue> queueC;
        succeeded(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&decoyQueue)), "Create decoy queue");
        succeeded(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queueB)), "Create primary present queue B");
        succeeded(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queueC)), "Create replacement present queue C");

        ComPtr<ID3D12CommandAllocator> decoyAlloc;
        ComPtr<ID3D12GraphicsCommandList> decoyList;
        succeeded(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&decoyAlloc)), "Decoy alloc");
        succeeded(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, decoyAlloc.Get(), nullptr, IID_PPV_ARGS(&decoyList)), "Decoy list");
        succeeded(decoyList->Close(), "Close initial decoy list");

        ComPtr<ID3D12Resource> decoyTex;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rdesc = {};
        rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rdesc.Width = 64;
        rdesc.Height = 64;
        rdesc.DepthOrArraySize = 1;
        rdesc.MipLevels = 1;
        rdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rdesc.SampleDesc.Count = 1;
        rdesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        succeeded(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&decoyTex)), "Decoy RT");

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        succeeded(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
        const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE decoyRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE swapRtv = { decoyRtv.ptr + rtvStep };
        device->CreateRenderTargetView(decoyTex.Get(), nullptr, decoyRtv);

        ComPtr<ID3D12CommandAllocator> drawAlloc;
        ComPtr<ID3D12GraphicsCommandList> drawList;
        succeeded(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&drawAlloc)), "Draw alloc");
        succeeded(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, drawAlloc.Get(), nullptr, IID_PPV_ARGS(&drawList)), "Draw list");
        succeeded(drawList->Close(), "Close initial draw list");

        ComPtr<ID3D12Fence> fence;
        succeeded(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(ev != nullptr, "Fence event");

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = 192;
        scDesc.Height = 128;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> swap1;
        succeeded(factory->CreateSwapChainForHwnd(queueB.Get(), hwnd, &scDesc, nullptr, nullptr, &swap1), "CreateSwapChainForHwnd");
        ComPtr<IDXGISwapChain3> swap3;
        succeeded(swap1.As(&swap3), "IDXGISwapChain3");

        ID3D12CommandQueue* activeQueue = queueB.Get();
        UINT64 fenceVal = 0;
        Color color{ 17, 73, 191, 255 };
        bool readySent = false;
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

        for (int frame = 0;; ++frame) {
            MSG msg = {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return 0;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetFileType(input) == FILE_TYPE_PIPE) {
                DWORD queued = 0;
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, &queued, nullptr)) break;
                if (queued > 0) {
                    char cmd = 0;
                    DWORD read = 0;
                    if (!ReadFile(input, &cmd, 1, &read, nullptr) || read != 1 || cmd == 'q') break;
                    if (cmd == 'n') {
                        color = { 211, 47, 89, 255 };
                    } else if (cmd == 'r') {
                        waitQueueIdle(activeQueue, fence.Get(), ev, fenceVal);
                        const UINT nodes[2] = { 1, 1 };
                        IUnknown* queues[2] = { queueC.Get(), queueC.Get() };
                        succeeded(swap3->ResizeBuffers1(
                            2, 256, 144, DXGI_FORMAT_B8G8R8A8_UNORM, 0, nodes, queues), "ResizeBuffers1 -> Queue C");
                        activeQueue = queueC.Get();
                        color = { 41, 173, 99, 255 };
                    } else if (cmd == '0') {
                        waitQueueIdle(activeQueue, fence.Get(), ev, fenceVal);
                        const UINT nodes[2] = { 1, 1 };
                        IUnknown* queues[2] = { queueB.Get(), queueB.Get() };
                        succeeded(swap3->ResizeBuffers1(
                            0, 224, 160, DXGI_FORMAT_R8G8B8A8_UNORM, 0, nodes, queues), "ResizeBuffers1(0) -> Queue B");
                        activeQueue = queueB.Get();
                        color = { 231, 195, 37, 255 };
                    }
                }
            }

            // Always submit decoy work on Decoy Queue A first!
            succeeded(decoyAlloc->Reset(), "Decoy alloc reset");
            succeeded(decoyList->Reset(decoyAlloc.Get(), nullptr), "Decoy list reset");
            const float decoyClear[4] = { 0.9f, 0.1f, 0.9f, 1.0f };
            decoyList->ClearRenderTargetView(decoyRtv, decoyClear, 0, nullptr);
            succeeded(decoyList->Close(), "Decoy list close");
            ID3D12CommandList* decoyLists[] = { decoyList.Get() };
            decoyQueue->ExecuteCommandLists(1, decoyLists);
            waitQueueIdle(decoyQueue.Get(), fence.Get(), ev, fenceVal);

            // Render to actual swapchain back buffer and submit on activeQueue (Queue B or Queue C).
            succeeded(drawAlloc->Reset(), "Draw alloc reset");
            succeeded(drawList->Reset(drawAlloc.Get(), nullptr), "Draw list reset");
            ComPtr<ID3D12Resource> backBuf;
            succeeded(swap3->GetBuffer(swap3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuf)), "GetBuffer");
            device->CreateRenderTargetView(backBuf.Get(), nullptr, swapRtv);
            D3D12_RESOURCE_BARRIER toRt = {};
            toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRt.Transition = { backBuf.Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
            drawList->ResourceBarrier(1, &toRt);
            const float clearRgba[4] = { color[0] / 255.f, color[1] / 255.f, color[2] / 255.f, 1.0f };
            drawList->ClearRenderTargetView(swapRtv, clearRgba, 0, nullptr);
            D3D12_RESOURCE_BARRIER toPresent = {};
            toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toPresent.Transition = { backBuf.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT };
            drawList->ResourceBarrier(1, &toPresent);
            backBuf.Reset();
            succeeded(drawList->Close(), "Draw list close");

            ID3D12CommandList* drawLists[] = { drawList.Get() };
            activeQueue->ExecuteCommandLists(1, drawLists);
            swap3->Present(0, 0);
            waitQueueIdle(activeQueue, fence.Get(), ev, fenceVal);

            if (!readySent && frame >= 3) {
                readySent = true;
                std::printf("READY %lu %lu %llx\n",
                    GetCurrentProcessId(), GetCurrentThreadId(),
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
                std::fflush(stdout);
            }
            Sleep(15);
        }
        CloseHandle(ev);
        DestroyWindow(hwnd);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

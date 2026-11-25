#include "d3d12_offsets.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "module_image.h"

namespace muxivo::capture::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PfnD3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// COM vtable slots verified against Windows SDK 10.0.26100.0 shared/dxgi1_4.h:
// IDXGISwapChain:  8=Present, 13=ResizeBuffers
// IDXGISwapChain1: 22=Present1
// IDXGISwapChain3: 38=SetColorSpace1, 39=ResizeBuffers1
constexpr std::size_t slotPresent = 8;
constexpr std::size_t slotResizeBuffers = 13;
constexpr std::size_t slotPresent1 = 22;
constexpr std::size_t slotSetColorSpace1 = 38;
constexpr std::size_t slotResizeBuffers1 = 39;

bool probeD3D12FlipModel(
    HMODULE d3d12Mod,
    HMODULE dxgiMod,
    HWND hwnd,
    const ModuleImageRange& dxgi,
    DxgiSwapOffsets& out) noexcept {
    const auto createDevice = reinterpret_cast<PfnD3D12CreateDevice>(
        GetProcAddress(d3d12Mod, "D3D12CreateDevice"));
    const auto createFactory = reinterpret_cast<PfnCreateDXGIFactory2>(
        GetProcAddress(dxgiMod, "CreateDXGIFactory2"));
    if (!createDevice || !createFactory) return false;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(createFactory(0, IID_PPV_ARGS(&factory))) || !factory) return false;

    ComPtr<ID3D12Device> device;
    if (FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            || FAILED(createDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            return false;
        }
    }

    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) || !queue) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 2;
    desc.Height = 2;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &swap1)) || !swap1) {
        return false;
    }

    if (!readVtableRva(swap1.Get(), slotPresent, dxgi, out.presentRva)
        || !readVtableRva(swap1.Get(), slotResizeBuffers, dxgi, out.resizeBuffersRva)
        || !readVtableRva(swap1.Get(), slotPresent1, dxgi, out.present1Rva)) {
        return false;
    }

    ComPtr<IDXGISwapChain3> swap3;
    if (SUCCEEDED(swap1.As(&swap3)) && swap3) {
        readVtableRva(swap3.Get(), slotSetColorSpace1, dxgi, out.setColorSpace1Rva);
        readVtableRva(swap3.Get(), slotResizeBuffers1, dxgi, out.resizeBuffers1Rva);
    }
    return true;
}

void probeSecondaryLegacyPresent(HMODULE d3d11Mod, HWND hwnd, const ModuleImageRange& dxgi, DxgiSwapOffsets& out) noexcept {
    if (!d3d11Mod) return;
    const auto createAndSwap = reinterpret_cast<PfnD3D11CreateDeviceAndSwapChain>(
        GetProcAddress(d3d11Mod, "D3D11CreateDeviceAndSwapChain"));
    if (!createAndSwap) return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> device;
    HRESULT hr = createAndSwap(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &desc, &swap, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = createAndSwap(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &desc, &swap, &device, nullptr, nullptr);
    }
    if (FAILED(hr) || !swap) return;

    std::uint32_t legacyPresent = 0;
    if (readVtableRva(swap.Get(), slotPresent, dxgi, legacyPresent) && legacyPresent != out.presentRva) {
        out.secondaryPresentRva = legacyPresent;
    }
}

} // namespace

bool probeDxgiSwapOffsets(DxgiSwapOffsets& out) noexcept {
    out = {};
    ModuleGuard dxgiMod(LoadLibraryW(L"dxgi.dll"));
    ModuleGuard d3d12Mod(LoadLibraryW(L"d3d12.dll"));
    ModuleGuard d3d11Mod(LoadLibraryW(L"d3d11.dll"));
    if (!dxgiMod || !d3d12Mod) return false;

    ModuleImageRange dxgi = {};
    if (!readModuleImageRange(dxgiMod.get(), dxgi)) return false;

    DummyWindow window;
    if (!window.create(L"MuxivoD3D12DxgiProbe")) return false;

    if (!probeD3D12FlipModel(d3d12Mod.get(), dxgiMod.get(), window.handle(), dxgi, out)) {
        return false;
    }
    probeSecondaryLegacyPresent(d3d11Mod.get(), window.handle(), dxgi, out);

    return out.presentRva != 0
        && out.resizeBuffersRva != 0
        && out.present1Rva != 0
        && out.resizeBuffers1Rva != 0;
}

} // namespace muxivo::capture::d3d12

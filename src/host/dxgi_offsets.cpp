#include "dxgi_offsets.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "module_image.h"

namespace muxivo {
namespace {

using Microsoft::WRL::ComPtr;
using muxivo::capture::DummyWindow;
using muxivo::capture::ModuleGuard;
using muxivo::capture::ModuleImageRange;
using muxivo::capture::readModuleImageRange;
using muxivo::capture::readVtableRva;

using PfnD3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PfnD3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

HRESULT createD3D11Device(PfnD3D11CreateDevice createFn, ComPtr<ID3D11Device>& device) noexcept {
    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = createFn(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = createFn(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &device, nullptr, nullptr);
    }
    return hr;
}

bool probeFlipModel(
    PfnD3D11CreateDevice createFn,
    HWND hwnd,
    const ModuleImageRange& dxgi,
    DxgiOffsets& out) noexcept {
    ComPtr<ID3D11Device> device;
    if (FAILED(createD3D11Device(createFn, device))) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf())))) {
        return false;
    }
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 2;
    desc.Height = 2;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &swap1))) {
        return false;
    }

    // IDXGISwapChain slots: 8=Present, 13=ResizeBuffers; IDXGISwapChain1 slot: 22=Present1.
    if (!readVtableRva(swap1.Get(), 8, dxgi, out.presentRva)
        || !readVtableRva(swap1.Get(), 13, dxgi, out.resizeBuffersRva)
        || !readVtableRva(swap1.Get(), 22, dxgi, out.present1Rva)) {
        return false;
    }

    ComPtr<IDXGISwapChain3> swap3;
    if (SUCCEEDED(swap1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swap3.GetAddressOf())))) {
        std::uint32_t rb1 = 0;
        if (readVtableRva(swap3.Get(), 39, dxgi, rb1)) {
            out.resizeBuffers1Rva = rb1;
        }
    }
    return true;
}

bool probeLegacySwapChain(
    PfnD3D11CreateDeviceAndSwapChain createAndSwapFn,
    HWND hwnd,
    const ModuleImageRange& dxgi,
    DxgiOffsets& out,
    bool fallbackPrimary) noexcept {
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
    HRESULT hr = createAndSwapFn(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &desc, &swap, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = createAndSwapFn(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &desc, &swap, &device, nullptr, nullptr);
    }
    if (FAILED(hr) || !swap) return false;

    std::uint32_t legacyPresent = 0;
    if (!readVtableRva(swap.Get(), 8, dxgi, legacyPresent)) return false;

    if (fallbackPrimary) {
        out.presentRva = legacyPresent;
        if (!readVtableRva(swap.Get(), 13, dxgi, out.resizeBuffersRva)) return false;
        ComPtr<IDXGISwapChain1> swap1;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(swap1.GetAddressOf())))) {
            readVtableRva(swap1.Get(), 22, dxgi, out.present1Rva);
        }
        ComPtr<IDXGISwapChain3> swap3;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swap3.GetAddressOf())))) {
            readVtableRva(swap3.Get(), 39, dxgi, out.resizeBuffers1Rva);
        }
    } else if (legacyPresent != out.presentRva) {
        out.secondaryPresentRva = legacyPresent;
    }
    return true;
}

} // namespace

bool probeDxgiOffsets(DxgiOffsets& out) noexcept {
    out = {};
    const ModuleGuard dxgiMod(LoadLibraryW(L"dxgi.dll"));
    const ModuleGuard d3d11Mod(LoadLibraryW(L"d3d11.dll"));
    if (!dxgiMod || !d3d11Mod) return false;

    ModuleImageRange dxgi = {};
    if (!readModuleImageRange(dxgiMod.get(), dxgi)) return false;

    const auto createDeviceFn = reinterpret_cast<PfnD3D11CreateDevice>(
        GetProcAddress(d3d11Mod.get(), "D3D11CreateDevice"));
    const auto createAndSwapFn = reinterpret_cast<PfnD3D11CreateDeviceAndSwapChain>(
        GetProcAddress(d3d11Mod.get(), "D3D11CreateDeviceAndSwapChain"));
    if (!createDeviceFn || !createAndSwapFn) return false;

    DummyWindow window;
    if (!window.create(L"MuxivoDxgiProbe")) return false;

    const bool gotFlip = probeFlipModel(createDeviceFn, window.handle(), dxgi, out);
    if (!gotFlip) {
        probeLegacySwapChain(createAndSwapFn, window.handle(), dxgi, out, true);
    } else {
        probeLegacySwapChain(createAndSwapFn, window.handle(), dxgi, out, false);
    }

    return out.presentRva != 0 && out.present1Rva != 0 && out.resizeBuffersRva != 0;
}

} // namespace muxivo

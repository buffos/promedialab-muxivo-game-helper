#include "d3d9_offsets.h"

#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>

#include "module_image.h"

namespace muxivo::capture::d3d9 {
namespace {

using Microsoft::WRL::ComPtr;
using PfnDirect3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
using PfnDirect3DCreate9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

// COM vtable slots verified against Windows SDK 10.0.26100.0 shared/d3d9.h:
// IDirect3DDevice9: 16=Reset, 17=Present
// IDirect3DDevice9Ex: 121=PresentEx, 132=ResetEx
// IDirect3DSwapChain9: 3=Present
constexpr std::size_t slotDeviceReset = 16;
constexpr std::size_t slotDevicePresent = 17;
constexpr std::size_t slotDevicePresentEx = 121;
constexpr std::size_t slotDeviceResetEx = 132;
constexpr std::size_t slotSwapChainPresent = 3;

D3DPRESENT_PARAMETERS makeProbeParams(HWND hwnd) noexcept {
    D3DPRESENT_PARAMETERS params = {};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.BackBufferFormat = D3DFMT_UNKNOWN;
    params.BackBufferWidth = 2;
    params.BackBufferHeight = 2;
    params.BackBufferCount = 1;
    params.hDeviceWindow = hwnd;
    return params;
}

bool probeExOffsets(PfnDirect3DCreate9Ex createExFn, HWND hwnd, const ModuleImageRange& d3d9, D3D9Offsets& out) noexcept {
    if (!createExFn) return false;
    ComPtr<IDirect3D9Ex> d3dEx;
    if (FAILED(createExFn(D3D_SDK_VERSION, &d3dEx)) || !d3dEx) return false;

    const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_REF, D3DDEVTYPE_NULLREF };
    const DWORD behaviors[] = { D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING };
    ComPtr<IDirect3DDevice9Ex> deviceEx;
    for (D3DDEVTYPE devType : types) {
        for (DWORD behavior : behaviors) {
            D3DPRESENT_PARAMETERS params = makeProbeParams(hwnd);
            if (SUCCEEDED(d3dEx->CreateDeviceEx(
                    D3DADAPTER_DEFAULT, devType, hwnd,
                    behavior | D3DCREATE_NOWINDOWCHANGES, &params, nullptr, &deviceEx)) && deviceEx) {
                break;
            }
        }
        if (deviceEx) break;
    }
    if (!deviceEx) return false;

    ComPtr<IDirect3DSwapChain9> swapChain;
    if (FAILED(deviceEx->GetSwapChain(0, &swapChain)) || !swapChain) return false;

    // On x86 SysWOW64 Windows, Reset/ResetEx slots may be shimmed outside d3d9.dll (e.g. apphelp.dll).
    // Require Present + SwapChain::Present inside d3d9.dll; read Reset/ResetEx if inside d3d9.dll
    // and hook shimmed Reset slots lazily from the live IDirect3DDevice9 inside the target process.
    if (!readVtableRva(deviceEx.Get(), slotDevicePresent, d3d9, out.devicePresentRva)
        || !readVtableRva(swapChain.Get(), slotSwapChainPresent, d3d9, out.swapChainPresentRva)) {
        return false;
    }
    readVtableRva(deviceEx.Get(), slotDeviceReset, d3d9, out.deviceResetRva);
    readVtableRva(deviceEx.Get(), slotDevicePresentEx, d3d9, out.devicePresentExRva);
    readVtableRva(deviceEx.Get(), slotDeviceResetEx, d3d9, out.deviceResetExRva);
    return true;
}

bool probeLegacyOffsets(PfnDirect3DCreate9 createFn, HWND hwnd, const ModuleImageRange& d3d9, D3D9Offsets& out) noexcept {
    if (!createFn) return false;
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(createFn(D3D_SDK_VERSION));
    if (!d3d) return false;

    const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_REF, D3DDEVTYPE_NULLREF };
    const DWORD behaviors[] = { D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING };
    ComPtr<IDirect3DDevice9> device;
    for (D3DDEVTYPE devType : types) {
        for (DWORD behavior : behaviors) {
            D3DPRESENT_PARAMETERS params = makeProbeParams(hwnd);
            if (SUCCEEDED(d3d->CreateDevice(
                    D3DADAPTER_DEFAULT, devType, hwnd,
                    behavior | D3DCREATE_NOWINDOWCHANGES, &params, &device)) && device) {
                break;
            }
        }
        if (device) break;
    }
    if (!device) return false;

    ComPtr<IDirect3DSwapChain9> swapChain;
    if (FAILED(device->GetSwapChain(0, &swapChain)) || !swapChain) return false;

    if (!readVtableRva(device.Get(), slotDevicePresent, d3d9, out.devicePresentRva)
        || !readVtableRva(swapChain.Get(), slotSwapChainPresent, d3d9, out.swapChainPresentRva)) {
        return false;
    }
    readVtableRva(device.Get(), slotDeviceReset, d3d9, out.deviceResetRva);
    return true;
}

} // namespace

bool probeD3D9Offsets(D3D9Offsets& out) noexcept {
    out = {};
    ModuleGuard d3d9Mod(LoadLibraryW(L"d3d9.dll"));
    if (!d3d9Mod) return false;

    ModuleImageRange d3d9 = {};
    if (!readModuleImageRange(d3d9Mod.get(), d3d9)) return false;

    DummyWindow window;
    if (!window.create(L"MuxivoD3D9Probe")) return false;

    const auto createExFn = reinterpret_cast<PfnDirect3DCreate9Ex>(
        GetProcAddress(d3d9Mod.get(), "Direct3DCreate9Ex"));
    const auto createFn = reinterpret_cast<PfnDirect3DCreate9>(
        GetProcAddress(d3d9Mod.get(), "Direct3DCreate9"));

    if (probeExOffsets(createExFn, window.handle(), d3d9, out)) {
        return true;
    }
    out = {};
    return probeLegacyOffsets(createFn, window.handle(), d3d9, out);
}

} // namespace muxivo::capture::d3d9

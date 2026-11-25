#pragma once

#include <d3d9.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace muxivo::d3d9 {

struct SdrFrame {
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> rgba;
};

// Owned by a single capture session and called on the renderer's device thread.
// The caller releases these surfaces before resetting the Direct3D device.
class PresentReadback {
public:
    PresentReadback() = default;
    PresentReadback(const PresentReadback&) = delete;
    PresentReadback& operator=(const PresentReadback&) = delete;

    void reset() noexcept;

    HRESULT read(IDirect3DDevice9& device, IDirect3DSwapChain9& swapChain,
        HWND presentWindow, bool hasWindowOverride, SdrFrame& frame);

private:
    HRESULT cooperativeStatus(IDirect3DDevice9& device) noexcept;
    HRESULT ensureSurfaces(IDirect3DDevice9& device, IUnknown& identity, const D3DSURFACE_DESC& desc) noexcept;

    Microsoft::WRL::ComPtr<IUnknown> device_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> staging_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> resolve_;
    UINT width_ = 0;
    UINT height_ = 0;
    D3DFORMAT format_ = D3DFMT_UNKNOWN;
    std::vector<std::uint8_t> scratch_;
};

} // namespace muxivo::d3d9

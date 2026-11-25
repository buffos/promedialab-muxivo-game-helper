#include "present_readback.h"

#include "window_capture_affinity.h"

#include <utility>

namespace muxivo::d3d9 {
namespace {

constexpr DWORD restrictedFlags = D3DPRESENTFLAG_RESTRICTED_CONTENT
    | D3DPRESENTFLAG_RESTRICT_SHARED_RESOURCE_DRIVER;

bool supports(const D3DSURFACE_DESC& desc) noexcept {
    return desc.Width > 0 && desc.Width <= 3840 && desc.Height > 0 && desc.Height <= 2160
        && (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8
            || desc.Format == D3DFMT_X8B8G8R8 || desc.Format == D3DFMT_A8B8G8R8);
}

bool isBlueFirst(D3DFORMAT format) noexcept {
    return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8;
}

} // namespace

void PresentReadback::reset() noexcept {
    resolve_.Reset();
    staging_.Reset();
    device_.Reset();
    width_ = height_ = 0;
    format_ = D3DFMT_UNKNOWN;
    std::vector<std::uint8_t>{}.swap(scratch_);
}

HRESULT PresentReadback::cooperativeStatus(IDirect3DDevice9& device) noexcept {
    const auto hr = device.TestCooperativeLevel();
    if (FAILED(hr)) reset();
    return hr;
}

HRESULT PresentReadback::ensureSurfaces(
    IDirect3DDevice9& device, IUnknown& identity, const D3DSURFACE_DESC& desc) noexcept {
    const bool multisampled = desc.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (staging_ && device_.Get() == &identity && width_ == desc.Width
        && height_ == desc.Height && format_ == desc.Format && bool(resolve_) == multisampled) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<IDirect3DSurface9> staging;
    auto hr = device.CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
        D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> resolve;
    if (multisampled) {
        hr = device.CreateRenderTarget(desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, nullptr);
        if (FAILED(hr)) return hr;
    }
    staging_ = std::move(staging);
    resolve_ = std::move(resolve);
    device_ = &identity;
    width_ = desc.Width;
    height_ = desc.Height;
    format_ = desc.Format;
    return S_OK;
}

HRESULT PresentReadback::read(
    IDirect3DDevice9& device, IDirect3DSwapChain9& swapChain,
    HWND presentWindow, bool hasWindowOverride, SdrFrame& frame) {
    auto hr = cooperativeStatus(device);
    if (FAILED(hr)) return hr;

    D3DPRESENT_PARAMETERS params{};
    hr = swapChain.GetPresentParameters(&params);
    if (FAILED(hr)) return hr;
    if (params.Flags & restrictedFlags) return E_ACCESSDENIED;

    Microsoft::WRL::ComPtr<IDirect3DDevice9> swapDevice;
    Microsoft::WRL::ComPtr<IUnknown> identity, swapIdentity;
    if (FAILED(hr = swapChain.GetDevice(&swapDevice))
        || FAILED(hr = swapDevice.As(&swapIdentity))
        || FAILED(hr = device.QueryInterface(IID_PPV_ARGS(&identity)))) return hr;
    if (swapIdentity.Get() != identity.Get()) return E_INVALIDARG;
    HWND swapWindow = params.hDeviceWindow;
    if (!swapWindow) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        hr = device.GetCreationParameters(&creation);
        if (FAILED(hr)) return hr;
        swapWindow = creation.hFocusWindow;
    }
    if ((presentWindow != swapWindow && !hasWindowOverride)
        || !presentWindowAllowsCapture(presentWindow)
        || (swapWindow && !presentWindowAllowsCapture(swapWindow))) {
        return E_ACCESSDENIED;
    }

    Microsoft::WRL::ComPtr<IDirect3DSurface9> backBuffer;
    hr = swapChain.GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr)) return hr;
    D3DSURFACE_DESC desc{};
    hr = backBuffer->GetDesc(&desc);
    if (FAILED(hr)) return hr;
    if (!supports(desc)) return D3DERR_NOTAVAILABLE;
    hr = ensureSurfaces(device, *identity.Get(), desc);
    if (FAILED(hr)) return hr;
    auto* source = backBuffer.Get();
    if (resolve_) {
        hr = device.StretchRect(source, nullptr, resolve_.Get(), nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) return hr;
        source = resolve_.Get();
    }
    hr = device.GetRenderTargetData(source, staging_.Get());
    if (FAILED(hr)) return hr;

    const auto stride = static_cast<std::size_t>(desc.Width) * 4;
    scratch_.resize(stride * desc.Height);
    D3DLOCKED_RECT mapped{};
    hr = staging_->LockRect(&mapped, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return hr;
    if (!mapped.pBits || mapped.Pitch <= 0 || static_cast<std::size_t>(mapped.Pitch) < stride) {
        staging_->UnlockRect();
        return E_UNEXPECTED;
    }
    const bool blueFirst = isBlueFirst(desc.Format);
    const auto* pixels = static_cast<const std::uint8_t*>(mapped.pBits);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * mapped.Pitch;
        auto* target = scratch_.data() + static_cast<std::size_t>(y) * stride;
        for (UINT x = 0; x < desc.Width; ++x, row += 4, target += 4) {
            target[0] = row[blueFirst ? 2 : 0];
            target[1] = row[1];
            target[2] = row[blueFirst ? 0 : 2];
            target[3] = 255;
        }
    }
    hr = staging_->UnlockRect();
    if (FAILED(hr)) {
        reset();
        return hr;
    }
    hr = cooperativeStatus(device);
    if (FAILED(hr)) return hr;
    if (!presentWindowAllowsCapture(presentWindow)) return E_ACCESSDENIED;
    frame.width = desc.Width;
    frame.height = desc.Height;
    frame.rgba.swap(scratch_);
    return S_OK;
}

} // namespace muxivo::d3d9

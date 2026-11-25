#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace muxivo::d3d11 {

class D3D11Readback {
public:
    D3D11Readback() = default;
    ~D3D11Readback() = default;

    D3D11Readback(const D3D11Readback&) = delete;
    D3D11Readback& operator=(const D3D11Readback&) = delete;

    void reset() noexcept;
    bool copy(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* backBuffer,
        std::vector<std::uint8_t>& packet);

private:
    bool ensureTextures(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        DXGI_FORMAT stagingFormat,
        UINT sampleCount) noexcept;
    bool readMappedTexture(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* staging,
        UINT mapFlags,
        std::vector<std::uint8_t>& packet,
        HRESULT& outHr);

    ID3D11Device* device_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    DXGI_FORMAT stagingFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT sampleCount_ = 1;
    bool blueFirst_ = false;
    bool firstFrameDelivered_ = false;
    std::size_t firstPending_ = 0;
    std::size_t pendingCount_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> staging_;
};

} // namespace muxivo::d3d11

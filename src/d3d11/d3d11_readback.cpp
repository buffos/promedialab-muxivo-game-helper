#include "d3d11_readback.h"

#include <cstring>

#include "d3d11_protocol.h"
#include "readback_queue.h"

namespace muxivo::d3d11 {
namespace {

constexpr std::uint32_t maxCaptureWidth = 3840;
constexpr std::uint32_t maxCaptureHeight = 2160;

bool classifyFormat(DXGI_FORMAT format, DXGI_FORMAT& stagingFormat, bool& blueFirst) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            stagingFormat = format;
            blueFirst = false;
            return true;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            stagingFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            blueFirst = false;
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            stagingFormat = format;
            blueFirst = true;
            return true;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            stagingFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            blueFirst = true;
            return true;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            stagingFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
            blueFirst = true;
            return true;
        default:
            return false;
    }
}

std::size_t pixelBytes(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}

void packPixels(
    const std::uint8_t* srcBase,
    UINT rowPitch,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT format,
    bool blueFirst,
    std::vector<std::uint8_t>& packet) {
    constexpr std::size_t headerBytes = 16;
    const std::size_t stride = static_cast<std::size_t>(width) * pixelBytes(format);
    packet.resize(headerBytes + stride * height);
    std::memcpy(packet.data(), muxivoD3D11FrameTag.data(), muxivoD3D11FrameTag.size());
    std::memcpy(packet.data() + 8, &width, sizeof(width));
    std::memcpy(packet.data() + 12, &height, sizeof(height));

    std::uint8_t* dstBase = packet.data() + headerBytes;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        // Send the storage format. Muxivo applies the color interpretation selected for the source.
        const auto storage = format == DXGI_FORMAT_R16G16B16A16_FLOAT
            ? D3D11FrameStorage::Rgba16Float : D3D11FrameStorage::Rgb10A2;
        packet[5] = static_cast<std::uint8_t>(storage);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(dstBase + stride * y, srcBase + static_cast<std::size_t>(rowPitch) * y, stride);
        }
        return;
    }
    if (blueFirst) {
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* src = srcBase + static_cast<std::size_t>(rowPitch) * y;
            std::uint8_t* dst = dstBase + stride * y;
            for (std::uint32_t x = 0; x < width; ++x) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = 255;
                src += 4;
                dst += 4;
            }
        }
    } else {
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* src = srcBase + static_cast<std::size_t>(rowPitch) * y;
            std::uint8_t* dst = dstBase + stride * y;
            for (std::uint32_t x = 0; x < width; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
                src += 4;
                dst += 4;
            }
        }
    }
}

} // namespace

void D3D11Readback::reset() noexcept {
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    stagingFormat_ = DXGI_FORMAT_UNKNOWN;
    sampleCount_ = 1;
    blueFirst_ = false;
    firstFrameDelivered_ = false;
    firstPending_ = 0;
    pendingCount_ = 0;
    resolved_.Reset();
    for (auto& texture : staging_) texture.Reset();
}

bool D3D11Readback::ensureTextures(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT stagingFormat,
    UINT sampleCount) noexcept {
    if (device_ == device && width_ == width && height_ == height
        && stagingFormat_ == stagingFormat && sampleCount_ == sampleCount
        && staging_.front() && (sampleCount <= 1 || resolved_)) {
        return true;
    }

    reset();

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = stagingFormat;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (auto& texture : staging_) {
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &texture))) {
            reset();
            return false;
        }
    }

    if (sampleCount > 1) {
        D3D11_TEXTURE2D_DESC resolveDesc = {};
        resolveDesc.Width = width;
        resolveDesc.Height = height;
        resolveDesc.MipLevels = 1;
        resolveDesc.ArraySize = 1;
        resolveDesc.Format = stagingFormat;
        resolveDesc.SampleDesc.Count = 1;
        resolveDesc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(&resolveDesc, nullptr, &resolved_))) {
            reset();
            return false;
        }
    }

    device_ = device;
    width_ = width;
    height_ = height;
    stagingFormat_ = stagingFormat;
    sampleCount_ = sampleCount;
    return true;
}

bool D3D11Readback::readMappedTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* staging,
    UINT mapFlags,
    std::vector<std::uint8_t>& packet,
    HRESULT& outHr) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    outHr = context->Map(staging, 0, D3D11_MAP_READ, mapFlags, &mapped);
    if (FAILED(outHr)) {
        packet.clear();
        return false;
    }
    const std::size_t minRowBytes = static_cast<std::size_t>(width_) * pixelBytes(stagingFormat_);
    if (!mapped.pData || mapped.RowPitch < minRowBytes) {
        context->Unmap(staging, 0);
        packet.clear();
        outHr = E_FAIL;
        return false;
    }
    try {
        packPixels(static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch, width_, height_, stagingFormat_, blueFirst_, packet);
    } catch (...) {
        context->Unmap(staging, 0);
        packet.clear();
        outHr = E_OUTOFMEMORY;
        return false;
    }
    context->Unmap(staging, 0);
    return true;
}

bool D3D11Readback::copy(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* backBuffer,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (!device || !context || !backBuffer) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    bool blueFirst = false;
    if (desc.Width == 0 || desc.Width > maxCaptureWidth
        || desc.Height == 0 || desc.Height > maxCaptureHeight
        || !classifyFormat(desc.Format, stagingFormat, blueFirst)) {
        return false;
    }

    const UINT sampleCount = desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count : 1;
    if (!ensureTextures(device, desc.Width, desc.Height, stagingFormat, sampleCount)) {
        return false;
    }
    blueFirst_ = blueFirst;

    const auto submitCopy = [&](ID3D11Texture2D* destination) {
        ID3D11Texture2D* source = backBuffer;
        if (sampleCount > 1) {
            context->ResolveSubresource(resolved_.Get(), 0, backBuffer, 0, stagingFormat);
            source = resolved_.Get();
        }
        context->CopyResource(destination, source);
    };

    if (!firstFrameDelivered_) {
        submitCopy(staging_.front().Get());
        HRESULT result = S_OK;
        firstFrameDelivered_ = readMappedTexture(context, staging_.front().Get(), 0, packet, result);
        return firstFrameDelivered_;
    }

    const auto completion = collectNewestPending(staging_.size(), firstPending_, pendingCount_,
        [&](std::size_t slot, UINT flags) {
            HRESULT result = S_OK;
            readMappedTexture(context, staging_[slot].Get(), flags, packet, result);
            return result;
        });
    if (FAILED(completion)) {
        reset();
        return false;
    }

    // Keep the remaining slots working while earlier copies are in flight.
    if (pendingCount_ < staging_.size()) {
        const auto slot = (firstPending_ + pendingCount_) % staging_.size();
        submitCopy(staging_[slot].Get());
        ++pendingCount_;
    }

    return !packet.empty();
}

} // namespace muxivo::d3d11

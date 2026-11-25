#include "frame_format.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using muxivo::FrameFormat;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

void vulkan_swapchain_requires_matching_storage_and_colorspace() {
    constexpr auto srgb = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    constexpr auto scrgb = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    constexpr auto pq = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    constexpr auto fp16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr auto rgb10 = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    constexpr auto bgr10 = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    require(muxivo::frameFormat(VK_FORMAT_R8G8B8A8_UNORM, srgb) == FrameFormat::Rgba8, "RGBA8 UNORM sRGB");
    require(muxivo::frameFormat(VK_FORMAT_R8G8B8A8_SRGB, srgb) == FrameFormat::Rgba8, "RGBA8 sRGB");
    require(muxivo::frameFormat(VK_FORMAT_B8G8R8A8_UNORM, srgb) == FrameFormat::Bgra8, "BGRA8 UNORM sRGB");
    require(muxivo::frameFormat(VK_FORMAT_B8G8R8A8_SRGB, srgb) == FrameFormat::Bgra8, "BGRA8 sRGB");
    require(muxivo::frameFormat(fp16, scrgb) == FrameFormat::ScRgb, "FP16 scRGB");
    require(muxivo::frameFormat(rgb10, pq) == FrameFormat::Hdr10Rgba, "RGB10 PQ");
    require(muxivo::frameFormat(bgr10, pq) == FrameFormat::Hdr10Bgra, "BGR10 PQ");
    require(!muxivo::frameFormat(rgb10, srgb), "10-bit SDR cannot be interpreted as PQ");
    require(!muxivo::frameFormat(rgb10, VK_COLOR_SPACE_HDR10_HLG_EXT), "HLG cannot be interpreted as PQ");
    require(!muxivo::frameFormat(fp16, pq), "FP16 PQ cannot be interpreted as scRGB");
    require(!muxivo::frameFormat(VK_FORMAT_R8G8B8A8_UNORM, scrgb), "8-bit linear is not scRGB FP16");
}

void vulkan_packets_preserve_color_channels_and_storage_size() {
    struct Layout { FrameFormat format; uint8_t storage; size_t bytes; };
    constexpr Layout layouts[] = {
        {FrameFormat::Rgba8, 1, 4}, {FrameFormat::Bgra8, 1, 4}, {FrameFormat::ScRgb, 2, 8},
        {FrameFormat::Hdr10Rgba, 3, 4}, {FrameFormat::Hdr10Bgra, 3, 4},
    };
    for (const auto& layout : layouts) {
        std::array<uint8_t, muxivo::frameHeaderSize> header = {};
        const decltype(header) expected = {'M', 'X', 'G', 'C', 1, layout.storage, 1, 0, 0, 15, 0, 0, 112, 8, 0, 0};
        muxivo::writeFrameHeader(header.data(), {3840, 2160}, layout.format);
        require(header == expected, "MXGC Vulkan packet header");
        require(muxivo::pixelSize(layout.format) == layout.bytes, "bytes per texel");
    }

    std::array<uint8_t, 8> output = {};
    constexpr std::array<uint8_t, 8> rgba = {10, 20, 230, 1, 99, 49, 111, 233};
    constexpr std::array<uint8_t, 8> bgra = {230, 20, 10, 1, 111, 49, 99, 233};
    constexpr std::array<uint8_t, 8> sdr = {10, 20, 230, 255, 99, 49, 111, 255};
    muxivo::copyFramePixels(output.data(), rgba.data(), rgba.size(), FrameFormat::Rgba8);
    require(output == sdr, "RGBA8 makes alpha opaque");
    muxivo::copyFramePixels(output.data(), bgra.data(), bgra.size(), FrameFormat::Bgra8);
    require(output == sdr, "BGRA8 reorders channels and makes alpha opaque");
    constexpr std::array<uint16_t, 4> scRgb = {0xb400, 0x3a00, 0x4a40, 0x3c00};
    muxivo::copyFramePixels(output.data(), scRgb.data(), sizeof(scRgb), FrameFormat::ScRgb);
    require(std::memcmp(output.data(), scRgb.data(), sizeof(scRgb)) == 0, "scRGB preserves negative and highlight channels");

    constexpr uint32_t rgb10 = (1u << 30) | (545u << 20) | (477u << 10) | 450u;
    constexpr uint32_t bgr10 = (1u << 30) | (450u << 20) | (477u << 10) | 545u;
    muxivo::copyFramePixels(output.data(), &rgb10, sizeof(rgb10), FrameFormat::Hdr10Rgba);
    require(std::memcmp(output.data(), &rgb10, sizeof(rgb10)) == 0, "RGB10 preserves the packed PQ samples");
    muxivo::copyFramePixels(output.data(), &bgr10, sizeof(bgr10), FrameFormat::Hdr10Bgra);
    require(std::memcmp(output.data(), &rgb10, sizeof(rgb10)) == 0, "BGR10 normalizes to red-low without quantization");

    // Exercise unaligned input and output, a block of varying pixels and its last pixel.
    constexpr size_t pixelCount = 17;
    std::array<uint8_t, 1 + 4 * pixelCount> unalignedInput{};
    std::array<uint8_t, 2 + 4 * pixelCount> unalignedOutput{};
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const size_t offset = 1 + pixel * 4;
        unalignedInput[offset] = static_cast<uint8_t>(pixel + 30);
        unalignedInput[offset + 1] = static_cast<uint8_t>(pixel + 70);
        unalignedInput[offset + 2] = static_cast<uint8_t>(pixel + 90);
        unalignedInput[offset + 3] = 30;
    }
    muxivo::copyFramePixels(unalignedOutput.data() + 1, unalignedInput.data() + 1, 4 * pixelCount, FrameFormat::Bgra8);
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const size_t offset = 1 + pixel * 4;
        require(unalignedOutput[offset] == pixel + 90 && unalignedOutput[offset + 1] == pixel + 70
            && unalignedOutput[offset + 2] == pixel + 30 && unalignedOutput[offset + 3] == 255,
            "Every unaligned BGRA8 pixel is converted to opaque RGBA8");
    }
    require(unalignedOutput.front() == 0 && unalignedOutput.back() == 0, "Conversion stays within the declared pixel range");

    constexpr std::array<uint32_t, 5> rgbaBlock = {0x025713e0, 0x132e70fe, 0xc3a74e12, 0xff000001, 0x00000c07};
    std::array<uint32_t, 5> converted{};
    muxivo::copyFramePixels(reinterpret_cast<uint8_t*>(converted.data()), rgbaBlock.data(), sizeof(rgbaBlock), FrameFormat::Rgba8);
    for (size_t index = 0; index < rgbaBlock.size(); ++index) {
        require(converted[index] == (rgbaBlock[index] | 0xff000000u), "Opaque RGBA preserves RGB throughout the block");
    }

    constexpr std::array<uint32_t, 5> packedBgr = {bgr10, 0xaa7d809du, 0x000fffffu, 0xc0000000u, 0x40000001u};
    muxivo::copyFramePixels(reinterpret_cast<uint8_t*>(converted.data()), packedBgr.data(), sizeof(packedBgr), FrameFormat::Hdr10Bgra);
    for (size_t index = 0; index < packedBgr.size(); ++index) {
        const auto input = packedBgr[index];
        const auto packed = converted[index];
        require((packed & 0xc00ffc00u) == (input & 0xc00ffc00u)
            && (packed & 0x3ffu) == ((input >> 20) & 0x3ffu)
            && ((packed >> 20) & 0x3ffu) == (input & 0x3ffu), "BGR10 preserves every channel throughout the block");
    }
}

int main() {
    vulkan_swapchain_requires_matching_storage_and_colorspace();
    vulkan_packets_preserve_color_channels_and_storage_size();
}

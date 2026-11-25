#include "frame_format.h"

#include <array>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace muxivo {

std::optional<FrameFormat> frameFormat(VkFormat format, VkColorSpaceKHR colorSpace) {
    switch (colorSpace) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) return FrameFormat::Rgba8;
        if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) return FrameFormat::Bgra8;
        break;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        if (format == VK_FORMAT_R16G16B16A16_SFLOAT) return FrameFormat::ScRgb;
        break;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) return FrameFormat::Hdr10Rgba;
        if (format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) return FrameFormat::Hdr10Bgra;
        break;
    default:
        break;
    }
    return std::nullopt;
}

size_t pixelSize(FrameFormat format) {
    return format == FrameFormat::ScRgb ? 8 : 4;
}

uint8_t frameStorage(FrameFormat format) {
    switch (format) {
    case FrameFormat::ScRgb: return 2;
    case FrameFormat::Hdr10Rgba:
    case FrameFormat::Hdr10Bgra: return 3;
    default: return 1;
    }
}

void writeFrameHeader(uint8_t* header, VkExtent2D extent, FrameFormat format) {
    const std::array<uint8_t, 8> tag = { 'M', 'X', 'G', 'C', 1, frameStorage(format), 1, 0 };
    std::memcpy(header, tag.data(), tag.size());
    std::memcpy(header + 8, &extent.width, sizeof(extent.width));
    std::memcpy(header + 12, &extent.height, sizeof(extent.height));
}

template<FrameFormat Format>
static void copyPackedPixels(uint8_t* output, const uint8_t* input, size_t bytes) {
    size_t offset = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const auto opaqueAlpha = _mm_set1_epi32(static_cast<int32_t>(0xff000000u));
    for (; bytes - offset >= sizeof(__m128i); offset += sizeof(__m128i)) {
        __m128i packed;
        std::memcpy(&packed, input + offset, sizeof(packed));
        if constexpr (Format == FrameFormat::Bgra8) {
            const auto channelMask = _mm_set1_epi32(0xff);
            const auto red = _mm_and_si128(_mm_srli_epi32(packed, 16), channelMask);
            const auto blue = _mm_slli_epi32(_mm_and_si128(packed, channelMask), 16);
            const auto green = _mm_and_si128(packed, _mm_set1_epi32(0x0000ff00));
            packed = _mm_or_si128(_mm_or_si128(red, blue), _mm_or_si128(green, opaqueAlpha));
        } else if constexpr (Format == FrameFormat::Hdr10Bgra) {
            const auto channelMask = _mm_set1_epi32(0x3ff);
            const auto red = _mm_and_si128(_mm_srli_epi32(packed, 20), channelMask);
            const auto blue = _mm_slli_epi32(_mm_and_si128(packed, channelMask), 20);
            const auto greenAlpha = _mm_and_si128(packed, _mm_set1_epi32(static_cast<int32_t>(0xc00ffc00u)));
            packed = _mm_or_si128(_mm_or_si128(red, blue), greenAlpha);
        } else {
            packed = _mm_or_si128(packed, opaqueAlpha);
        }
        std::memcpy(output + offset, &packed, sizeof(packed));
    }
#endif
    for (; offset < bytes; offset += sizeof(uint32_t)) {
        uint32_t packed;
        std::memcpy(&packed, input + offset, sizeof(packed));
        if constexpr (Format == FrameFormat::Hdr10Bgra) {
            // The receiver uses R10G10B10A2 with red in the low bits.
            constexpr uint32_t channelMask = 0x3ff;
            packed = (packed & 0xc00ffc00) | ((packed & channelMask) << 20) | ((packed >> 20) & channelMask);
        } else if constexpr (Format == FrameFormat::Bgra8) {
            packed = 0xff000000 | (packed & 0x0000ff00) | ((packed & 0xff) << 16) | ((packed >> 16) & 0xff);
        } else {
            packed |= 0xff000000;
        }
        std::memcpy(output + offset, &packed, sizeof(packed));
    }
}

void copyFramePixels(uint8_t* output, const void* input, size_t bytes, FrameFormat format) {
    const auto* source = static_cast<const uint8_t*>(input);
    switch (format) {
    case FrameFormat::Rgba8: return copyPackedPixels<FrameFormat::Rgba8>(output, source, bytes);
    case FrameFormat::Bgra8: return copyPackedPixels<FrameFormat::Bgra8>(output, source, bytes);
    case FrameFormat::Hdr10Bgra: return copyPackedPixels<FrameFormat::Hdr10Bgra>(output, source, bytes);
    case FrameFormat::ScRgb:
    case FrameFormat::Hdr10Rgba: std::memcpy(output, input, bytes); break;
    }
}

} // namespace muxivo

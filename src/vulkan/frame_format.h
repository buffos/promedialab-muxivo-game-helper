#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace muxivo {

enum class FrameFormat { Rgba8, Bgra8, ScRgb, Hdr10Rgba, Hdr10Bgra };

std::optional<FrameFormat> frameFormat(VkFormat format, VkColorSpaceKHR colorSpace);
size_t pixelSize(FrameFormat format);
uint8_t frameStorage(FrameFormat format);

constexpr size_t frameHeaderSize = 16;
void writeFrameHeader(uint8_t* header, VkExtent2D extent, FrameFormat format);
void copyFramePixels(uint8_t* output, const void* input, size_t bytes, FrameFormat format);

} // namespace muxivo

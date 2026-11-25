#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "frame_format.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace muxivo {

std::optional<uint32_t> readbackMemoryType(uint32_t supported,
    const VkPhysicalDeviceMemoryProperties& properties);

class Readback {
public:
    Readback(VkDevice device, PFN_vkGetDeviceProcAddr dispatch, PFN_vkSetDeviceLoaderData loaderData);
    ~Readback();
    Readback(const Readback&) = delete;
    Readback& operator=(const Readback&) = delete;

    bool prepare(VkExtent2D extent, FrameFormat format, uint32_t family,
        const VkPhysicalDeviceMemoryProperties& properties);
    std::optional<VkResult> copy(VkQueue queue, VkImage image, const VkPresentInfoKHR& original,
        std::vector<uint8_t>& packet);
    bool matches(uint32_t family) const { return family == family_; }

private:
    template<typename Function> Function command(const char* name) const {
        return reinterpret_cast<Function>(dispatch_(device_, name));
    }

    VkDevice device_;
    PFN_vkGetDeviceProcAddr dispatch_;
    PFN_vkSetDeviceLoaderData loaderData_;
    VkExtent2D extent_ = {};
    FrameFormat format_ = FrameFormat::Rgba8;
    uint32_t family_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    bool poisoned_ = false;
};

} // namespace muxivo

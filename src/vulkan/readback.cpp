#include "readback.h"

#include <limits>

namespace muxivo {

std::optional<uint32_t> readbackMemoryType(uint32_t supported,
    const VkPhysicalDeviceMemoryProperties& properties) {
    constexpr auto hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::optional<uint32_t> compatible;
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if (!(supported & (1u << index)) || (flags & hostMemory) != hostMemory) continue;
        // Captured pixels flow from device to CPU. Uncached upload memory is
        // still usable when the driver has no compatible cached coherent type.
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) return index;
        if (!compatible) compatible = index;
    }
    return compatible;
}

Readback::Readback(VkDevice device, PFN_vkGetDeviceProcAddr dispatch, PFN_vkSetDeviceLoaderData loaderData)
    : device_(device), dispatch_(dispatch), loaderData_(loaderData) {}

Readback::~Readback() {
    if (mapped_) command<PFN_vkUnmapMemory>("vkUnmapMemory")(device_, memory_);
    if (fence_) command<PFN_vkDestroyFence>("vkDestroyFence")(device_, fence_, nullptr);
    if (pool_) command<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device_, pool_, nullptr);
    if (buffer_) command<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device_, buffer_, nullptr);
    if (memory_) command<PFN_vkFreeMemory>("vkFreeMemory")(device_, memory_, nullptr);
}

bool Readback::prepare(VkExtent2D extent, FrameFormat format, uint32_t family,
    const VkPhysicalDeviceMemoryProperties& properties) {
    extent_ = extent;
    format_ = format;
    family_ = family;
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent.width) * extent.height * pixelSize(format);
    VkBufferCreateInfo buffer = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer.size = size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (command<PFN_vkCreateBuffer>("vkCreateBuffer")(device_, &buffer, nullptr, &buffer_) != VK_SUCCESS) return false;

    VkMemoryRequirements requirements = {};
    command<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device_, buffer_, &requirements);
    const auto memoryType = readbackMemoryType(requirements.memoryTypeBits, properties);
    if (!memoryType) return false;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *memoryType;
    if (command<PFN_vkAllocateMemory>("vkAllocateMemory")(device_, &allocation, nullptr, &memory_) != VK_SUCCESS
        || command<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device_, buffer_, memory_, 0) != VK_SUCCESS
        || command<PFN_vkMapMemory>("vkMapMemory")(device_, memory_, 0, size, 0, &mapped_) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = family;
    if (command<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device_, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo commands = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    commands.commandPool = pool_;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    if (command<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device_, &commands, &commands_) != VK_SUCCESS) return false;
    if (loaderData_(device_, commands_) != VK_SUCCESS) return false;
    const VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return command<PFN_vkCreateFence>("vkCreateFence")(device_, &fence, nullptr, &fence_) == VK_SUCCESS;
}

std::optional<VkResult> Readback::copy(VkQueue queue, VkImage image, const VkPresentInfoKHR& original,
    std::vector<uint8_t>& packet) {
    if (poisoned_) return std::nullopt;
    // All potentially throwing allocations happen before consuming the game's wait semaphores.
    const size_t pixelBytes = static_cast<size_t>(extent_.width) * extent_.height * pixelSize(format_);
    packet.resize(frameHeaderSize + pixelBytes);
    writeFrameHeader(packet.data(), extent_, format_);
    const std::vector<VkPipelineStageFlags> stages(original.waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (command<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands_, 0) != VK_SUCCESS) return std::nullopt;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (command<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands_, &begin) != VK_SUCCESS) return std::nullopt;
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    auto pipelineBarrier = command<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { extent_.width, extent_.height, 1 };
    command<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        commands_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    pipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (command<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands_) != VK_SUCCESS
        || command<PFN_vkResetFences>("vkResetFences")(device_, 1, &fence_) != VK_SUCCESS) return std::nullopt;

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = original.waitSemaphoreCount;
    submit.pWaitSemaphores = original.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    const auto submitted = command<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence_);
    if (submitted != VK_SUCCESS) return submitted;
    const auto completed = command<PFN_vkWaitForFences>("vkWaitForFences")(
        device_, 1, &fence_, VK_TRUE, std::numeric_limits<uint64_t>::max());
    if (completed != VK_SUCCESS) {
        poisoned_ = true;
        return completed;
    }

    copyFramePixels(packet.data() + frameHeaderSize, mapped_, pixelBytes, format_);
    return VK_SUCCESS;
}

} // namespace muxivo

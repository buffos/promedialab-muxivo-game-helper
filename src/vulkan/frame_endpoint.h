#pragma once

#include "overlapped_frame_pipe_writer.h"
#include "frame_format.h"

#include <algorithm>

namespace muxivo {

inline DWORD vulkanFramePipeCapacity(VkExtent2D extent, FrameFormat format) {
    constexpr DWORD baselineCapacity = 12 * 1024 * 1024;
    constexpr DWORD maximumCapacity = 32 * 1024 * 1024;
    const uint64_t pixels = uint64_t{extent.width} * extent.height;
    const auto bytesPerPixel = pixelSize(format);
    if (pixels > (maximumCapacity - frameHeaderSize) / bytesPerPixel) return maximumCapacity;
    const auto packetSize = static_cast<DWORD>(frameHeaderSize + pixels * bytesPerPixel);
    return std::max(baselineCapacity, packetSize);
}

class VulkanFrameEndpoint {
public:
    VulkanFrameEndpoint(HWND window, VkExtent2D extent, FrameFormat format)
        : frames(window, L"Muxivo.GameCapture.Vulkan.v1", vulkanFramePipeCapacity(extent, format)),
          storage(frameStorage(format)) {
        wchar_t name[128] = {};
        swprintf_s(name, L"Local\\Muxivo.GameCapture.Vulkan.Present.v2.%lu.%llx", GetCurrentProcessId(),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)));
        const auto security = createCurrentUserSecurityDescriptor();
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        SetLastError(ERROR_SUCCESS);
        mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
            sizeof(LONG64), name));
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("Could not create unique Vulkan presentation state");
        }
        record = static_cast<LONG64*>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
        if (!record) throw std::runtime_error("Could not map Vulkan presentation state");
    }

    ~VulkanFrameEndpoint() {
        InterlockedExchange64(record, 0);
        UnmapViewOfFile(record);
    }

    bool wantsFrame() { return frames.wantsFrame(); }
    std::shared_ptr<std::vector<uint8_t>> acquireFrameBuffer() { return frames.acquireFrameBuffer(); }

    void presented(const std::shared_ptr<const std::vector<uint8_t>>& frame) {
        // Keep the active format and last successful present in one atomic observation.
        InterlockedExchange64(record, static_cast<LONG64>((GetTickCount64() << 8) | storage));
        if (frame) frames.publish(frame);
    }

private:
    OverlappedFramePipeWriter frames;
    const uint8_t storage;
    UniqueHandle mapping;
    LONG64* record = nullptr;
};

} // namespace muxivo

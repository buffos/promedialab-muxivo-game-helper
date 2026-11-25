#pragma once

#include "overlapped_frame_pipe_writer.h"

#include <cstring>

namespace muxivo {

inline constexpr DWORD sdrFramePipeCapacity = 12 * 1024 * 1024;

inline void publishSdrFrame(OverlappedFramePipeWriter& pipe, std::uint8_t provider,
    UINT width, UINT height, const std::vector<std::uint8_t>& rgba) {
    constexpr std::size_t headerSize = 16;
    const std::array<std::uint8_t, 8> tag{'M', 'X', 'G', 'C', 1, 1, provider, 0};
    auto packet = pipe.acquireFrameBuffer();
    packet->resize(headerSize + rgba.size());
    std::memcpy(packet->data(), tag.data(), tag.size());
    std::memcpy(packet->data() + 8, &width, sizeof(width));
    std::memcpy(packet->data() + 12, &height, sizeof(height));
    std::memcpy(packet->data() + headerSize, rgba.data(), rgba.size());
    pipe.publish(packet);
}

} // namespace muxivo

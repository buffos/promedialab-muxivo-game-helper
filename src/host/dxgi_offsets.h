#pragma once

#include <cstdint>

namespace muxivo {

struct DxgiOffsets {
    std::uint32_t presentRva = 0;
    std::uint32_t present1Rva = 0;
    std::uint32_t resizeBuffersRva = 0;
    std::uint32_t resizeBuffers1Rva = 0;
    std::uint32_t secondaryPresentRva = 0;
};

bool probeDxgiOffsets(DxgiOffsets& out) noexcept;

} // namespace muxivo

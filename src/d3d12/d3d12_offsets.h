#pragma once

#include <cstdint>

namespace muxivo::capture::d3d12 {

struct DxgiSwapOffsets {
    std::uint32_t presentRva = 0;
    std::uint32_t resizeBuffersRva = 0;
    std::uint32_t present1Rva = 0;
    std::uint32_t setColorSpace1Rva = 0;
    std::uint32_t resizeBuffers1Rva = 0;
    std::uint32_t secondaryPresentRva = 0;
};

// Probes COM vtable RVAs inside dxgi.dll for x64 DXGI flip-model and legacy swap chains.
bool probeDxgiSwapOffsets(DxgiSwapOffsets& out) noexcept;

} // namespace muxivo::capture::d3d12

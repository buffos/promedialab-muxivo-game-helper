#pragma once

#include <cstdint>

namespace muxivo::capture::d3d9 {

struct D3D9Offsets {
    std::uint32_t deviceResetRva = 0;
    std::uint32_t devicePresentRva = 0;
    std::uint32_t devicePresentExRva = 0;
    std::uint32_t deviceResetExRva = 0;
    std::uint32_t swapChainPresentRva = 0;
};

// Probes COM vtable RVAs inside d3d9.dll for the current architecture (x86 or x64).
bool probeD3D9Offsets(D3D9Offsets& out) noexcept;

} // namespace muxivo::capture::d3d9

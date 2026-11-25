#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

namespace muxivo {

inline constexpr wchar_t muxivoD3D11PresenceEvent[] = L"Local\\Muxivo.D3D11GameCapture.v1";
inline constexpr wchar_t muxivoD3D11PipeEndpoint[] = L"Muxivo.GameCapture.D3D11.v1";
inline constexpr wchar_t muxivoD3D11ConfigPrefix[] = L"Local\\Muxivo.D3D11GameCapture.Config.v1.";
inline constexpr wchar_t muxivoD3D11LoadedPrefix[] = L"Local\\Muxivo.D3D11GameCapture.Loaded.v1.";

enum class D3D11FrameStorage : std::uint8_t {
    Rgba8 = 1,
    Rgba16Float = 2,
    Rgb10A2 = 3,
};

inline constexpr std::array<std::uint8_t, 8> muxivoD3D11FrameTag = {
    'M', 'X', 'G', 'C', 1, static_cast<std::uint8_t>(D3D11FrameStorage::Rgba8), 2, 0
};

inline constexpr std::uint32_t muxivoD3D11ConfigMagic = 0x3144584Du; // "MXD1"
inline constexpr std::uint32_t muxivoD3D11ConfigVersion = 1;

struct D3D11HookConfig {
    std::uint32_t magic;
    std::uint32_t version;
    volatile std::uint32_t hostPid;
    std::uint32_t targetPid;
    volatile std::uint64_t targetHwnd;
    volatile std::uint32_t presentRva;
    volatile std::uint32_t present1Rva;
    volatile std::uint32_t resizeBuffersRva;
    volatile std::uint32_t resizeBuffers1Rva;
    volatile std::uint32_t secondaryPresentRva;
    volatile std::uint32_t generation;
    volatile std::uint32_t active;
};

static_assert(std::is_trivially_copyable_v<D3D11HookConfig>);
static_assert(offsetof(D3D11HookConfig, targetHwnd) % 8 == 0);

inline std::uint64_t readConfigU64(const volatile std::uint64_t& slot) noexcept {
    // Aligned 64-bit reads on x64 are atomic; never use InterlockedCompareExchange on FILE_MAP_READ.
    const std::uint64_t value = slot;
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
}

inline void writeConfigU64(volatile std::uint64_t& slot, std::uint64_t value) noexcept {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&slot), static_cast<LONG64>(value));
}

inline std::uint32_t readConfigU32(const volatile std::uint32_t& slot) noexcept {
    const std::uint32_t value = slot;
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
}

inline void writeConfigU32(volatile std::uint32_t& slot, std::uint32_t value) noexcept {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot), static_cast<LONG>(value));
}

inline std::wstring d3d11ConfigMappingName(DWORD pid) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"%ls%lu", muxivoD3D11ConfigPrefix, pid);
    return buffer;
}

inline std::wstring d3d11LoadedEventName(DWORD pid) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"%ls%lu", muxivoD3D11LoadedPrefix, pid);
    return buffer;
}

} // namespace muxivo

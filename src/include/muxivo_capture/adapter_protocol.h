#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace muxivo::capture {

enum class CaptureApi : std::uint8_t {
    D3D9 = 3,
    D3D12 = 4,
    OpenGL = 5,
};

enum class FrameStorage : std::uint8_t {
    Rgba8 = 1,
};

inline constexpr std::uint32_t maxFrameWidth = 3840;
inline constexpr std::uint32_t maxFrameHeight = 2160;
inline constexpr std::size_t frameHeaderBytes = 16;

inline constexpr std::uint32_t adapterHookConfigMagic = 0x4D584131u; // "MXA1"
inline constexpr std::uint32_t adapterHookConfigVersion = 1;

// Shared configuration layout between 64-bit controller, optional 32-bit attach trampoline,
// and in-process 32-bit / 64-bit adapter hook DLLs. Binary-identical across x86 and x64.
struct AdapterHookConfig {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t api;
    std::uint32_t hostPid;
    std::uint32_t targetPid;
    std::uint32_t generation;
    volatile std::uint32_t active;
    volatile std::uint32_t targetHwndLo;
    volatile std::uint32_t targetHwndHi;
    std::uint32_t reserved0;
    volatile std::uint32_t rva[6];
};

static_assert(std::is_trivially_copyable_v<AdapterHookConfig>);
static_assert(sizeof(AdapterHookConfig) == 64);
static_assert(offsetof(AdapterHookConfig, rva) == 40);

inline std::uint32_t readConfigU32(const volatile std::uint32_t& slot) noexcept {
    const std::uint32_t value = slot;
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
}

inline void writeConfigU32(volatile std::uint32_t& slot, std::uint32_t value) noexcept {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot), static_cast<LONG>(value));
}

inline std::uint64_t readConfigHwnd(const AdapterHookConfig& cfg) noexcept {
    const std::uint32_t lo = readConfigU32(cfg.targetHwndLo);
    const std::uint32_t hi = readConfigU32(cfg.targetHwndHi);
    return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

inline void writeConfigHwnd(AdapterHookConfig& cfg, std::uint64_t hwnd) noexcept {
    writeConfigU32(cfg.targetHwndHi, static_cast<std::uint32_t>((hwnd >> 32) & 0xFFFFFFFFu));
    writeConfigU32(cfg.targetHwndLo, static_cast<std::uint32_t>(hwnd & 0xFFFFFFFFu));
}

inline constexpr const wchar_t* apiEventPrefix(CaptureApi api) noexcept {
    switch (api) {
        case CaptureApi::D3D9: return L"D3D9";
        case CaptureApi::D3D12: return L"D3D12";
        case CaptureApi::OpenGL: return L"OpenGL";
    }
    return L"Unknown";
}

inline constexpr const wchar_t* apiPipeEndpoint(CaptureApi api) noexcept {
    switch (api) {
        case CaptureApi::D3D9: return L"Muxivo.GameCapture.D3D9.v1";
        case CaptureApi::D3D12: return L"Muxivo.GameCapture.D3D12.v1";
        case CaptureApi::OpenGL: return L"Muxivo.GameCapture.OpenGL.v1";
    }
    return L"Muxivo.GameCapture.Unknown.v1";
}

inline std::wstring apiPresenceEventName(CaptureApi api) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"Local\\Muxivo.%lsGameCapture.v1", apiEventPrefix(api));
    return buffer;
}

inline std::wstring apiConfigMappingName(CaptureApi api, DWORD pid) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"Local\\Muxivo.%lsGameCapture.Config.v1.%lu", apiEventPrefix(api), pid);
    return buffer;
}

inline std::wstring apiLoadedEventName(CaptureApi api, DWORD pid) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"Local\\Muxivo.%lsGameCapture.Loaded.v1.%lu", apiEventPrefix(api), pid);
    return buffer;
}

inline std::wstring makeFramePipeName(CaptureApi api, DWORD pid, std::uint64_t hwnd) {
    wchar_t buffer[128] = {};
    swprintf_s(buffer, L"\\\\.\\pipe\\%ls.%lu.%llx",
        apiPipeEndpoint(api), pid, static_cast<unsigned long long>(hwnd));
    return buffer;
}

struct FrameHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FrameStorage storage = FrameStorage::Rgba8;
    CaptureApi provider = CaptureApi::D3D9;
};

inline bool parseFrameHeaderBytes(const std::uint8_t bytes[16], CaptureApi expectedApi, FrameHeader& out) noexcept {
    if (!bytes) return false;
    if (std::memcmp(bytes, "MXGC\x01", 5) != 0) return false;
    if (bytes[5] != static_cast<std::uint8_t>(FrameStorage::Rgba8)) return false;
    if (bytes[6] != static_cast<std::uint8_t>(expectedApi)) return false;
    if (bytes[7] != 0) return false;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::memcpy(&width, bytes + 8, sizeof(width));
    std::memcpy(&height, bytes + 12, sizeof(height));
    if (width == 0 || width > maxFrameWidth || height == 0 || height > maxFrameHeight) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.storage = FrameStorage::Rgba8;
    out.provider = expectedApi;
    return true;
}

} // namespace muxivo::capture

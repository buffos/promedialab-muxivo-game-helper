#pragma once

#include "adapter_protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace muxivo::capture {

enum class ProcessArch : std::uint8_t {
    Unknown = 0,
    X86 = 1,
    X64 = 2,
};

enum class AttachStatus : std::uint8_t {
    Ok = 0,
    TargetUnavailable,
    AccessDenied,
    UnsupportedArch,
    AdapterMissing,
    ApiOffsetsUnavailable,
    HookFailed,
    AttachTimeout,
};

struct ProbeMatch {
    std::uint32_t pid = 0;
    ProcessArch arch = ProcessArch::Unknown;
};

const char* attachStatusCode(AttachStatus status) noexcept;
const char* processArchName(ProcessArch arch) noexcept;
const char* captureApiName(CaptureApi api) noexcept;

// Coordinates read-only target probing, per-API presence events, shared-memory
// target binding, on-demand x64/x86 hook injection, and clean detachment.
class AdapterController {
public:
    explicit AdapterController(std::wstring artifactDir = {});
    ~AdapterController();

    AdapterController(const AdapterController&) = delete;
    AdapterController& operator=(const AdapterController&) = delete;

    // Signals OS presence events for the requested API so attached adapters know the host is alive.
    bool ensureApiReady(CaptureApi api) noexcept;

    // Read-only module check across candidate PIDs. Never injects into non-selected processes.
    std::vector<ProbeMatch> probe(CaptureApi api, const std::vector<std::uint32_t>& pids) const;

    // Attaches the API adapter to the selected (pid, tid, hwnd) target and writes the frame pipe name.
    AttachStatus attach(
        CaptureApi api,
        std::uint32_t pid,
        std::uint32_t tid,
        std::uint64_t hwnd,
        std::wstring* outPipeName = nullptr);

    // Retires capture for (api, pid, hwnd) without killing the target process.
    void detach(CaptureApi api, std::uint32_t pid, std::uint64_t hwnd) noexcept;

    // Detaches all active sessions and releases controller handles/events.
    void shutdown() noexcept;

    static std::wstring framePipeName(CaptureApi api, std::uint32_t pid, std::uint64_t hwnd);
    static bool parseFrameHeader(const std::uint8_t bytes[16], CaptureApi expectedApi, FrameHeader& out) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace muxivo::capture

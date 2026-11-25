#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "d3d11_protocol.h"
#include "dxgi_offsets.h"

namespace muxivo {

enum class AttachStatus {
    Ok,
    TargetUnavailable,
    AccessDenied,
    UnsupportedArch,
    AdapterMissing,
    DxgiUnavailable,
    HookFailed,
    AttachTimeout,
};

const char* attachErrorCode(AttachStatus status) noexcept;
bool probeD3D11Process(DWORD pid) noexcept;

class D3D11HostManager {
public:
    D3D11HostManager() = default;
    ~D3D11HostManager();

    D3D11HostManager(const D3D11HostManager&) = delete;
    D3D11HostManager& operator=(const D3D11HostManager&) = delete;

    AttachStatus attach(DWORD pid, DWORD tid, HWND hwnd);
    void detach(DWORD pid, HWND hwnd) noexcept;
    void shutdown() noexcept;

private:
    struct Session {
        HANDLE process = nullptr;
        HANDLE mapping = nullptr;
        D3D11HookConfig* view = nullptr;
    };

    void pruneExitedSessions() noexcept;
    bool ensureAdapterDll() noexcept;
    bool ensureDxgiOffsets() noexcept;

    bool offsetsReady_ = false;
    DxgiOffsets offsets_ = {};
    std::wstring dllPath_;
    HMODULE dllModule_ = nullptr;
    HOOKPROC msgHookProc_ = nullptr;
    HOOKPROC wndHookProc_ = nullptr;
    std::unordered_map<DWORD, Session> sessions_;
};

} // namespace muxivo

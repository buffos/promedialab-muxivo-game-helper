#include "injector.h"

#include <tlhelp32.h>

#include <cwchar>
#include <filesystem>

#include "security_descriptor.h"

namespace muxivo {
namespace {

using PfnIsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

bool sameProcessMachineAsHost(HANDLE targetProcess, bool& matches) noexcept {
    static const auto isWow64Process2 = reinterpret_cast<PfnIsWow64Process2>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT hostMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT hostNative = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT targetNative = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(GetCurrentProcess(), &hostMachine, &hostNative)) return false;
        if (!isWow64Process2(targetProcess, &targetMachine, &targetNative)) return false;
        matches = (targetMachine == hostMachine) && (targetNative == hostNative);
        return true;
    }

    BOOL hostWow64 = FALSE;
    BOOL targetWow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &hostWow64) || !IsWow64Process(targetProcess, &targetWow64)) {
        return false;
    }
    matches = (hostWow64 == targetWow64);
    return true;
}

bool isProcessAlive(HANDLE process) noexcept {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

bool isLoadedEventSignaled(DWORD pid) noexcept {
    const std::wstring name = d3d11LoadedEventName(pid);
    const UniqueHandle loaded(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    return loaded && WaitForSingleObject(loaded.get(), 0) == WAIT_OBJECT_0;
}

std::wstring siblingDllPath() {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (!length || length >= ARRAYSIZE(buffer)) return {};
    std::filesystem::path hostPath(buffer, buffer + length);
    return (hostPath.parent_path() / L"muxivo-d3d11-game-capture.dll").wstring();
}

} // namespace

const char* attachErrorCode(AttachStatus status) noexcept {
    switch (status) {
        case AttachStatus::Ok: return "OK";
        case AttachStatus::TargetUnavailable: return "TARGET_UNAVAILABLE";
        case AttachStatus::AccessDenied: return "ACCESS_DENIED";
        case AttachStatus::UnsupportedArch: return "UNSUPPORTED_ARCH";
        case AttachStatus::AdapterMissing: return "ADAPTER_MISSING";
        case AttachStatus::DxgiUnavailable: return "DXGI_UNAVAILABLE";
        case AttachStatus::HookFailed: return "HOOK_FAILED";
        case AttachStatus::AttachTimeout: return "ATTACH_TIMEOUT";
    }
    return "HOOK_FAILED";
}

bool probeD3D11Process(DWORD pid) noexcept {
    if (pid == 0 || pid == GetCurrentProcessId()) return false;
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return false;

    bool archMatch = false;
    if (!sameProcessMachineAsHost(process.get(), archMatch) || !archMatch) return false;

    HANDLE rawSnap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5; ++attempt) {
        rawSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (rawSnap != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        Sleep(1);
    }
    if (rawSnap == INVALID_HANDLE_VALUE) return false;
    const UniqueHandle snap(rawSnap);

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snap.get(), &entry)) return false;

    bool hasD3D11 = false;
    bool hasDxgi = false;
    do {
        if (_wcsicmp(entry.szModule, L"d3d11.dll") == 0) hasD3D11 = true;
        else if (_wcsicmp(entry.szModule, L"dxgi.dll") == 0) hasDxgi = true;
        if (hasD3D11 && hasDxgi) return true;
    } while (Module32NextW(snap.get(), &entry));

    return false;
}

D3D11HostManager::~D3D11HostManager() {
    shutdown();
}

void D3D11HostManager::shutdown() noexcept {
    for (auto& [pid, session] : sessions_) {
        if (session.view) {
            writeConfigU32(session.view->active, 0);
            UnmapViewOfFile(session.view);
        }
        if (session.mapping) CloseHandle(session.mapping);
        if (session.process) CloseHandle(session.process);
    }
    sessions_.clear();
    if (dllModule_) {
        FreeLibrary(dllModule_);
        dllModule_ = nullptr;
        msgHookProc_ = nullptr;
        wndHookProc_ = nullptr;
    }
}

void D3D11HostManager::pruneExitedSessions() noexcept {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!isProcessAlive(it->second.process)) {
            if (it->second.view) UnmapViewOfFile(it->second.view);
            if (it->second.mapping) CloseHandle(it->second.mapping);
            if (it->second.process) CloseHandle(it->second.process);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

bool D3D11HostManager::ensureAdapterDll() noexcept {
    if (dllModule_ && msgHookProc_ && wndHookProc_) return true;
    dllPath_ = siblingDllPath();
    if (dllPath_.empty()) return false;
    const DWORD attrs = GetFileAttributesW(dllPath_.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    dllModule_ = LoadLibraryExW(dllPath_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dllModule_) return false;
    msgHookProc_ = reinterpret_cast<HOOKPROC>(GetProcAddress(dllModule_, "MuxivoCaptureMsgProc"));
    wndHookProc_ = reinterpret_cast<HOOKPROC>(GetProcAddress(dllModule_, "MuxivoCaptureWndProc"));
    if (!msgHookProc_ || !wndHookProc_) {
        FreeLibrary(dllModule_);
        dllModule_ = nullptr;
        msgHookProc_ = nullptr;
        wndHookProc_ = nullptr;
        return false;
    }
    return true;
}

bool D3D11HostManager::ensureDxgiOffsets() noexcept {
    if (offsetsReady_) return true;
    offsetsReady_ = probeDxgiOffsets(offsets_);
    return offsetsReady_;
}

AttachStatus D3D11HostManager::attach(DWORD pid, DWORD tid, HWND hwnd) {
    pruneExitedSessions();

    if (pid == 0 || tid == 0 || !hwnd || pid == GetCurrentProcessId()) {
        return AttachStatus::TargetUnavailable;
    }

    DWORD actualPid = 0;
    const DWORD actualTid = GetWindowThreadProcessId(hwnd, &actualPid);
    if (actualTid != tid || actualPid != pid) {
        return AttachStatus::TargetUnavailable;
    }

    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED
            ? AttachStatus::AccessDenied
            : AttachStatus::TargetUnavailable;
    }
    if (!isProcessAlive(process.get())) return AttachStatus::TargetUnavailable;

    bool archMatch = false;
    if (!sameProcessMachineAsHost(process.get(), archMatch)) return AttachStatus::TargetUnavailable;
    if (!archMatch) return AttachStatus::UnsupportedArch;

    if (!ensureAdapterDll()) return AttachStatus::AdapterMissing;
    if (!ensureDxgiOffsets()) return AttachStatus::DxgiUnavailable;

    Session* session = nullptr;
    bool createdSession = false;
    const auto existing = sessions_.find(pid);
    if (existing != sessions_.end()) {
        session = &existing->second;
        writeConfigU32(session->view->active, 0);
        writeConfigU32(session->view->hostPid, GetCurrentProcessId());
    } else {
        const auto security = tryCreateCurrentUserSecurityDescriptor();
        if (!security) return AttachStatus::HookFailed;
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        const std::wstring mapName = d3d11ConfigMappingName(pid);
        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(D3D11HookConfig), mapName.c_str());
        if (!mapping) return AttachStatus::HookFailed;
        auto* view = static_cast<D3D11HookConfig*>(
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(D3D11HookConfig)));
        if (!view) {
            CloseHandle(mapping);
            return AttachStatus::HookFailed;
        }
        Session fresh = {};
        fresh.process = process.release();
        fresh.mapping = mapping;
        fresh.view = view;
        if (fresh.view->magic != muxivoD3D11ConfigMagic
            || fresh.view->version != muxivoD3D11ConfigVersion
            || fresh.view->targetPid != pid) {
            *fresh.view = {};
            fresh.view->magic = muxivoD3D11ConfigMagic;
            fresh.view->version = muxivoD3D11ConfigVersion;
            fresh.view->targetPid = pid;
        }
        writeConfigU32(fresh.view->active, 0);
        writeConfigU32(fresh.view->hostPid, GetCurrentProcessId());
        writeConfigU32(fresh.view->presentRva, offsets_.presentRva);
        writeConfigU32(fresh.view->present1Rva, offsets_.present1Rva);
        writeConfigU32(fresh.view->resizeBuffersRva, offsets_.resizeBuffersRva);
        writeConfigU32(fresh.view->resizeBuffers1Rva, offsets_.resizeBuffers1Rva);
        writeConfigU32(fresh.view->secondaryPresentRva, offsets_.secondaryPresentRva);
        session = &sessions_.emplace(pid, fresh).first->second;
        createdSession = true;
    }

    const std::uint32_t nextGen = readConfigU32(session->view->generation) + 1;
    writeConfigU64(session->view->targetHwnd, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)));
    writeConfigU32(session->view->generation, nextGen);
    writeConfigU32(session->view->active, 1);

    if (isLoadedEventSignaled(pid)) {
        return AttachStatus::Ok;
    }

    auto rollbackNewSession = [&]() noexcept {
        writeConfigU32(session->view->active, 0);
        if (createdSession) {
            const auto it = sessions_.find(pid);
            if (it != sessions_.end()) {
                if (it->second.view) UnmapViewOfFile(it->second.view);
                if (it->second.mapping) CloseHandle(it->second.mapping);
                if (it->second.process) CloseHandle(it->second.process);
                sessions_.erase(it);
            }
        }
    };

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, msgHookProc_, dllModule_, tid);
    if (!hook) {
        const DWORD err = GetLastError();
        rollbackNewSession();
        return err == ERROR_ACCESS_DENIED ? AttachStatus::AccessDenied : AttachStatus::HookFailed;
    }

    bool loaded = false;
    for (int step = 0; step < 10; ++step) {
        PostThreadMessageW(tid, WM_NULL, 0, 0);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (isLoadedEventSignaled(pid)) {
            loaded = true;
            break;
        }
        if (!isProcessAlive(session->process)) break;
        Sleep(50);
    }
    UnhookWindowsHookEx(hook);

    if (loaded) return AttachStatus::Ok;
    if (!isProcessAlive(session->process)) {
        rollbackNewSession();
        return AttachStatus::TargetUnavailable;
    }

    // Some message loops drain sent messages differently than posted messages.
    HHOOK wndHook = SetWindowsHookExW(WH_CALLWNDPROC, wndHookProc_, dllModule_, tid);
    if (!wndHook) {
        const DWORD err = GetLastError();
        rollbackNewSession();
        return err == ERROR_ACCESS_DENIED ? AttachStatus::AccessDenied : AttachStatus::HookFailed;
    }
    DWORD_PTR sendResult = 0;
    SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 150, &sendResult);
    for (int step = 0; step < 2; ++step) {
        if (isLoadedEventSignaled(pid)) {
            loaded = true;
            break;
        }
        if (!isProcessAlive(session->process)) break;
        Sleep(50);
    }
    UnhookWindowsHookEx(wndHook);

    if (loaded) return AttachStatus::Ok;
    if (!isProcessAlive(session->process)) {
        rollbackNewSession();
        return AttachStatus::TargetUnavailable;
    }
    rollbackNewSession();
    return AttachStatus::AttachTimeout;
}

void D3D11HostManager::detach(DWORD pid, HWND hwnd) noexcept {
    pruneExitedSessions();
    const auto it = sessions_.find(pid);
    if (it == sessions_.end() || !it->second.view) return;
    const auto expected = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
    if (readConfigU64(it->second.view->targetHwnd) == expected) {
        writeConfigU32(it->second.view->active, 0);
    }
}

} // namespace muxivo

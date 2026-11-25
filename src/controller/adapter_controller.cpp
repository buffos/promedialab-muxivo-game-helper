#include "muxivo_capture/adapter_controller.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <tuple>
#include <vector>

#include "d3d12_offsets.h"
#include "d3d9_offsets.h"
#include "security_descriptor.h"

namespace muxivo::capture {
namespace {

using PfnIsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

bool queryProcessArch(HANDLE process, ProcessArch& out) noexcept {
    out = ProcessArch::Unknown;
    static const auto isWow64Process2 = reinterpret_cast<PfnIsWow64Process2>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine)) return false;
        if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
            if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) out = ProcessArch::X64;
            else if (nativeMachine == IMAGE_FILE_MACHINE_I386) out = ProcessArch::X86;
        } else if (processMachine == IMAGE_FILE_MACHINE_I386) {
            out = ProcessArch::X86;
        } else if (processMachine == IMAGE_FILE_MACHINE_AMD64) {
            out = ProcessArch::X64;
        }
        return out != ProcessArch::Unknown;
    }

    BOOL isWow64 = FALSE;
    if (!IsWow64Process(process, &isWow64)) return false;
    out = isWow64 ? ProcessArch::X86 : ProcessArch::X64;
    return true;
}

bool isProcessAlive(HANDLE process) noexcept {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

bool isLoadedEventSignaled(CaptureApi api, DWORD pid) noexcept {
    const std::wstring name = apiLoadedEventName(api, pid);
    const UniqueHandle loaded(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    return loaded && WaitForSingleObject(loaded.get(), 0) == WAIT_OBJECT_0;
}

std::wstring defaultArtifactDir() {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (!length || length >= ARRAYSIZE(buffer)) return {};
    return std::filesystem::path(buffer, buffer + length).parent_path().wstring();
}

std::wstring normalizeArtifactDir(std::wstring dir) {
    if (dir.empty()) return defaultArtifactDir();
    std::error_code ec;
    const auto abs = std::filesystem::absolute(dir, ec);
    return (!ec && !abs.empty()) ? abs.wstring() : dir;
}

bool probeProcessModules(CaptureApi api, DWORD pid, ProcessArch& arch) noexcept {
    arch = ProcessArch::Unknown;
    if (pid == 0 || pid == GetCurrentProcessId()) return false;
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process || !queryProcessArch(process.get(), arch)) return false;

    if (api == CaptureApi::D3D12 && arch != ProcessArch::X64) return false;
    if (arch != ProcessArch::X64 && arch != ProcessArch::X86) return false;

    HANDLE rawSnap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5; ++attempt) {
        // TH32CS_SNAPMODULE32 is mandatory from an x64 host to inspect 32-bit WOW64 targets.
        rawSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (rawSnap != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        Sleep(1);
    }
    if (rawSnap == INVALID_HANDLE_VALUE) return false;
    const UniqueHandle snap(rawSnap);

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snap.get(), &entry)) return false;

    bool hasD3D9 = false;
    bool hasD3D12 = false;
    bool hasDxgi = false;
    bool hasOpenGl = false;
    do {
        if (_wcsicmp(entry.szModule, L"d3d9.dll") == 0) hasD3D9 = true;
        else if (_wcsicmp(entry.szModule, L"d3d12.dll") == 0) hasD3D12 = true;
        else if (_wcsicmp(entry.szModule, L"dxgi.dll") == 0) hasDxgi = true;
        else if (_wcsicmp(entry.szModule, L"opengl32.dll") == 0) hasOpenGl = true;

        switch (api) {
            case CaptureApi::D3D9:
                if (hasD3D9) return true;
                break;
            case CaptureApi::D3D12:
                if (hasD3D12 && hasDxgi) return true;
                break;
            case CaptureApi::OpenGL:
                if (hasOpenGl) return true;
                break;
        }
    } while (Module32NextW(snap.get(), &entry));

    return false;
}

const wchar_t* hookDllName(CaptureApi api, ProcessArch arch) noexcept {
    if (arch == ProcessArch::X64) {
        switch (api) {
            case CaptureApi::D3D9: return L"muxivo-d3d9-game-capture-x64.dll";
            case CaptureApi::D3D12: return L"muxivo-d3d12-game-capture-x64.dll";
            case CaptureApi::OpenGL: return L"muxivo-opengl-game-capture-x64.dll";
        }
    } else if (arch == ProcessArch::X86) {
        switch (api) {
            case CaptureApi::D3D9: return L"muxivo-d3d9-game-capture-x86.dll";
            case CaptureApi::D3D12: return nullptr;
            case CaptureApi::OpenGL: return L"muxivo-opengl-game-capture-x86.dll";
        }
    }
    return nullptr;
}

const wchar_t* cliApiToken(CaptureApi api) noexcept {
    switch (api) {
        case CaptureApi::D3D9: return L"d3d9";
        case CaptureApi::D3D12: return L"d3d12";
        case CaptureApi::OpenGL: return L"opengl";
    }
    return L"unknown";
}

} // namespace

struct AdapterController::Impl {
    struct LoadedDll {
        HMODULE module = nullptr;
        HOOKPROC msgProc = nullptr;
        HOOKPROC wndProc = nullptr;
    };

    struct Session {
        HANDLE process = nullptr;
        HANDLE mapping = nullptr;
        AdapterHookConfig* view = nullptr;
    };

    std::mutex mutex;
    std::wstring artifactDir;
    std::map<CaptureApi, UniqueHandle> presenceEvents;
    std::map<std::pair<CaptureApi, std::uint32_t>, Session> sessions;
    std::map<CaptureApi, LoadedDll> x64Dlls;

    bool d3d9OffsetsReady = false;
    d3d9::D3D9Offsets d3d9Offsets{};
    bool d3d12OffsetsReady = false;
    d3d12::DxgiSwapOffsets d3d12Offsets{};

    explicit Impl(std::wstring dir)
        : artifactDir(normalizeArtifactDir(std::move(dir))) {}

    ~Impl() { shutdown(); }

    void pruneExited() noexcept {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!isProcessAlive(it->second.process)) {
                if (it->second.view) UnmapViewOfFile(it->second.view);
                if (it->second.mapping) CloseHandle(it->second.mapping);
                if (it->second.process) CloseHandle(it->second.process);
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool ensurePresenceLocked(CaptureApi api) noexcept {
        if (presenceEvents.count(api)) return true;
        const auto security = tryCreateCurrentUserSecurityDescriptor();
        if (!security) return false;
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        const std::wstring name = apiPresenceEventName(api);
        HANDLE ev = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
        if (!ev) return false;
        presenceEvents.emplace(api, UniqueHandle(ev));
        return true;
    }

    bool ensureX64DllLocked(CaptureApi api, LoadedDll& out) noexcept {
        auto it = x64Dlls.find(api);
        if (it != x64Dlls.end() && it->second.module) {
            out = it->second;
            return true;
        }
        const wchar_t* name = hookDllName(api, ProcessArch::X64);
        if (!name || artifactDir.empty()) return false;
        const std::wstring fullPath = (std::filesystem::path(artifactDir) / name).wstring();
        const DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
        HMODULE mod = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) return false;
        auto msgProc = reinterpret_cast<HOOKPROC>(GetProcAddress(mod, "MuxivoCaptureMsgProc"));
        auto wndProc = reinterpret_cast<HOOKPROC>(GetProcAddress(mod, "MuxivoCaptureWndProc"));
        if (!msgProc || !wndProc) {
            FreeLibrary(mod);
            return false;
        }
        out = { mod, msgProc, wndProc };
        x64Dlls[api] = out;
        return true;
    }

    AttachStatus ensureX64OffsetsLocked(CaptureApi api, AdapterHookConfig& cfg) noexcept {
        if (api == CaptureApi::D3D9) {
            if (!d3d9OffsetsReady) d3d9OffsetsReady = d3d9::probeD3D9Offsets(d3d9Offsets);
            if (!d3d9OffsetsReady) return AttachStatus::ApiOffsetsUnavailable;
            writeConfigU32(cfg.rva[0], d3d9Offsets.deviceResetRva);
            writeConfigU32(cfg.rva[1], d3d9Offsets.devicePresentRva);
            writeConfigU32(cfg.rva[2], d3d9Offsets.devicePresentExRva);
            writeConfigU32(cfg.rva[3], d3d9Offsets.deviceResetExRva);
            writeConfigU32(cfg.rva[4], d3d9Offsets.swapChainPresentRva);
            writeConfigU32(cfg.rva[5], 0);
            return AttachStatus::Ok;
        }
        if (api == CaptureApi::D3D12) {
            if (!d3d12OffsetsReady) d3d12OffsetsReady = d3d12::probeDxgiSwapOffsets(d3d12Offsets);
            if (!d3d12OffsetsReady) return AttachStatus::ApiOffsetsUnavailable;
            writeConfigU32(cfg.rva[0], d3d12Offsets.presentRva);
            writeConfigU32(cfg.rva[1], d3d12Offsets.resizeBuffersRva);
            writeConfigU32(cfg.rva[2], d3d12Offsets.present1Rva);
            writeConfigU32(cfg.rva[3], d3d12Offsets.resizeBuffers1Rva);
            writeConfigU32(cfg.rva[4], d3d12Offsets.setColorSpace1Rva);
            writeConfigU32(cfg.rva[5], d3d12Offsets.secondaryPresentRva);
            return AttachStatus::Ok;
        }
        for (std::size_t i = 0; i < 6; ++i) writeConfigU32(cfg.rva[i], 0);
        return AttachStatus::Ok;
    }

    AttachStatus injectX64(
        CaptureApi api, std::uint32_t pid, std::uint32_t tid, HWND hwnd, HANDLE process, const LoadedDll& dll) noexcept {
        HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, dll.msgProc, dll.module, tid);
        if (!hook) {
            return GetLastError() == ERROR_ACCESS_DENIED ? AttachStatus::AccessDenied : AttachStatus::HookFailed;
        }
        bool loaded = false;
        for (int step = 0; step < 12; ++step) {
            PostThreadMessageW(tid, WM_NULL, 0, 0);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (isLoadedEventSignaled(api, pid)) {
                loaded = true;
                break;
            }
            if (!isProcessAlive(process)) break;
            Sleep(50);
        }
        UnhookWindowsHookEx(hook);
        if (loaded) return AttachStatus::Ok;
        if (!isProcessAlive(process)) return AttachStatus::TargetUnavailable;

        HHOOK wndHook = SetWindowsHookExW(WH_CALLWNDPROC, dll.wndProc, dll.module, tid);
        if (!wndHook) {
            return GetLastError() == ERROR_ACCESS_DENIED ? AttachStatus::AccessDenied : AttachStatus::HookFailed;
        }
        DWORD_PTR sendResult = 0;
        SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 150, &sendResult);
        for (int step = 0; step < 4; ++step) {
            if (isLoadedEventSignaled(api, pid)) {
                loaded = true;
                break;
            }
            if (!isProcessAlive(process)) break;
            Sleep(50);
        }
        UnhookWindowsHookEx(wndHook);
        if (loaded) return AttachStatus::Ok;
        if (!isProcessAlive(process)) return AttachStatus::TargetUnavailable;
        return AttachStatus::AttachTimeout;
    }

    AttachStatus injectX86ViaHelper(
        CaptureApi api, std::uint32_t pid, std::uint32_t tid, std::uint64_t hwnd) const noexcept {
        if (artifactDir.empty()) return AttachStatus::AdapterMissing;
        const std::filesystem::path base(artifactDir);
        const std::wstring attachExe = (base / L"muxivo-capture-attach-x86.exe").wstring();
        const wchar_t* dllName = hookDllName(api, ProcessArch::X86);
        if (!dllName) return AttachStatus::UnsupportedArch;
        const std::wstring dllPath = (base / dllName).wstring();

        if (GetFileAttributesW(attachExe.c_str()) == INVALID_FILE_ATTRIBUTES
            || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return AttachStatus::AdapterMissing;
        }

        const std::wstring cmdBase = L"\"" + attachExe + L"\" " + cliApiToken(api) + L" "
            + std::to_wstring(pid) + L" " + std::to_wstring(tid) + L" ";
        wchar_t hwndHex[32] = {};
        swprintf_s(hwndHex, L"%llx", static_cast<unsigned long long>(hwnd));
        std::wstring cmd = cmdBase + hwndHex;

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(attachExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, artifactDir.c_str(), &si, &pi)) {
            return AttachStatus::HookFailed;
        }
        const UniqueHandle proc(pi.hProcess);
        const UniqueHandle th(pi.hThread);
        if (WaitForSingleObject(proc.get(), 3500) == WAIT_TIMEOUT) {
            TerminateProcess(proc.get(), 1);
            return AttachStatus::AttachTimeout;
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(proc.get(), &exitCode);
        switch (exitCode) {
            case 0: return AttachStatus::Ok;
            case 10: return AttachStatus::TargetUnavailable;
            case 11: return AttachStatus::AccessDenied;
            case 12: return AttachStatus::AdapterMissing;
            case 13: return AttachStatus::ApiOffsetsUnavailable;
            case 14: return AttachStatus::AttachTimeout;
            default: return AttachStatus::HookFailed;
        }
    }

    void shutdown() noexcept {
        std::lock_guard lock(mutex);
        for (auto& [key, session] : sessions) {
            if (session.view) {
                writeConfigU32(session.view->active, 0);
                UnmapViewOfFile(session.view);
            }
            if (session.mapping) CloseHandle(session.mapping);
            if (session.process) CloseHandle(session.process);
        }
        sessions.clear();
        for (auto& [api, dll] : x64Dlls) {
            if (dll.module) FreeLibrary(dll.module);
        }
        x64Dlls.clear();
        presenceEvents.clear();
    }
};

const char* attachStatusCode(AttachStatus status) noexcept {
    switch (status) {
        case AttachStatus::Ok: return "OK";
        case AttachStatus::TargetUnavailable: return "TARGET_UNAVAILABLE";
        case AttachStatus::AccessDenied: return "ACCESS_DENIED";
        case AttachStatus::UnsupportedArch: return "UNSUPPORTED_ARCH";
        case AttachStatus::AdapterMissing: return "ADAPTER_MISSING";
        case AttachStatus::ApiOffsetsUnavailable: return "API_OFFSETS_UNAVAILABLE";
        case AttachStatus::HookFailed: return "HOOK_FAILED";
        case AttachStatus::AttachTimeout: return "ATTACH_TIMEOUT";
    }
    return "HOOK_FAILED";
}

const char* processArchName(ProcessArch arch) noexcept {
    switch (arch) {
        case ProcessArch::X86: return "x86";
        case ProcessArch::X64: return "x64";
        case ProcessArch::Unknown: return "unknown";
    }
    return "unknown";
}

const char* captureApiName(CaptureApi api) noexcept {
    switch (api) {
        case CaptureApi::D3D9: return "d3d9";
        case CaptureApi::D3D12: return "d3d12";
        case CaptureApi::OpenGL: return "opengl";
    }
    return "unknown";
}

AdapterController::AdapterController(std::wstring artifactDir)
    : impl_(std::make_unique<Impl>(std::move(artifactDir))) {}

AdapterController::~AdapterController() = default;

bool AdapterController::ensureApiReady(CaptureApi api) noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->ensurePresenceLocked(api);
}

std::vector<ProbeMatch> AdapterController::probe(
    CaptureApi api, const std::vector<std::uint32_t>& pids) const {
    std::vector<ProbeMatch> matches;
    for (const std::uint32_t pid : pids) {
        ProcessArch arch = ProcessArch::Unknown;
        if (probeProcessModules(api, pid, arch)) {
            matches.push_back({ pid, arch });
        }
    }
    return matches;
}

AttachStatus AdapterController::attach(
    CaptureApi api,
    std::uint32_t pid,
    std::uint32_t tid,
    std::uint64_t hwndValue,
    std::wstring* outPipeName) {
    if (outPipeName) outPipeName->clear();
    if (pid == 0 || tid == 0 || hwndValue == 0 || pid == GetCurrentProcessId()) {
        return AttachStatus::TargetUnavailable;
    }

    const HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwndValue));
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

    ProcessArch arch = ProcessArch::Unknown;
    if (!queryProcessArch(process.get(), arch)) return AttachStatus::TargetUnavailable;
    if (api == CaptureApi::D3D12 && arch != ProcessArch::X64) return AttachStatus::UnsupportedArch;
    if (arch != ProcessArch::X64 && arch != ProcessArch::X86) return AttachStatus::UnsupportedArch;

    std::lock_guard lock(impl_->mutex);
    impl_->pruneExited();
    if (!impl_->ensurePresenceLocked(api)) return AttachStatus::HookFailed;

    Impl::LoadedDll x64Dll = {};
    if (arch == ProcessArch::X64 && !impl_->ensureX64DllLocked(api, x64Dll)) {
        return AttachStatus::AdapterMissing;
    }

    const auto sessionKey = std::make_pair(api, pid);
    Impl::Session* session = nullptr;
    bool createdSession = false;
    if (const auto it = impl_->sessions.find(sessionKey); it != impl_->sessions.end()) {
        session = &it->second;
    } else {
        const auto security = tryCreateCurrentUserSecurityDescriptor();
        if (!security) return AttachStatus::HookFailed;
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        const std::wstring mapName = apiConfigMappingName(api, pid);
        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(AdapterHookConfig), mapName.c_str());
        if (!mapping) return AttachStatus::HookFailed;
        auto* view = static_cast<AdapterHookConfig*>(
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AdapterHookConfig)));
        if (!view) {
            CloseHandle(mapping);
            return AttachStatus::HookFailed;
        }
        Impl::Session fresh = {};
        fresh.process = process.release();
        fresh.mapping = mapping;
        fresh.view = view;
        if (view->magic != adapterHookConfigMagic
            || view->version != adapterHookConfigVersion
            || view->api != static_cast<std::uint32_t>(api)
            || view->targetPid != pid) {
            *view = {};
            view->magic = adapterHookConfigMagic;
            view->version = adapterHookConfigVersion;
            view->api = static_cast<std::uint32_t>(api);
            view->targetPid = pid;
        }
        view->hostPid = GetCurrentProcessId();
        session = &impl_->sessions.emplace(sessionKey, fresh).first->second;
        createdSession = true;
    }

    writeConfigU32(session->view->active, 0);
    if (arch == ProcessArch::X64) {
        const AttachStatus offsetStatus = impl_->ensureX64OffsetsLocked(api, *session->view);
        if (offsetStatus != AttachStatus::Ok) {
            if (createdSession) {
                UnmapViewOfFile(session->view);
                CloseHandle(session->mapping);
                CloseHandle(session->process);
                impl_->sessions.erase(sessionKey);
            }
            return offsetStatus;
        }
    }

    const std::uint32_t nextGen = readConfigU32(session->view->generation) + 1;
    writeConfigHwnd(*session->view, hwndValue);
    writeConfigU32(session->view->generation, nextGen);
    writeConfigU32(session->view->active, 1);

    AttachStatus status = AttachStatus::Ok;
    if (!isLoadedEventSignaled(api, pid)) {
        if (arch == ProcessArch::X64) {
            status = impl_->injectX64(api, pid, tid, hwnd, session->process, x64Dll);
        } else {
            status = impl_->injectX86ViaHelper(api, pid, tid, hwndValue);
        }
    }

    if (status != AttachStatus::Ok) {
        writeConfigU32(session->view->active, 0);
        if (createdSession) {
            if (auto it = impl_->sessions.find(sessionKey); it != impl_->sessions.end()) {
                UnmapViewOfFile(it->second.view);
                CloseHandle(it->second.mapping);
                CloseHandle(it->second.process);
                impl_->sessions.erase(it);
            }
        }
        return status;
    }

    if (outPipeName) {
        *outPipeName = makeFramePipeName(api, pid, hwndValue);
    }
    return AttachStatus::Ok;
}

void AdapterController::detach(CaptureApi api, std::uint32_t pid, std::uint64_t hwnd) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->pruneExited();
    const auto it = impl_->sessions.find(std::make_pair(api, pid));
    if (it == impl_->sessions.end() || !it->second.view) return;
    if (readConfigHwnd(*it->second.view) == hwnd) {
        writeConfigU32(it->second.view->active, 0);
    }
}

void AdapterController::shutdown() noexcept {
    impl_->shutdown();
}

std::wstring AdapterController::framePipeName(CaptureApi api, std::uint32_t pid, std::uint64_t hwnd) {
    return makeFramePipeName(api, pid, hwnd);
}

bool AdapterController::parseFrameHeader(
    const std::uint8_t bytes[16], CaptureApi expectedApi, FrameHeader& out) noexcept {
    return parseFrameHeaderBytes(bytes, expectedApi, out);
}

} // namespace muxivo::capture

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>

#include "d3d9_offsets.h"
#include "module_image.h"
#include "muxivo_capture/adapter_protocol.h"
#include "parse_int.h"
#include "security_descriptor.h"

namespace {

using muxivo::UniqueHandle;
using namespace muxivo::capture;

bool isProcessAlive(HANDLE process) noexcept {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

bool isLoadedEventSignaled(CaptureApi api, DWORD pid) noexcept {
    const std::wstring name = apiLoadedEventName(api, pid);
    const UniqueHandle loaded(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    return loaded && WaitForSingleObject(loaded.get(), 0) == WAIT_OBJECT_0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 5) return 10;

    CaptureApi api = CaptureApi::D3D9;
    std::wstring dllName;
    if (_wcsicmp(argv[1], L"d3d9") == 0) {
        api = CaptureApi::D3D9;
        dllName = L"muxivo-d3d9-game-capture-x86.dll";
    } else if (_wcsicmp(argv[1], L"opengl") == 0) {
        api = CaptureApi::OpenGL;
        dllName = L"muxivo-opengl-game-capture-x86.dll";
    } else {
        return 10;
    }

    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    std::uint64_t hwndVal = 0;
    if (!parseWideInt(argv[2], 10, pid) || !parseWideInt(argv[3], 10, tid) || !parseWideInt(argv[4], 16, hwndVal)) {
        return 10;
    }

    const HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwndVal));
    DWORD actualPid = 0;
    if (GetWindowThreadProcessId(hwnd, &actualPid) != tid || actualPid != pid) {
        return 10;
    }

    const UniqueHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED ? 11 : 10;
    }

    if (isLoadedEventSignaled(api, pid)) {
        const std::wstring mapName = apiConfigMappingName(api, pid);
        const UniqueHandle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str()));
        if (!mapping) return 15;
        return 0;
    }

    const std::wstring mapName = apiConfigMappingName(api, pid);
    const UniqueHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str()));
    if (!mapping) return 15;

    auto* view = static_cast<AdapterHookConfig*>(
        MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AdapterHookConfig)));
    if (!view) return 15;

    if (view->magic != adapterHookConfigMagic
        || view->version != adapterHookConfigVersion
        || view->api != static_cast<std::uint32_t>(api)
        || view->targetPid != pid) {
        UnmapViewOfFile(view);
        return 15;
    }

    if (api == CaptureApi::D3D9) {
        d3d9::D3D9Offsets offsets = {};
        if (!d3d9::probeD3D9Offsets(offsets)) {
            UnmapViewOfFile(view);
            return 13;
        }
        writeConfigU32(view->rva[0], offsets.deviceResetRva);
        writeConfigU32(view->rva[1], offsets.devicePresentRva);
        writeConfigU32(view->rva[2], offsets.devicePresentExRva);
        writeConfigU32(view->rva[3], offsets.deviceResetExRva);
        writeConfigU32(view->rva[4], offsets.swapChainPresentRva);
        writeConfigU32(view->rva[5], 0);
    }
    UnmapViewOfFile(view);

    wchar_t exePath[MAX_PATH * 4] = {};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    if (!exeLen || exeLen >= ARRAYSIZE(exePath)) return 12;
    const std::wstring dllPath = (std::filesystem::path(exePath, exePath + exeLen).parent_path() / dllName).wstring();

    const muxivo::capture::ModuleGuard mod(
        LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!mod) return 12;
    const auto msgProc = reinterpret_cast<HOOKPROC>(GetProcAddress(mod.get(), "MuxivoCaptureMsgProc"));
    const auto wndProc = reinterpret_cast<HOOKPROC>(GetProcAddress(mod.get(), "MuxivoCaptureWndProc"));
    if (!msgProc || !wndProc) {
        return 12;
    }

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, msgProc, mod.get(), tid);
    if (!hook) {
        const DWORD err = GetLastError();
        return err == ERROR_ACCESS_DENIED ? 11 : 15;
    }

    bool loaded = false;
    for (int step = 0; step < 12; ++step) {
        PostThreadMessageW(tid, WM_NULL, 0, 0);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (isLoadedEventSignaled(api, pid)) {
            loaded = true;
            break;
        }
        if (!isProcessAlive(process.get())) break;
        Sleep(50);
    }
    UnhookWindowsHookEx(hook);

    DWORD fallbackError = ERROR_SUCCESS;
    if (!loaded && isProcessAlive(process.get())) {
        HHOOK wndHook = SetWindowsHookExW(WH_CALLWNDPROC, wndProc, mod.get(), tid);
        if (!wndHook) {
            fallbackError = GetLastError();
        } else {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 150, &res);
            for (int step = 0; step < 4; ++step) {
                if (isLoadedEventSignaled(api, pid)) {
                    loaded = true;
                    break;
                }
                if (!isProcessAlive(process.get())) break;
                Sleep(50);
            }
            UnhookWindowsHookEx(wndHook);
        }
    }

    if (loaded) return 0;
    if (!isProcessAlive(process.get())) return 10;
    if (fallbackError == ERROR_ACCESS_DENIED) return 11;
    return 14;
}

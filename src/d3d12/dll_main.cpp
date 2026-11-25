#include <windows.h>

#include "adapter_dll_bootstrap.h"
#include "d3d12_hook.h"

namespace {

using Bootstrap = muxivo::capture::AdapterDllBootstrap<
    &muxivo::capture::d3d12::hooksReady,
    &muxivo::capture::d3d12::bootstrapHooks>;

} // namespace

extern "C" LRESULT CALLBACK MuxivoCaptureMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) Bootstrap::triggerOnce();
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" LRESULT CALLBACK MuxivoCaptureWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) Bootstrap::triggerOnce();
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

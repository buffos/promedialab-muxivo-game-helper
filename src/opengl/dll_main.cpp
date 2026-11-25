#include <windows.h>

#include "adapter_dll_bootstrap.h"
#include "opengl_hook.h"

namespace {

using Bootstrap = muxivo::capture::AdapterDllBootstrap<
    &muxivo::capture::opengl::hooksReady,
    &muxivo::capture::opengl::bootstrapHooks>;

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

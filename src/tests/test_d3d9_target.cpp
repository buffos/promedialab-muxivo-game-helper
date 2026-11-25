#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;
using Color = std::array<std::uint8_t, 4>;

void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

void succeeded(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed 0x%08lx\n", msg, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

D3DPRESENT_PARAMETERS makeParams(HWND hwnd, UINT w, UINT h) noexcept {
    D3DPRESENT_PARAMETERS params = {};
    params.Windowed = TRUE;
    params.BackBufferWidth = w;
    params.BackBufferHeight = h;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = hwnd;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return params;
}

} // namespace

int main() {
    try {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        const WNDCLASSW wc{ CS_OWNDC, DefWindowProcW, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"MuxivoD3D9Sim" };
        RegisterClassW(&wc);
        const HWND hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE, L"MuxivoD3D9Sim", L"Muxivo D3D9 Sim",
            WS_POPUP | WS_VISIBLE, 16, 16, 160, 120, nullptr, nullptr, inst, nullptr);
        check(hwnd != nullptr, "Create D3D9 sim window");

        ComPtr<IDirect3D9Ex> d3d;
        succeeded(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d), "Direct3DCreate9Ex");
        D3DPRESENT_PARAMETERS params = makeParams(hwnd, 160, 120);
        ComPtr<IDirect3DDevice9Ex> dev;
        HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &params, nullptr, &dev);
        if (FAILED(hr)) {
            succeeded(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &params, nullptr, &dev),
                "CreateDeviceEx fallback");
        }

        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        Color color{ 17, 73, 191, 255 };
        bool readySent = false;

        for (int frame = 0;; ++frame) {
            MSG msg = {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return 0;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetFileType(input) == FILE_TYPE_PIPE) {
                DWORD queued = 0;
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, &queued, nullptr)) break;
                if (queued > 0) {
                    char cmd = 0;
                    DWORD read = 0;
                    if (!ReadFile(input, &cmd, 1, &read, nullptr) || read != 1 || cmd == 'q') break;
                    if (cmd == 'n') {
                        color = { 211, 47, 89, 255 };
                    } else if (cmd == 'r') {
                        params = makeParams(hwnd, 200, 150);
                        succeeded(dev->Reset(&params), "Device Reset");
                        SetWindowPos(hwnd, nullptr, 16, 16, 200, 150, SWP_NOACTIVATE | SWP_NOZORDER);
                        color = { 41, 173, 99, 255 };
                    }
                }
            }

            succeeded(dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                D3DCOLOR_ARGB(255, color[0], color[1], color[2]), 1.0f, 0), "Clear");
            dev->Present(nullptr, nullptr, nullptr, nullptr);

            if (!readySent && frame >= 3) {
                readySent = true;
                std::printf("READY %lu %lu %llx\n",
                    GetCurrentProcessId(), GetCurrentThreadId(),
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
                std::fflush(stdout);
            }
            Sleep(15);
        }
        DestroyWindow(hwnd);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

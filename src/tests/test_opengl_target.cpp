#include <windows.h>
#include <GL/gl.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using Color = std::array<std::uint8_t, 4>;

void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        bool useCompat = (argc == 2 && std::string_view(argv[1]) == "--compat");
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        const WNDCLASSW wc{ CS_OWNDC, DefWindowProcW, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"MuxivoGLSim" };
        RegisterClassW(&wc);
        const HWND hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE, L"MuxivoGLSim", L"Muxivo OpenGL Sim",
            WS_POPUP | WS_VISIBLE, 16, 16, 160, 120, nullptr, nullptr, inst, nullptr);
        check(hwnd != nullptr, "Create OpenGL sim window");

        const HDC dc = GetDC(hwnd);
        check(dc != nullptr, "GetDC");
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        const int pf = ChoosePixelFormat(dc, &pfd);
        check(pf && SetPixelFormat(dc, pf, &pfd), "SetPixelFormat");

        HGLRC boot = wglCreateContext(dc);
        check(boot && wglMakeCurrent(dc, boot), "wglCreateContext");
        using PfnCreateAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
        const auto createAttribs = reinterpret_cast<PfnCreateAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        HGLRC glrc = boot;
        if (createAttribs) {
            const int attrs[] = { 0x2091, 3, 0x2092, 2, 0x9126, useCompat ? 2 : 1, 0 };
            HGLRC modern = createAttribs(dc, nullptr, attrs);
            if (modern) {
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(boot);
                glrc = modern;
                check(wglMakeCurrent(dc, glrc) != FALSE, "wglMakeCurrent modern");
            }
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
                        SetWindowPos(hwnd, nullptr, 16, 16, 200, 150, SWP_NOACTIVATE | SWP_NOZORDER);
                        glViewport(0, 0, 200, 150);
                        color = { 41, 173, 99, 255 };
                    }
                }
            }

            glPixelStorei(GL_PACK_ALIGNMENT, 2);
            glPixelStorei(GL_PACK_ROW_LENGTH, 7);
            glReadBuffer(GL_FRONT);
            check(glGetError() == GL_NO_ERROR, "Pre-swap GL state");

            glDisable(GL_DITHER);
            glClearColor(color[0] / 255.f, color[1] / 255.f, color[2] / 255.f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            SwapBuffers(dc);

            check(glGetError() == GL_NO_ERROR, "Post-swap glGetError");
            GLint align = 0;
            GLint rowLen = 0;
            GLint readBuf = 0;
            glGetIntegerv(GL_PACK_ALIGNMENT, &align);
            glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
            glGetIntegerv(GL_READ_BUFFER, &readBuf);
            check(align == 2 && rowLen == 7 && readBuf == GL_FRONT, "GL state preserved across capture");

            if (!readySent && frame >= 3) {
                readySent = true;
                std::printf("READY %lu %lu %llx\n",
                    GetCurrentProcessId(), GetCurrentThreadId(),
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
                std::fflush(stdout);
            }
            Sleep(15);
        }

        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(glrc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

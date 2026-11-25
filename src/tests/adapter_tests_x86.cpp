#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "d3d9_offsets.h"
#include "muxivo_capture/adapter_protocol.h"
#include "security_descriptor.h"

namespace {

using muxivo::UniqueHandle;
using namespace muxivo::capture;

void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

std::string envRequired(const char* name) {
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || !raw || !*raw) {
        std::free(raw);
        throw std::runtime_error(std::string("Missing env var: ") + name);
    }
    std::string val(raw);
    std::free(raw);
    return val;
}

struct SpawnedTarget {
    UniqueHandle process;
    UniqueHandle stdinWrite;
    UniqueHandle stdoutRead;
    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    std::uint64_t hwnd = 0;

    SpawnedTarget() = default;
    SpawnedTarget(SpawnedTarget&&) = default;
    SpawnedTarget& operator=(SpawnedTarget&&) = default;

    ~SpawnedTarget() {
        if (stdinWrite) {
            DWORD written = 0;
            const char q = 'q';
            WriteFile(stdinWrite.get(), &q, 1, &written, nullptr);
            stdinWrite.reset();
        }
        if (process && WaitForSingleObject(process.get(), 2000) == WAIT_TIMEOUT) {
            TerminateProcess(process.get(), 1);
        }
    }

    void sendCmd(char c) const {
        DWORD written = 0;
        check(WriteFile(stdinWrite.get(), &c, 1, &written, nullptr) && written == 1, "Send target cmd");
    }
};

SpawnedTarget spawnAndWaitReady(const std::string& exePath) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    check(CreatePipe(&inR, &inW, &sa, 0) && CreatePipe(&outR, &outW, &sa, 0), "Create pipes");
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + exePath + "\"";
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    check(ok != FALSE, "Launch x86 test target");
    CloseHandle(pi.hThread);

    SpawnedTarget t;
    t.process.reset(pi.hProcess);
    t.stdinWrite.reset(inW);
    t.stdoutRead.reset(outR);

    std::string line;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        DWORD avail = 0;
        if (PeekNamedPipe(t.stdoutRead.get(), nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char ch = 0;
            DWORD rd = 0;
            if (ReadFile(t.stdoutRead.get(), &ch, 1, &rd, nullptr) && rd == 1) {
                if (ch == '\n') break;
                if (ch != '\r') line.push_back(ch);
            }
        } else {
            Sleep(10);
        }
    }
    std::uint32_t pid = 0, tid = 0;
    unsigned long long hwnd = 0;
    check(sscanf_s(line.c_str(), "READY %u %u %llx", &pid, &tid, &hwnd) == 3, "Parse READY");
    t.pid = pid;
    t.tid = tid;
    t.hwnd = static_cast<std::uint64_t>(hwnd);
    return t;
}

void runAttachHelper(
    const std::string& attachExe, const char* apiToken, std::uint32_t pid, std::uint32_t tid, std::uint64_t hwnd) {
    char cmd[512] = {};
    std::snprintf(cmd, sizeof(cmd), "\"%s\" %s %u %u %llx",
        attachExe.c_str(), apiToken, pid, tid, static_cast<unsigned long long>(hwnd));
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    check(CreateProcessA(attachExe.c_str(), cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE,
        "Launch muxivo-capture-attach-x86");
    CloseHandle(pi.hThread);
    const UniqueHandle proc(pi.hProcess);
    check(WaitForSingleObject(proc.get(), 5000) == WAIT_OBJECT_0, "Attach helper timeout");
    DWORD code = 1;
    GetExitCodeProcess(proc.get(), &code);
    check(code == 0, "Attach helper returned non-zero");
}

void verifyX86Api(
    CaptureApi api, const char* apiToken, const std::string& targetExe, const std::string& attachExe) {
    const auto security = muxivo::createCurrentUserSecurityDescriptor();
    SECURITY_ATTRIBUTES attrs = { sizeof(attrs), security.get(), FALSE };
    const UniqueHandle presence(CreateEventW(&attrs, TRUE, FALSE, apiPresenceEventName(api).c_str()));
    check(presence.get() != nullptr, "Create x86 test presence event");

    auto target = spawnAndWaitReady(targetExe);
    const UniqueHandle mapping(CreateFileMappingW(
        INVALID_HANDLE_VALUE, &attrs, PAGE_READWRITE, 0, sizeof(AdapterHookConfig),
        apiConfigMappingName(api, target.pid).c_str()));
    check(mapping.get() != nullptr, "Create x86 config mapping");
    auto* cfg = static_cast<AdapterHookConfig*>(
        MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AdapterHookConfig)));
    check(cfg != nullptr, "Map config");
    *cfg = {};
    cfg->magic = adapterHookConfigMagic;
    cfg->version = adapterHookConfigVersion;
    cfg->api = static_cast<std::uint32_t>(api);
    cfg->hostPid = GetCurrentProcessId();
    cfg->targetPid = target.pid;
    writeConfigHwnd(*cfg, target.hwnd);
    writeConfigU32(cfg->generation, 1);
    writeConfigU32(cfg->active, 1);

    runAttachHelper(attachExe, apiToken, target.pid, target.tid, target.hwnd);

    const std::wstring pipeName = makeFramePipeName(api, target.pid, target.hwnd);
    auto connectX86Pipe = [&]() {
        UniqueHandle p;
        const ULONGLONG dl = GetTickCount64() + 5000;
        while (GetTickCount64() < dl) {
            HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (h != INVALID_HANDLE_VALUE) { p.reset(h); break; }
            Sleep(10);
        }
        check(p.get() != nullptr, "Connect to x86 frame pipe");
        return p;
    };

    UniqueHandle pipe = connectX86Pipe();

    auto readColor = [&](std::uint32_t expectedW, std::uint32_t expectedH, const std::array<std::uint8_t, 4>& expected, int maxSkip = 12) {
        for (int i = 0; i <= maxSkip; ++i) {
            std::array<std::uint8_t, 16> hdr{};
            DWORD rd = 0;
            const ULONGLONG dl = GetTickCount64() + 3000;
            for (std::size_t off = 0; off < 16;) {
                DWORD avail = 0;
                check(PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &avail, nullptr) != FALSE, "Peek");
                if (!avail) { check(GetTickCount64() < dl, "Timeout hdr"); Sleep(2); continue; }
                check(ReadFile(pipe.get(), hdr.data() + off, static_cast<DWORD>(16 - off), &rd, nullptr) && rd, "Read hdr");
                off += rd;
            }
            FrameHeader parsed{};
            check(parseFrameHeaderBytes(hdr.data(), api, parsed), "Parse header");
            std::vector<std::uint8_t> px(static_cast<std::size_t>(parsed.width) * parsed.height * 4);
            for (std::size_t off = 0; off < px.size();) {
                DWORD avail = 0;
                check(PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &avail, nullptr) != FALSE, "Peek body");
                if (!avail) { check(GetTickCount64() < dl, "Timeout body"); Sleep(2); continue; }
                const DWORD chunk = static_cast<DWORD>((px.size() - off) < avail ? (px.size() - off) : avail);
                check(ReadFile(pipe.get(), px.data() + off, chunk, &rd, nullptr) && rd, "Read body");
                off += rd;
            }
            if (parsed.width == expectedW && parsed.height == expectedH
                && std::memcmp(px.data(), expected.data(), 4) == 0) {
                return;
            }
        }
        throw std::runtime_error("Expected x86 color/size not received");
    };

    readColor(160, 120, { 17, 73, 191, 255 });
    target.sendCmd('n');
    readColor(160, 120, { 211, 47, 89, 255 });
    target.sendCmd('r');
    readColor(200, 150, { 41, 173, 99, 255 }, 20);
    writeConfigU32(cfg->active, 0);
    UnmapViewOfFile(cfg);
    std::printf("[PASS] %s x86 WOW64 attach, pixel verification, Reset/resize recovery, and detach\n", apiToken);
    std::fflush(stdout);
}

} // namespace

int main() {
    try {
        static_assert(sizeof(AdapterHookConfig) == 64);

        d3d9::D3D9Offsets off = {};
        check(d3d9::probeD3D9Offsets(off) && off.devicePresentRva && off.swapChainPresentRva,
            "D3D9 x86 offset probe");

        const std::string attachExe = envRequired("MUXIVO_ATTACH_X86_EXE");
        verifyX86Api(CaptureApi::D3D9, "d3d9", envRequired("MUXIVO_TEST_D3D9_X86_EXE"), attachExe);
        verifyX86Api(CaptureApi::OpenGL, "opengl", envRequired("MUXIVO_TEST_OPENGL_X86_EXE"), attachExe);
        std::puts("All x86 adapter tests passed.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "X86 TEST FAILURE: %s\n", e.what());
        return 1;
    }
}

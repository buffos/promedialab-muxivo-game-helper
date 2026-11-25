#include <windows.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "d3d12_offsets.h"
#include "d3d9_offsets.h"
#include "muxivo_capture/adapter_controller.h"
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
        if (process) {
            if (WaitForSingleObject(process.get(), 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process.get(), 1);
            }
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
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        throw std::runtime_error("Failed to launch test target: " + exePath);
    }
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
    check(sscanf_s(line.c_str(), "READY %u %u %llx", &pid, &tid, &hwnd) == 3,
        ("Expected READY line, got: " + line).c_str());
    t.pid = pid;
    t.tid = tid;
    t.hwnd = static_cast<std::uint64_t>(hwnd);
    return t;
}

UniqueHandle connectPipe(const std::wstring& pipeName, std::uint32_t expectedPid, ULONGLONG timeoutMs = 5000) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD owner = 0;
            check(GetNamedPipeServerProcessId(h, &owner) && owner == expectedPid, "Kernel pipe server PID check");
            return UniqueHandle(h);
        }
        Sleep(10);
    }
    throw std::runtime_error("Timed out connecting to frame pipe");
}

void readFrameExpectColor(
    HANDLE pipe,
    CaptureApi expectedApi,
    std::uint32_t expectedW,
    std::uint32_t expectedH,
    const std::array<std::uint8_t, 4>& expectedColor,
    int maxSkipFrames = 12) {
    for (int attempt = 0; attempt <= maxSkipFrames; ++attempt) {
        std::array<std::uint8_t, 16> hdr{};
        const ULONGLONG deadline = GetTickCount64() + 4000;
        std::size_t off = 0;
        while (off < hdr.size()) {
            DWORD avail = 0;
            check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) != FALSE, "Peek frame header");
            if (avail == 0) {
                check(GetTickCount64() < deadline, "Timed out reading frame header");
                Sleep(2);
                continue;
            }
            DWORD rd = 0;
            check(ReadFile(pipe, hdr.data() + off, static_cast<DWORD>(hdr.size() - off), &rd, nullptr) && rd > 0,
                "Read frame header");
            off += rd;
        }
        FrameHeader parsed = {};
        check(AdapterController::parseFrameHeader(hdr.data(), expectedApi, parsed), "Parse frame header");
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(parsed.width) * parsed.height * 4);
        off = 0;
        while (off < rgba.size()) {
            DWORD avail = 0;
            check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) != FALSE, "Peek frame body");
            if (avail == 0) {
                check(GetTickCount64() < deadline, "Timed out reading frame body");
                Sleep(2);
                continue;
            }
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rgba.size() - off, avail));
            DWORD rd = 0;
            check(ReadFile(pipe, rgba.data() + off, chunk, &rd, nullptr) && rd > 0, "Read frame body");
            off += rd;
        }
        if (parsed.width == expectedW && parsed.height == expectedH
            && std::memcmp(rgba.data(), expectedColor.data(), 4) == 0
            && std::memcmp(rgba.data() + rgba.size() - 4, expectedColor.data(), 4) == 0) {
            return;
        }
    }
    throw std::runtime_error("Did not observe expected dimensions and RGBA pixels");
}

void testContracts(AdapterController& ctrl) {
    FrameHeader parsed = {};
    const std::array<std::uint8_t, 16> validD3D9 = {
        'M', 'X', 'G', 'C', 1, 1, 3, 0,
        160, 0, 0, 0, 120, 0, 0, 0
    };
    check(AdapterController::parseFrameHeader(validD3D9.data(), CaptureApi::D3D9, parsed)
        && parsed.width == 160 && parsed.height == 120, "Valid D3D9 header");
    check(!AdapterController::parseFrameHeader(validD3D9.data(), CaptureApi::D3D12, parsed),
        "Mismatched provider tag rejected");

    auto badMagic = validD3D9;
    badMagic[0] = 'X';
    check(!AdapterController::parseFrameHeader(badMagic.data(), CaptureApi::D3D9, parsed), "Bad magic rejected");

    auto badVersion = validD3D9;
    badVersion[4] = 2;
    check(!AdapterController::parseFrameHeader(badVersion.data(), CaptureApi::D3D9, parsed), "Bad version rejected");

    auto badStorage = validD3D9;
    badStorage[5] = 9;
    check(!AdapterController::parseFrameHeader(badStorage.data(), CaptureApi::D3D9, parsed), "Bad storage rejected");

    auto zeroWidth = validD3D9;
    zeroWidth[8] = 0;
    check(!AdapterController::parseFrameHeader(zeroWidth.data(), CaptureApi::D3D9, parsed), "Zero width rejected");

    auto hugeWidth = validD3D9;
    hugeWidth[8] = 1;
    hugeWidth[9] = 15; // 3841
    check(!AdapterController::parseFrameHeader(hugeWidth.data(), CaptureApi::D3D9, parsed), "Oversized width rejected");

    check(ctrl.attach(CaptureApi::D3D9, 0, 1, 1) == AttachStatus::TargetUnavailable, "Zero PID rejected");
    check(ctrl.attach(CaptureApi::D3D9, GetCurrentProcessId(), GetCurrentThreadId(), 1)
        == AttachStatus::TargetUnavailable, "Self PID rejected");
    check(ctrl.attach(CaptureApi::D3D9, GetCurrentProcessId() + 4, 1, 0x1234)
        == AttachStatus::TargetUnavailable, "Invalid HWND/TID rejected");
    std::puts("[PASS] Controller & MXGC protocol negative contract checks");
    std::fflush(stdout);
}

void testD3D9(AdapterController& ctrl, const std::string& d3d9Exe) {
    auto target = spawnAndWaitReady(d3d9Exe);
    const auto matches = ctrl.probe(CaptureApi::D3D9, { target.pid, GetCurrentProcessId() });
    check(matches.size() == 1 && matches[0].pid == target.pid && matches[0].arch == ProcessArch::X64,
        "D3D9 probe must find only target x64 process");
    std::wstring pipeName;
    check(ctrl.attach(CaptureApi::D3D9, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Attach D3D9");
    auto pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 160, 120, { 17, 73, 191, 255 });
    target.sendCmd('n');
    readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 160, 120, { 211, 47, 89, 255 });
    target.sendCmd('r');
    readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 200, 150, { 41, 173, 99, 255 }, 20);
    pipe.reset();
    ctrl.detach(CaptureApi::D3D9, target.pid, target.hwnd);

    check(ctrl.attach(CaptureApi::D3D9, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Re-attach D3D9 after detach");
    pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 200, 150, { 41, 173, 99, 255 }, 20);
    pipe.reset();
    ctrl.detach(CaptureApi::D3D9, target.pid, target.hwnd);
    std::puts("[PASS] D3D9 x64 mid-session attach, Reset recovery, detach, and re-attach");
    std::fflush(stdout);
}

void testD3D12MultiQueue(AdapterController& ctrl, const std::string& d3d12Exe) {
    auto target = spawnAndWaitReady(d3d12Exe);
    const auto matches = ctrl.probe(CaptureApi::D3D12, { target.pid, GetCurrentProcessId() });
    check(matches.size() == 1 && matches[0].pid == target.pid && matches[0].arch == ProcessArch::X64,
        "D3D12 probe must find only target x64 process");
    std::wstring pipeName;
    check(ctrl.attach(CaptureApi::D3D12, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Attach D3D12");
    auto pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::D3D12, 192, 128, { 17, 73, 191, 255 });
    std::puts("  [D3D12] Queue B initial RGBA8 192x128 frame verified");
    target.sendCmd('n');
    readFrameExpectColor(pipe.get(), CaptureApi::D3D12, 192, 128, { 211, 47, 89, 255 });
    std::puts("  [D3D12] Queue B updated RGBA8 192x128 frame verified");

    // Trigger ResizeBuffers1 moving presentation from Queue B to Queue C and format to BGRA8 256x144!
    target.sendCmd('r');
    readFrameExpectColor(pipe.get(), CaptureApi::D3D12, 256, 144, { 41, 173, 99, 255 }, 20);
    std::puts("  [D3D12] ResizeBuffers1 Queue B -> Queue C BGRA8 256x144 verified");

    // Trigger ResizeBuffers1(BufferCount=0) moving back to Queue B 224x160!
    target.sendCmd('0');
    readFrameExpectColor(pipe.get(), CaptureApi::D3D12, 224, 160, { 231, 195, 37, 255 }, 20);
    std::puts("  [D3D12] ResizeBuffers1(BufferCount=0) Queue C -> Queue B 224x160 verified");
    pipe.reset();
    ctrl.detach(CaptureApi::D3D12, target.pid, target.hwnd);

    check(ctrl.attach(CaptureApi::D3D12, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Re-attach D3D12 after detach");
    pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::D3D12, 224, 160, { 231, 195, 37, 255 }, 20);
    pipe.reset();
    ctrl.detach(CaptureApi::D3D12, target.pid, target.hwnd);
    std::puts("[PASS] D3D12 x64 multi-queue (decoy rejection, ResizeBuffers1/BufferCount=0 migration, detach, re-attach)");
    std::fflush(stdout);
}

void testOpenGL(AdapterController& ctrl, const std::string& glExe) {
    auto target = spawnAndWaitReady(glExe);
    const auto matches = ctrl.probe(CaptureApi::OpenGL, { target.pid, GetCurrentProcessId() });
    check(matches.size() == 1 && matches[0].pid == target.pid && matches[0].arch == ProcessArch::X64,
        "OpenGL probe must find only target x64 process");
    std::wstring pipeName;
    check(ctrl.attach(CaptureApi::OpenGL, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Attach OpenGL");
    auto pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 160, 120, { 17, 73, 191, 255 });
    target.sendCmd('n');
    readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 160, 120, { 211, 47, 89, 255 });
    target.sendCmd('r');
    readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 200, 150, { 41, 173, 99, 255 }, 20);
    pipe.reset();
    ctrl.detach(CaptureApi::OpenGL, target.pid, target.hwnd);

    check(ctrl.attach(CaptureApi::OpenGL, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
        "Re-attach OpenGL after detach");
    pipe = connectPipe(pipeName, target.pid);
    readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 200, 150, { 41, 173, 99, 255 }, 20);
    pipe.reset();
    ctrl.detach(CaptureApi::OpenGL, target.pid, target.hwnd);
    std::puts("[PASS] OpenGL x64 mid-session attach, GL state preservation, live resize on same pipe, detach, and re-attach");
    std::fflush(stdout);
}

void testWow64ThroughController(const std::filesystem::path& x64ExeDir) {
    const auto distBinDir = (x64ExeDir / ".." / ".." / ".." / "dist" / "bin").lexically_normal();
    const auto attachX86 = distBinDir / "muxivo-capture-attach-x86.exe";
    if (!std::filesystem::exists(attachX86)) {
        return;
    }

    auto resolveX86Target = [&](const char* envName, const char* fileName) -> std::string {
        char* raw = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&raw, &len, envName) == 0 && raw && *raw) {
            std::string val(raw);
            std::free(raw);
            return val;
        }
        std::free(raw);
        const auto rel = (x64ExeDir / ".." / ".." / "x86" / "Release" / fileName).lexically_normal();
        check(std::filesystem::exists(rel), ("Missing x86 test target for cross-arch test: " + rel.string()).c_str());
        return rel.string();
    };

    const std::string d3d9X86Exe = resolveX86Target("MUXIVO_TEST_D3D9_X86_EXE", "muxivo-test-d3d9-x86.exe");
    const std::string glX86Exe = resolveX86Target("MUXIVO_TEST_OPENGL_X86_EXE", "muxivo-test-opengl-x86.exe");
    AdapterController distCtrl(distBinDir.wstring());

    {
        auto target = spawnAndWaitReady(d3d9X86Exe);
        const auto matches = distCtrl.probe(CaptureApi::D3D9, { target.pid });
        check(matches.size() == 1 && matches[0].pid == target.pid && matches[0].arch == ProcessArch::X86,
            "Cross-arch D3D9 x86 probe failed");
        check(distCtrl.attach(CaptureApi::D3D12, target.pid, target.tid, target.hwnd) == AttachStatus::UnsupportedArch,
            "Cross-arch D3D12 x86 must return UnsupportedArch");
        std::wstring pipeName;
        check(distCtrl.attach(CaptureApi::D3D9, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
            "Cross-arch D3D9 x86 attach via dist/bin failed");
        auto pipe = connectPipe(pipeName, target.pid);
        readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 160, 120, { 17, 73, 191, 255 });
        target.sendCmd('r');
        readFrameExpectColor(pipe.get(), CaptureApi::D3D9, 200, 150, { 41, 173, 99, 255 }, 20);
        pipe.reset();
        distCtrl.detach(CaptureApi::D3D9, target.pid, target.hwnd);
    }

    {
        auto target = spawnAndWaitReady(glX86Exe);
        const auto matches = distCtrl.probe(CaptureApi::OpenGL, { target.pid });
        check(matches.size() == 1 && matches[0].pid == target.pid && matches[0].arch == ProcessArch::X86,
            "Cross-arch OpenGL x86 probe failed");
        std::wstring pipeName;
        check(distCtrl.attach(CaptureApi::OpenGL, target.pid, target.tid, target.hwnd, &pipeName) == AttachStatus::Ok,
            "Cross-arch OpenGL x86 attach via dist/bin failed");
        auto pipe = connectPipe(pipeName, target.pid);
        readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 160, 120, { 17, 73, 191, 255 });
        target.sendCmd('r');
        readFrameExpectColor(pipe.get(), CaptureApi::OpenGL, 200, 150, { 41, 173, 99, 255 }, 20);
        pipe.reset();
        distCtrl.detach(CaptureApi::OpenGL, target.pid, target.hwnd);
    }

    distCtrl.shutdown();
    std::puts("[PASS] Cross-arch x64 AdapterController -> x86 WOW64 (probe, D3D12 UnsupportedArch, D3D9 & OpenGL staged dist/bin)");
    std::fflush(stdout);
}

} // namespace

int main() {
    try {
        static_assert(sizeof(AdapterHookConfig) == 64);

        d3d9::D3D9Offsets d3d9Off = {};
        check(d3d9::probeD3D9Offsets(d3d9Off) && d3d9Off.devicePresentRva && d3d9Off.swapChainPresentRva,
            "D3D9 x64 offset probe");

        d3d12::DxgiSwapOffsets d3d12Off = {};
        check(d3d12::probeDxgiSwapOffsets(d3d12Off) && d3d12Off.presentRva && d3d12Off.resizeBuffersRva
            && d3d12Off.present1Rva && d3d12Off.resizeBuffers1Rva, "D3D12 DXGI x64 offset probe");

        wchar_t exePath[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
        const auto exeDir = std::filesystem::path(exePath).parent_path();
        AdapterController ctrl(exeDir.wstring());

        testContracts(ctrl);
        testD3D9(ctrl, envRequired("MUXIVO_TEST_D3D9_EXE"));
        testD3D12MultiQueue(ctrl, envRequired("MUXIVO_TEST_D3D12_EXE"));
        testOpenGL(ctrl, envRequired("MUXIVO_TEST_OPENGL_EXE"));
        testWow64ThroughController(exeDir);

        ctrl.shutdown();
        std::puts("All x64 adapter tests passed.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TEST FAILURE: %s\n", e.what());
        return 1;
    }
}

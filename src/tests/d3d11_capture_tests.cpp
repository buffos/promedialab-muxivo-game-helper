#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "d3d11_protocol.h"
#include "dxgi_offsets.h"
#include "security_descriptor.h"

namespace {

using muxivo::UniqueHandle;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring getEnv(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (!written) return {};
    value.resize(written);
    return value;
}

struct WindowSearch {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    DWORD tid = 0;
};

BOOL CALLBACK findProcessWindow(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid && IsWindowVisible(hwnd)) {
        search->hwnd = hwnd;
        search->tid = tid;
        return FALSE;
    }
    return TRUE;
}

WindowSearch waitForWindow(DWORD pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        WindowSearch search = { pid, nullptr, 0 };
        EnumWindows(&findProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (search.hwnd) return search;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("Timed out waiting for D3D11 fixture window");
}

struct HostProcess {
    UniqueHandle process;
    UniqueHandle stdinWrite;
    UniqueHandle stdoutRead;
    std::string buffer;

    static HostProcess launch(const std::wstring& exePath) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE inRead = nullptr, inWrite = nullptr;
        HANDLE outRead = nullptr, outWrite = nullptr;
        require(CreatePipe(&inRead, &inWrite, &sa, 0), "CreatePipe stdin");
        require(CreatePipe(&outRead, &outWrite, &sa, 0), "CreatePipe stdout");
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + exePath + L"\"";
        const BOOL ok = CreateProcessW(
            exePath.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        require(ok != FALSE, "CreateProcessW capture host");
        CloseHandle(pi.hThread);

        HostProcess host;
        host.process.reset(pi.hProcess);
        host.stdinWrite.reset(inWrite);
        host.stdoutRead.reset(outRead);

        const std::string ready = host.readLine(2000);
        require(ready == "MXGC/3", ("Unexpected host ready line: " + ready).c_str());
        return host;
    }

    HostProcess() = default;
    HostProcess(HostProcess&&) = default;
    HostProcess& operator=(HostProcess&&) = default;

    ~HostProcess() {
        stdinWrite.reset();
        if (process) WaitForSingleObject(process.get(), 2000);
    }

    std::string command(const std::string& cmd, DWORD timeoutMs = 5000) {
        const std::string wire = cmd + "\n";
        DWORD written = 0;
        require(WriteFile(stdinWrite.get(), wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr)
            && written == wire.size(), "WriteFile host command");
        return readLine(timeoutMs);
    }

    std::string readLine(DWORD timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto pos = buffer.find('\n');
            if (pos != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buffer.erase(0, pos + 1);
                return line;
            }
            DWORD available = 0;
            require(PeekNamedPipe(stdoutRead.get(), nullptr, 0, nullptr, &available, nullptr) != FALSE,
                "PeekNamedPipe host stdout");
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            char chunk[512] = {};
            DWORD count = 0;
            require(ReadFile(stdoutRead.get(), chunk, sizeof(chunk), &count, nullptr) && count > 0,
                "ReadFile host stdout");
            buffer.append(chunk, count);
        }
        throw std::runtime_error("Timed out reading line from capture host");
    }
};

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

bool readExactTimeout(HANDLE pipe, std::uint8_t* dst, std::size_t size, DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::size_t offset = 0;
    while (offset < size) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        DWORD readBytes = 0;
        const auto chunk = static_cast<DWORD>((std::min)(size - offset, static_cast<std::size_t>(available)));
        if (!ReadFile(pipe, dst + offset, chunk, &readBytes, nullptr) || readBytes == 0) return false;
        offset += readBytes;
    }
    return true;
}

UniqueHandle openGamePipe(DWORD pid, HWND hwnd) {
    wchar_t pipeName[128] = {};
    swprintf_s(pipeName, L"\\\\.\\pipe\\%ls.%lu.%llx",
        muxivo::muxivoD3D11PipeEndpoint, pid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE h = CreateFileW(pipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ULONG serverPid = 0;
            require(GetNamedPipeServerProcessId(h, &serverPid) && serverPid == pid,
                "Pipe server PID mismatch");
            return UniqueHandle(h);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Timed out opening D3D11 capture pipe");
}

Frame readFrame(HANDLE pipe) {
    std::uint8_t header[16] = {};
    require(readExactTimeout(pipe, header, sizeof(header), 2000), "Read D3D11 frame header");
    require(std::memcmp(header, muxivo::muxivoD3D11FrameTag.data(), 8) == 0, "Invalid D3D11 frame signature");
    Frame frame;
    std::memcpy(&frame.width, header + 8, 4);
    std::memcpy(&frame.height, header + 12, 4);
    require(frame.width > 0 && frame.height > 0, "Invalid D3D11 frame dimensions");
    frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    require(readExactTimeout(pipe, frame.pixels.data(), frame.pixels.size(), 2000), "Read D3D11 frame pixels");
    return frame;
}

std::array<std::uint8_t, 4> pixelAt(const Frame& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
    return { frame.pixels[offset], frame.pixels[offset + 1], frame.pixels[offset + 2], frame.pixels[offset + 3] };
}

} // namespace

int main() {
    try {
        const auto fixtureExe = getEnv(L"MUXIVO_D3D11_FIXTURE_EXE");
        const auto hostExe = getEnv(L"MUXIVO_CAPTURE_HOST_EXE");
        if (!hostExe.empty()) {
            auto host = HostProcess::launch(hostExe);
            require(host.command("UNKNOWN_CMD 123") == "ERR INVALID_REQUEST",
                "Expected INVALID_REQUEST for unknown command");
            require(host.command("PROBE_D3D11 not_a_pid") == "ERR INVALID_REQUEST",
                "Expected INVALID_REQUEST for invalid probe PID");
            require(host.command("ATTACH_D3D11 0 0 0") == "ERR TARGET_UNAVAILABLE",
                "Expected TARGET_UNAVAILABLE for zero PID/TID/HWND");
            const std::string oversized(4500, 'A');
            require(host.command(oversized) == "ERR INVALID_REQUEST",
                "Expected single INVALID_REQUEST for oversized line");
            require(host.command("PROBE_D3D11 0") == "OK",
                "Expected host framing to remain synchronized after oversized line");
            require(host.command("DETACH_D3D11 0 0") == "OK",
                "Expected DETACH_D3D11 on non-attached target to succeed idempotently");
            for (const char* api : { "D3D9", "D3D12", "OPENGL" }) {
                const std::string p = api;
                require(host.command("PROBE_" + p + " not_a_pid") == "ERR INVALID_REQUEST",
                    "Expected INVALID_REQUEST for invalid adapter probe PID");
                std::string tooMany = "PROBE_" + p;
                for (int i = 0; i < 129; ++i) tooMany += " 0";
                require(host.command(tooMany) == "ERR INVALID_REQUEST",
                    "Expected INVALID_REQUEST for >128 adapter probe PIDs");
                require(host.command("PROBE_" + p + " 0") == "OK",
                    "Expected empty OK for PID 0 adapter probe");
                require(host.command("ATTACH_" + p + " 0 0 0") == "ERR TARGET_UNAVAILABLE",
                    "Expected TARGET_UNAVAILABLE for zero PID/TID/HWND");
                require(host.command("ATTACH_" + p + " 1 2") == "ERR INVALID_REQUEST",
                    "Expected INVALID_REQUEST for truncated ATTACH command");
                require(host.command("DETACH_" + p + " 0 0") == "OK",
                    "Expected idempotent DETACH on non-attached target");
            }
        }
        if (!fixtureExe.empty() && !hostExe.empty()) {
            auto launchFixture = [&](const wchar_t* args) {
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + fixtureExe + L"\" " + args;
            require(CreateProcessW(fixtureExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE,
                "Launch D3D11 fixture");
            CloseHandle(pi.hThread);
            return std::pair<UniqueHandle, DWORD>(UniqueHandle(pi.hProcess), pi.dwProcessId);
        };

        auto [redProc, redPid] = launchFixture(L"--frames=600");
        auto [greenProc, greenPid] = launchFixture(L"--green --frames=600");
        const auto terminateFixtures = [&]() {
            if (redProc) TerminateProcess(redProc.get(), 0);
            if (greenProc) TerminateProcess(greenProc.get(), 0);
        };

        try {
            const auto redWin = waitForWindow(redPid);
            const auto greenWin = waitForWindow(greenPid);
            SetWindowPos(redWin.hwnd, HWND_TOPMOST, 80, 80, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(greenWin.hwnd, HWND_BOTTOM, 760, 80, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

            {
                auto host = HostProcess::launch(hostExe);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                std::string probeReply;
                const std::string expected = "OK " + std::to_string(redPid) + " " + std::to_string(greenPid);
                while (std::chrono::steady_clock::now() < deadline) {
                    probeReply = host.command(
                        "PROBE_D3D11 " + std::to_string(redPid) + " " + std::to_string(greenPid) + " "
                        + std::to_string(GetCurrentProcessId()));
                    if (probeReply == expected) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                require(probeReply == expected, ("Unexpected PROBE_D3D11 reply: " + probeReply).c_str());

                char attachCmd[128] = {};
                std::snprintf(attachCmd, sizeof(attachCmd), "ATTACH_D3D11 %lu %lu %llx",
                    redPid, redWin.tid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(redWin.hwnd)));
                require(host.command(attachCmd) == "OK", "ATTACH_D3D11 failed");

                auto pipe = openGamePipe(redPid, redWin.hwnd);
                bool sawBlack = false;
                bool sawWhite = false;
                for (int i = 0; i < 6; ++i) {
                    const Frame f = readFrame(pipe.get());
                    require(f.width == 640 && f.height == 360, "Unexpected fixture dimensions");
                    require((pixelAt(f, 4, 4) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }),
                        "Expected red identity marker");
                    require((pixelAt(f, 600, 320) == std::array<std::uint8_t, 4>{ 0, 0, 255, 255 }),
                        "Expected blue background");
                    const auto center = pixelAt(f, 320, 180);
                    if (center == std::array<std::uint8_t, 4>{ 0, 0, 0, 255 }) sawBlack = true;
                    if (center == std::array<std::uint8_t, 4>{ 255, 255, 255, 255 }) sawWhite = true;
                    require(sawBlack || sawWhite, "Expected black or white cadence marker");
                }
                wchar_t desktopName[128] = {};
                DWORD needed = 0;
                if (GetUserObjectInformationW(
                        GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktopName, sizeof(desktopName), &needed)
                    && _wcsicmp(desktopName, L"Default") == 0) {
                    require(sawBlack && sawWhite, "Expected cadence marker to alternate black and white on Default desktop");
                }
            }

            // Host exited; restart a replacement host and reattach to the already-hooked red target.
            {
                auto host2 = HostProcess::launch(hostExe);
                char attachCmd[128] = {};
                std::snprintf(attachCmd, sizeof(attachCmd), "ATTACH_D3D11 %lu %lu %llx",
                    redPid, redWin.tid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(redWin.hwnd)));
                require(host2.command(attachCmd) == "OK", "Replacement host ATTACH_D3D11 failed");
                auto pipe2 = openGamePipe(redPid, redWin.hwnd);
                const Frame f2 = readFrame(pipe2.get());
                require((pixelAt(f2, 4, 4) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }),
                    "Expected red marker after host restart");

                char detachCmd[128] = {};
                std::snprintf(detachCmd, sizeof(detachCmd), "DETACH_D3D11 %lu %llx",
                    redPid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(redWin.hwnd)));
                require(host2.command(detachCmd) == "OK", "DETACH_D3D11 failed");

                // Switch to the same-title green instance and verify target isolation.
                std::snprintf(attachCmd, sizeof(attachCmd), "ATTACH_D3D11 %lu %lu %llx",
                    greenPid, greenWin.tid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(greenWin.hwnd)));
                require(host2.command(attachCmd) == "OK", "Green ATTACH_D3D11 failed");
                auto greenPipe = openGamePipe(greenPid, greenWin.hwnd);
                const Frame gf = readFrame(greenPipe.get());
                require(gf.width == 640 && gf.height == 360, "Unexpected green fixture dimensions");
                require((pixelAt(gf, 4, 4) == std::array<std::uint8_t, 4>{ 0, 255, 0, 255 }),
                    "Expected green identity marker");

                std::snprintf(detachCmd, sizeof(detachCmd), "DETACH_D3D11 %lu %llx",
                    greenPid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(greenWin.hwnd)));
                require(host2.command(detachCmd) == "OK", "Green DETACH_D3D11 failed");
            }

            terminateFixtures();
        } catch (...) {
            terminateFixtures();
            throw;
        }
        } else {
            std::puts("Skipping fixture process test (MUXIVO_D3D11_FIXTURE_EXE not set)");
        }

        muxivo::DxgiOffsets offsets = {};
        require(muxivo::probeDxgiOffsets(offsets), "probeDxgiOffsets failed");
        require(offsets.presentRva != 0 && offsets.present1Rva != 0 && offsets.resizeBuffersRva != 0,
            "Incomplete DXGI RVAs");
        std::printf("DXGI offsets ok: present=0x%x present1=0x%x resize=0x%x\n",
            offsets.presentRva, offsets.present1Rva, offsets.resizeBuffersRva);

        std::puts("Direct3D 11 host + hook capture tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Test failure: %s\n", error.what());
        return 1;
    }
}

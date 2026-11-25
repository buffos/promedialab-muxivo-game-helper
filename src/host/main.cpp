#include <windows.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "d3d11_protocol.h"
#include "injector.h"
#include "muxivo_capture/adapter_controller.h"
#include "security_descriptor.h"
#include "vulkan_presence.h"

namespace {

constexpr std::size_t maxCommandBytes = 4096;

bool writeAll(HANDLE pipe, std::string_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(text.size() - offset);
        if (!WriteFile(pipe, text.data() + offset, chunk, &count, nullptr) || count == 0) {
            return false;
        }
        offset += count;
    }
    return true;
}

bool readCommandLine(HANDLE pipe, std::string& buffer, std::string& line) {
    bool discardingOversizedLine = false;
    while (true) {
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            if (discardingOversizedLine) {
                buffer.erase(0, newline + 1);
                line.clear();
                return true;
            }
            line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, newline + 1);
            return true;
        }
        if (buffer.size() >= maxCommandBytes) {
            discardingOversizedLine = true;
            buffer.clear();
        }
        char chunk[512] = {};
        DWORD count = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &count, nullptr) || count == 0) {
            return false;
        }
        buffer.append(chunk, count);
    }
}

std::vector<std::string_view> splitTokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos >= line.size()) break;
        const std::size_t end = line.find(' ', pos);
        if (end == std::string_view::npos) {
            tokens.push_back(line.substr(pos));
            break;
        }
        tokens.push_back(line.substr(pos, end - pos));
        pos = end + 1;
    }
    return tokens;
}

template<typename Integer>
bool parseInteger(std::string_view token, int base, Integer& value) noexcept {
    if (token.empty()) return false;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, value, base);
    return result.ec == std::errc() && result.ptr == end;
}

struct AdapterCommandEntry {
    std::string_view probe;
    std::string_view attach;
    std::string_view detach;
    muxivo::capture::CaptureApi api;
};

constexpr AdapterCommandEntry adapterCommands[] = {
    { "PROBE_D3D9", "ATTACH_D3D9", "DETACH_D3D9", muxivo::capture::CaptureApi::D3D9 },
    { "PROBE_D3D12", "ATTACH_D3D12", "DETACH_D3D12", muxivo::capture::CaptureApi::D3D12 },
    { "PROBE_OPENGL", "ATTACH_OPENGL", "DETACH_OPENGL", muxivo::capture::CaptureApi::OpenGL },
};

std::string handleAdapterProbe(
    const std::vector<std::string_view>& tokens,
    const muxivo::capture::AdapterController& adapters,
    muxivo::capture::CaptureApi api) {
    if (tokens.size() > 129) return "ERR INVALID_REQUEST\n";
    std::vector<std::uint32_t> pids;
    pids.reserve(tokens.size() - 1);
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        std::uint32_t pid = 0;
        if (!parseInteger(tokens[index], 10, pid)) return "ERR INVALID_REQUEST\n";
        pids.push_back(pid);
    }
    const auto matches = adapters.probe(api, pids);
    std::string reply = "OK";
    for (const auto& match : matches) {
        if (api == muxivo::capture::CaptureApi::D3D12
            && match.arch != muxivo::capture::ProcessArch::X64) {
            continue;
        }
        reply.push_back(' ');
        reply.append(std::to_string(match.pid));
    }
    reply.push_back('\n');
    return reply;
}

std::string handleAdapterAttach(
    const std::vector<std::string_view>& tokens,
    muxivo::capture::AdapterController& adapters,
    muxivo::capture::CaptureApi api) {
    if (tokens.size() != 4) return "ERR INVALID_REQUEST\n";
    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    std::uint64_t hwnd = 0;
    if (!parseInteger(tokens[1], 10, pid)
        || !parseInteger(tokens[2], 10, tid)
        || !parseInteger(tokens[3], 16, hwnd)) {
        return "ERR INVALID_REQUEST\n";
    }
    const auto status = adapters.attach(api, pid, tid, hwnd);
    if (status == muxivo::capture::AttachStatus::Ok) return "OK\n";
    return std::string("ERR ") + muxivo::capture::attachStatusCode(status) + "\n";
}

std::string handleAdapterDetach(
    const std::vector<std::string_view>& tokens,
    muxivo::capture::AdapterController& adapters,
    muxivo::capture::CaptureApi api) {
    if (tokens.size() != 3) return "ERR INVALID_REQUEST\n";
    std::uint32_t pid = 0;
    std::uint64_t hwnd = 0;
    if (!parseInteger(tokens[1], 10, pid) || !parseInteger(tokens[2], 16, hwnd)) {
        return "ERR INVALID_REQUEST\n";
    }
    adapters.detach(api, pid, hwnd);
    return "OK\n";
}

std::string handleCommand(
    std::string_view line,
    muxivo::D3D11HostManager& d3d11,
    muxivo::capture::AdapterController& adapters) {
    const auto tokens = splitTokens(line);
    if (tokens.empty()) return "ERR INVALID_REQUEST\n";

    if (tokens[0] == "PROBE_D3D11") {
        if (tokens.size() > 129) return "ERR INVALID_REQUEST\n";
        std::vector<std::uint32_t> pids;
        pids.reserve(tokens.size() - 1);
        for (std::size_t index = 1; index < tokens.size(); ++index) {
            std::uint32_t pid = 0;
            if (!parseInteger(tokens[index], 10, pid)) return "ERR INVALID_REQUEST\n";
            pids.push_back(pid);
        }
        std::string reply = "OK";
        for (const std::uint32_t pid : pids) {
            if (muxivo::probeD3D11Process(pid)) {
                reply.push_back(' ');
                reply.append(std::to_string(pid));
            }
        }
        reply.push_back('\n');
        return reply;
    }

    if (tokens[0] == "ATTACH_D3D11") {
        if (tokens.size() != 4) return "ERR INVALID_REQUEST\n";
        std::uint32_t pid = 0;
        std::uint32_t tid = 0;
        std::uintptr_t hwnd = 0;
        if (!parseInteger(tokens[1], 10, pid)
            || !parseInteger(tokens[2], 10, tid)
            || !parseInteger(tokens[3], 16, hwnd)) {
            return "ERR INVALID_REQUEST\n";
        }
        const auto status = d3d11.attach(pid, tid, reinterpret_cast<HWND>(hwnd));
        if (status == muxivo::AttachStatus::Ok) return "OK\n";
        return std::string("ERR ") + muxivo::attachErrorCode(status) + "\n";
    }

    if (tokens[0] == "DETACH_D3D11") {
        if (tokens.size() != 3) return "ERR INVALID_REQUEST\n";
        std::uint32_t pid = 0;
        std::uintptr_t hwnd = 0;
        if (!parseInteger(tokens[1], 10, pid) || !parseInteger(tokens[2], 16, hwnd)) {
            return "ERR INVALID_REQUEST\n";
        }
        d3d11.detach(pid, reinterpret_cast<HWND>(hwnd));
        return "OK\n";
    }

    for (const auto& entry : adapterCommands) {
        if (tokens[0] == entry.probe) return handleAdapterProbe(tokens, adapters, entry.api);
        if (tokens[0] == entry.attach) return handleAdapterAttach(tokens, adapters, entry.api);
        if (tokens[0] == entry.detach) return handleAdapterDetach(tokens, adapters, entry.api);
    }

    return "ERR INVALID_REQUEST\n";
}

} // namespace

int main() {
    const HANDLE parent = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE reply = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(parent) != FILE_TYPE_PIPE || GetFileType(reply) != FILE_TYPE_PIPE) return 1;

    const auto security = muxivo::tryCreateCurrentUserSecurityDescriptor();
    if (!security) return 1;
    SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };

    const HANDLE vulkanPresence = CreateEventW(&attributes, TRUE, FALSE, muxivoVulkanPresenceEvent);
    const HANDLE d3d11Presence = CreateEventW(&attributes, TRUE, FALSE, muxivo::muxivoD3D11PresenceEvent);
    if (!vulkanPresence || !d3d11Presence) {
        if (vulkanPresence) CloseHandle(vulkanPresence);
        if (d3d11Presence) CloseHandle(d3d11Presence);
        return 1;
    }

    muxivo::D3D11HostManager d3d11;
    muxivo::capture::AdapterController adapters;
    if (!adapters.ensureApiReady(muxivo::capture::CaptureApi::D3D9)
        || !adapters.ensureApiReady(muxivo::capture::CaptureApi::D3D12)
        || !adapters.ensureApiReady(muxivo::capture::CaptureApi::OpenGL)) {
        adapters.shutdown();
        CloseHandle(d3d11Presence);
        CloseHandle(vulkanPresence);
        return 1;
    }

    constexpr std::string_view ready = "MXGC/3\n";
    bool ok = writeAll(reply, ready);
    if (ok) {
        std::string buffer;
        std::string line;
        // The parent owns the only write end. EOF also retires us after an app crash.
        while (readCommandLine(parent, buffer, line)) {
            const std::string response = handleCommand(line, d3d11, adapters);
            if (!writeAll(reply, response)) break;
        }
    }

    adapters.shutdown();
    d3d11.shutdown();
    CloseHandle(d3d11Presence);
    CloseHandle(vulkanPresence);
    return ok ? 0 : 1;
}

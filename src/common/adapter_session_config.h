#pragma once

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "muxivo_capture/adapter_protocol.h"
#include "security_descriptor.h"

namespace muxivo::capture {

struct ConfigSnapshot {
    bool active = false;
    std::uint32_t generation = 0;
    std::uint64_t targetHwnd = 0;
};

class SessionConfigClient {
public:
    explicit SessionConfigClient(CaptureApi api) noexcept : api_(api) {
        const DWORD pid = GetCurrentProcessId();
        const wchar_t* prefix = apiEventPrefix(api);
        swprintf_s(presenceName_, L"Local\\Muxivo.%lsGameCapture.v1", prefix);
        swprintf_s(configName_, L"Local\\Muxivo.%lsGameCapture.Config.v1.%lu", prefix, pid);
        swprintf_s(loadedName_, L"Local\\Muxivo.%lsGameCapture.Loaded.v1.%lu", prefix, pid);
    }

    ~SessionConfigClient() {
        closeConfig();
    }

    SessionConfigClient(const SessionConfigClient&) = delete;
    SessionConfigClient& operator=(const SessionConfigClient&) = delete;

    bool isIdleCached(std::chrono::steady_clock::time_point now) const noexcept {
        return !hostRunning_ && now < nextPresenceCheck_;
    }

    bool checkHostRunning(std::chrono::steady_clock::time_point now, bool& justStopped) noexcept {
        justStopped = false;
        if (now < nextPresenceCheck_) return hostRunning_;

        const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, presenceName_);
        const bool running = event != nullptr;
        if (event) CloseHandle(event);

        nextPresenceCheck_ = now + std::chrono::milliseconds((running && configView_) ? 250 : 50);
        if (running && !configView_) {
            openConfigMapping();
        } else if (!running && hostRunning_) {
            justStopped = true;
            closeConfig();
        } else if (!running) {
            closeConfig();
        }
        hostRunning_ = running;
        return hostRunning_;
    }

    bool openConfigMapping() noexcept {
        if (configView_) return true;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, configName_);
        if (!mapping) return false;
        const auto* view = static_cast<const AdapterHookConfig*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(AdapterHookConfig)));
        if (!view || view->magic != adapterHookConfigMagic
            || view->version != adapterHookConfigVersion
            || view->api != static_cast<std::uint32_t>(api_)
            || view->targetPid != GetCurrentProcessId()) {
            if (view) UnmapViewOfFile(view);
            CloseHandle(mapping);
            return false;
        }
        configMapping_ = mapping;
        configView_ = view;
        return true;
    }

    void closeConfig() noexcept {
        if (configView_) {
            UnmapViewOfFile(configView_);
            configView_ = nullptr;
        }
        if (configMapping_) {
            CloseHandle(configMapping_);
            configMapping_ = nullptr;
        }
    }

    bool hasConfig() const noexcept { return configView_ != nullptr; }

    bool readSnapshot(ConfigSnapshot& out) const noexcept {
        out = {};
        if (!configView_) return false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint32_t genBefore = readConfigU32(configView_->generation);
            const std::uint32_t active = readConfigU32(configView_->active);
            const std::uint64_t hwnd = readConfigHwnd(*configView_);
            const std::uint32_t genAfter = readConfigU32(configView_->generation);
            if (genBefore == genAfter) {
                out.active = (active != 0);
                out.generation = genBefore;
                out.targetHwnd = hwnd;
                return true;
            }
        }
        return false;
    }

    std::uint32_t readRva(std::size_t index) const noexcept {
        if (!configView_ || index >= 6) return 0;
        return readConfigU32(configView_->rva[index]);
    }

    HANDLE createLoadedEvent() const noexcept {
        const auto security = tryCreateCurrentUserSecurityDescriptor();
        if (!security) return nullptr;
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        return CreateEventW(&attributes, TRUE, FALSE, loadedName_);
    }

private:
    const CaptureApi api_;
    wchar_t presenceName_[128] = {};
    wchar_t configName_[128] = {};
    wchar_t loadedName_[128] = {};
    HANDLE configMapping_ = nullptr;
    const AdapterHookConfig* configView_ = nullptr;
    bool hostRunning_ = false;
    std::chrono::steady_clock::time_point nextPresenceCheck_{};
};

} // namespace muxivo::capture

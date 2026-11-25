#pragma once

#include <windows.h>

#include <atomic>

namespace muxivo::capture {

struct ReentrancyGuard {
    int& depth;
    explicit ReentrancyGuard(int& d) noexcept : depth(d) { ++depth; }
    ~ReentrancyGuard() { --depth; }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
};

template <bool (*HooksReadyFn)(), bool (*BootstrapFn)()>
class AdapterDllBootstrap {
public:
    static void triggerOnce() noexcept {
        if (HooksReadyFn()) return;
        if (running().exchange(true, std::memory_order_acq_rel)) return;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&AdapterDllBootstrap::triggerOnce),
                &pinned)) {
            running().store(false, std::memory_order_release);
            return;
        }
        const HANDLE thread = CreateThread(nullptr, 0, &AdapterDllBootstrap::threadProc, pinned, 0, nullptr);
        if (!thread) {
            FreeLibrary(pinned);
            running().store(false, std::memory_order_release);
            return;
        }
        CloseHandle(thread);
    }

private:
    static std::atomic<bool>& running() noexcept {
        static std::atomic<bool> flag{ false };
        return flag;
    }

    static DWORD WINAPI threadProc(LPVOID param) {
        const auto pinned = static_cast<HMODULE>(param);
        if (BootstrapFn()) {
            return 0;
        }
        running().store(false, std::memory_order_release);
        FreeLibraryAndExitThread(pinned, 0);
    }
};

} // namespace muxivo::capture

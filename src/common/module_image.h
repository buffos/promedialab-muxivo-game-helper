#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace muxivo::capture {

struct ModuleImageRange {
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
};

class ModuleGuard {
public:
    explicit ModuleGuard(HMODULE module = nullptr) noexcept : module_(module) {}
    ~ModuleGuard() {
        if (module_) FreeLibrary(module_);
    }

    ModuleGuard(const ModuleGuard&) = delete;
    ModuleGuard& operator=(const ModuleGuard&) = delete;

    HMODULE get() const noexcept { return module_; }
    explicit operator bool() const noexcept { return module_ != nullptr; }

private:
    HMODULE module_ = nullptr;
};

inline bool readModuleImageRange(HMODULE module, ModuleImageRange& range) noexcept {
    if (!module) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || dos->e_lfanew >= 0x100000 || (dos->e_lfanew % 4) != 0) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC
        || nt->OptionalHeader.SizeOfImage == 0) {
        return false;
    }
    range.base = base;
    range.size = nt->OptionalHeader.SizeOfImage;
    return true;
}

inline bool readVtableRva(
    const void* instance, std::size_t slot, const ModuleImageRange& range, std::uint32_t& rva) noexcept {
    if (!instance) return false;
    const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(instance);
    if (!vtable) return false;
    const std::uintptr_t address = vtable[slot];
    if (address < range.base || address - range.base >= range.size) return false;
    rva = static_cast<std::uint32_t>(address - range.base);
    return true;
}

class DummyWindow {
public:
    bool create(const wchar_t* prefix) noexcept {
        static std::atomic<std::uint64_t> counter{ 0 };
        const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        instance_ = GetModuleHandleW(nullptr);
        swprintf_s(className_, L"%ls_%lu_%llu",
            prefix, GetCurrentProcessId(), static_cast<unsigned long long>(seq));
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance_;
        wc.lpszClassName = className_;
        if (!RegisterClassExW(&wc)) {
            className_[0] = 0;
            return false;
        }
        window_ = CreateWindowExW(
            0, className_, L"", WS_POPUP,
            0, 0, 2, 2, nullptr, nullptr, instance_, nullptr);
        return window_ != nullptr;
    }

    ~DummyWindow() {
        if (window_) DestroyWindow(window_);
        if (className_[0]) UnregisterClassW(className_, instance_);
    }

    HWND handle() const noexcept { return window_; }

private:
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    wchar_t className_[64] = {};
};

} // namespace muxivo::capture

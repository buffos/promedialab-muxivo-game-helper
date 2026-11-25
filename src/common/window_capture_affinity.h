#pragma once

#include <windows.h>

namespace muxivo {

inline bool presentWindowAllowsCapture(HWND window) noexcept {
    DWORD owner = 0;
    if (!window || !IsWindow(window) || IsIconic(window)
        || !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId()) {
        return false;
    }
    const HWND root = GetAncestor(window, GA_ROOT);
    DWORD affinity = WDA_NONE;
    return root && GetWindowDisplayAffinity(root, &affinity) && affinity == WDA_NONE;
}

} // namespace muxivo

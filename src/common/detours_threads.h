#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <detours.h>

#include <vector>

namespace muxivo::capture {

inline std::vector<HANDLE> updateOtherProcessThreads() noexcept {
    std::vector<HANDLE> threads;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return threads;

    const DWORD currentPid = GetCurrentProcessId();
    const DWORD currentTid = GetCurrentThreadId();
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID == currentPid && entry.th32ThreadID != currentTid) {
                const HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
                if (thread) {
                    DetourUpdateThread(thread);
                    threads.push_back(thread);
                }
            }
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    return threads;
}

} // namespace muxivo::capture

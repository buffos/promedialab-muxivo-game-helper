#pragma once

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace muxivo {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreeDeleter {
    void operator()(void* pointer) const noexcept {
        if (pointer) LocalFree(pointer);
    }
};

using SecurityDescriptorPtr = std::unique_ptr<void, LocalFreeDeleter>;

inline SecurityDescriptorPtr tryCreateCurrentUserSecurityDescriptor() noexcept {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return nullptr;
    }
    const UniqueHandle token(rawToken);

    DWORD size = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &size);
    if (!size) return nullptr;

    std::vector<std::uint8_t> identity(size);
    if (!GetTokenInformation(rawToken, TokenUser, identity.data(), size, &size)) {
        return nullptr;
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(identity.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid)) {
        return nullptr;
    }
    const std::unique_ptr<void, LocalFreeDeleter> sid(rawSid);

    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + rawSid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return nullptr;
    }
    return SecurityDescriptorPtr(descriptor);
}

inline SecurityDescriptorPtr createCurrentUserSecurityDescriptor() {
    auto descriptor = tryCreateCurrentUserSecurityDescriptor();
    if (!descriptor) {
        throw std::runtime_error("Could not restrict game capture to the current Windows account");
    }
    return descriptor;
}

} // namespace muxivo

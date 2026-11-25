#pragma once

#include <windows.h>

#include <charconv>
#include <cstring>

namespace muxivo::capture {

template <typename Integer>
bool parseWideInt(const wchar_t* text, int base, Integer& value) noexcept {
    if (!text || !*text) return false;
    char buf[64] = {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (len <= 1) return false;
    const char* begin = buf;
    const char* end = buf + len - 1;
    if (base == 16 && (end - begin) > 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
        begin += 2;
    }
    const auto result = std::from_chars(begin, end, value, base);
    return result.ec == std::errc() && result.ptr == end;
}

} // namespace muxivo::capture

#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <cstddef>

namespace muxivo::d3d11 {

template <typename Read>
HRESULT collectNewestPending(std::size_t capacity, std::size_t& first, std::size_t& pending, Read read) {
    for (auto ready = pending; ready != 0; --ready) {
        const auto slot = (first + ready - 1) % capacity;
        const UINT flags = ready == 1 && pending == capacity ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT;
        const HRESULT result = read(slot, flags);
        if (result == S_OK) {
            // Skip older frames once a newer copy can be read. Reuse all slots
            // through the same immediate context, which orders writes to them.
            first = (first + ready) % capacity;
            pending -= ready;
            return S_OK;
        }
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) return result;
    }
    return S_FALSE;
}

} // namespace muxivo::d3d11

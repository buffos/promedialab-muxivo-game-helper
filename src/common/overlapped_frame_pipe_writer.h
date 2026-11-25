#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "security_descriptor.h"

namespace muxivo {

class OverlappedFramePipeWriter {
    using Packet = std::vector<std::uint8_t>;
    static constexpr DWORD capacity = 3;

    class Channel {
        enum class State { Disconnected, Connecting, Ready, Writing };

    public:
        ~Channel() {
            if (state_ == State::Connecting || state_ == State::Writing) {
                DWORD ignored = 0;
                CancelIoEx(pipe_.get(), &operation_);
                // Both the operation and its packet must outlive cancellation.
                GetOverlappedResult(pipe_.get(), &operation_, &ignored, TRUE);
            }
        }

        void open(const wchar_t* name, DWORD access, DWORD bufferSize, SECURITY_ATTRIBUTES& attributes) {
            event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            pipe_.reset(CreateNamedPipeW(name, access | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                capacity, bufferSize, 0, 0, &attributes));
            if (!event_ || !pipe_ || pipe_.get() == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Could not create an asynchronous game frame pipe");
            }
        }

        bool ready() const { return state_ == State::Ready; }

        void poll() {
            if (state_ == State::Connecting || state_ == State::Writing) {
                DWORD count = 0;
                if (!GetOverlappedResult(pipe_.get(), &operation_, &count, FALSE)) {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) return;
                    disconnect();
                } else if (state_ == State::Writing && count != packet_->size()) {
                    disconnect();
                } else {
                    packet_.reset();
                    state_ = State::Ready;
                }
            }
            if (state_ != State::Disconnected) return;
            resetOperation();
            if (ConnectNamedPipe(pipe_.get(), &operation_)) {
                state_ = State::Ready;
                return;
            }
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) state_ = State::Ready;
            else if (error == ERROR_IO_PENDING) state_ = State::Connecting;
            else if (error == ERROR_NO_DATA) disconnect();
        }

        void send(const std::shared_ptr<const Packet>& packet) {
            resetOperation();
            // The kernel advances a packet larger than the pipe quota as the
            // reader drains it. Each reader has at most one outstanding packet.
            if (WriteFile(pipe_.get(), packet->data(), static_cast<DWORD>(packet->size()),
                    nullptr, &operation_)) return;
            if (GetLastError() == ERROR_IO_PENDING) {
                packet_ = packet;
                state_ = State::Writing;
            } else {
                disconnect();
            }
        }

    private:
        void resetOperation() {
            ResetEvent(event_.get());
            operation_ = {};
            operation_.hEvent = event_.get();
        }

        void disconnect() {
            DisconnectNamedPipe(pipe_.get());
            packet_.reset();
            state_ = State::Disconnected;
        }

        UniqueHandle pipe_;
        UniqueHandle event_;
        OVERLAPPED operation_ = {};
        State state_ = State::Disconnected;
        std::shared_ptr<const Packet> packet_;
    };

public:
    OverlappedFramePipeWriter(HWND window, const wchar_t* endpoint, DWORD bufferSize) {
        wchar_t name[128] = {};
        swprintf_s(name, L"\\\\.\\pipe\\%ls.%lu.%llx", endpoint, GetCurrentProcessId(),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)));
        const auto security = createCurrentUserSecurityDescriptor();
        SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE };
        for (std::size_t index = 0; index < channels_.size(); ++index) {
            const DWORD access = PIPE_ACCESS_OUTBOUND | (index == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
            channels_[index].open(name, access, bufferSize, attributes);
        }
        wantsFrame();
    }

    bool wantsFrame() {
        bool ready = false;
        for (auto& channel : channels_) {
            channel.poll();
            ready |= channel.ready();
        }
        return ready;
    }

    std::shared_ptr<Packet> acquireFrameBuffer() {
        // A pending pipe write must keep its bytes intact until completion.
        if (!buffer_ || buffer_.use_count() != 1) buffer_ = std::make_shared<Packet>();
        return buffer_;
    }

    void publish(const std::shared_ptr<const Packet>& frame) {
        if (!frame || frame->empty() || frame->size() > std::numeric_limits<DWORD>::max()) return;
        for (auto& channel : channels_) {
            if (channel.ready()) channel.send(frame);
        }
    }

private:
    std::array<Channel, capacity> channels_;
    std::shared_ptr<Packet> buffer_;
};

} // namespace muxivo

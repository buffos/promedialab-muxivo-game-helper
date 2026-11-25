#pragma once

#include <chrono>
#include <optional>

namespace muxivo {

class CaptureCadence {
public:
    using Clock = std::chrono::steady_clock;

    bool admit(Clock::time_point now, bool readerReady) {
        if (!readerReady || (deadline_ && now + jitter < *deadline_)) return false;
        // Keep the phase when Present is a little early or late. After a stall,
        // resume from now instead of trying to take frames that were missed.
        if (!deadline_ || now >= *deadline_ + interval) {
            deadline_ = now + interval;
        } else {
            *deadline_ += interval;
        }
        return true;
    }

private:
    static constexpr auto interval = std::chrono::nanoseconds(1'000'000'000) / 60;
    static constexpr auto jitter = std::chrono::milliseconds(1);
    std::optional<Clock::time_point> deadline_;
};

} // namespace muxivo

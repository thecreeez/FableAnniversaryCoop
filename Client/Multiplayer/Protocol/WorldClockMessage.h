#pragma once
#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    struct WorldClockMessage final
    {
        std::uint64_t epoch = 0;
        std::uint64_t revision = 0;
        std::uint64_t sampledAt = 0;
        float fraction = 0, previous = 0, rate = 0, accumulated = 0;
        float target = 0, remaining = 0;
        std::int32_t day = 0;
        // frozen bit0: native freeze tokens; bit1: world update stopped (inventory/pause).
        std::uint8_t restricted = 0, restriction = 0, jumping = 0, frozen = 0;
    };
    inline constexpr std::size_t WorldClockBytes = 56;
    bool EncodeWorldClock(const WorldClockMessage&, std::uint8_t*, std::size_t) noexcept;
    bool DecodeWorldClock(const std::uint8_t*, std::size_t, WorldClockMessage&) noexcept;
}

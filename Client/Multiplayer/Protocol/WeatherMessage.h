#pragma once
#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    struct WeatherMessage final
    {
        std::uint64_t epoch = 0, revision = 0, sampledAt = 0, worldFence = 0;
        std::uint32_t actorGeneration = 0, mapEpoch = 0;
        std::uint16_t map = 0;
        std::uint8_t enabled = 0;
        float rain = 0, snow = 0, mist = 0;
    };
    inline constexpr std::size_t WeatherBytes = 56;
    bool EncodeWeather(const WeatherMessage&, std::uint8_t*, std::size_t) noexcept;
    bool DecodeWeather(const std::uint8_t*, std::size_t, WeatherMessage&) noexcept;
}

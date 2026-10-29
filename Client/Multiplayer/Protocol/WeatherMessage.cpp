#include "WeatherMessage.h"
#include <cmath>
#include <cstring>

namespace fable::multiplayer::protocol
{
    namespace
    {
        bool Valid(const WeatherMessage& s) noexcept
        {
            const auto strength = [](float v) { return std::isfinite(v) && v >= 0 && v <= 16; };
            return s.epoch && s.revision && s.actorGeneration && s.mapEpoch && s.map &&
                !(s.enabled & ~7u) && strength(s.rain) && strength(s.snow) && strength(s.mist);
        }
    }
    bool EncodeWeather(const WeatherMessage& s, std::uint8_t* bytes, std::size_t size) noexcept
    {
        if (!bytes || size != WeatherBytes || !Valid(s)) return false;
        std::memcpy(bytes, &s.epoch, 8); std::memcpy(bytes + 8, &s.revision, 8);
        std::memcpy(bytes + 16, &s.sampledAt, 8); std::memcpy(bytes + 24, &s.worldFence, 8);
        std::memcpy(bytes + 32, &s.actorGeneration, 4); std::memcpy(bytes + 36, &s.mapEpoch, 4);
        std::memcpy(bytes + 40, &s.map, 2); bytes[42] = s.enabled; bytes[43] = 0;
        std::memcpy(bytes + 44, &s.rain, 4); std::memcpy(bytes + 48, &s.snow, 4); std::memcpy(bytes + 52, &s.mist, 4);
        return true;
    }
    bool DecodeWeather(const std::uint8_t* bytes, std::size_t size, WeatherMessage& output) noexcept
    {
        if (!bytes || size != WeatherBytes || bytes[43]) return false;
        WeatherMessage s;
        std::memcpy(&s.epoch, bytes, 8); std::memcpy(&s.revision, bytes + 8, 8);
        std::memcpy(&s.sampledAt, bytes + 16, 8); std::memcpy(&s.worldFence, bytes + 24, 8);
        std::memcpy(&s.actorGeneration, bytes + 32, 4); std::memcpy(&s.mapEpoch, bytes + 36, 4);
        std::memcpy(&s.map, bytes + 40, 2); s.enabled = bytes[42];
        std::memcpy(&s.rain, bytes + 44, 4); std::memcpy(&s.snow, bytes + 48, 4); std::memcpy(&s.mist, bytes + 52, 4);
        if (!Valid(s)) return false;
        output = s; return true;
    }
}

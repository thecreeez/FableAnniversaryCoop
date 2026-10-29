#include "WorldClockMessage.h"
#include <cmath>
#include <cstring>

namespace fable::multiplayer::protocol
{
    namespace
    {
        bool Valid(const WorldClockMessage& s) noexcept
        {
            return s.epoch != 0 && s.revision != 0 && s.sampledAt != 0 && s.day >= 0 &&
                s.day < 100000000 && std::isfinite(s.fraction) && s.fraction >= 0 && s.fraction <= 1 &&
                std::isfinite(s.previous) && s.previous >= 0 && s.previous <= 1 &&
                std::isfinite(s.rate) && s.rate >= 0 && s.rate <= 1 &&
                std::isfinite(s.accumulated) && s.accumulated >= 0 && s.accumulated <= 1 &&
                std::isfinite(s.target) && s.target >= -1 && s.target <= 1 &&
                std::isfinite(s.remaining) && s.remaining >= 0 && s.remaining <= 86400 &&
                s.restricted <= 1 && s.restriction <= 1 && s.jumping <= 1 && s.frozen <= 3;
        }
    }
    bool EncodeWorldClock(const WorldClockMessage& s, std::uint8_t* bytes, const std::size_t size) noexcept
    {
        if (!bytes || size < WorldClockBytes || !Valid(s)) return false;
        std::size_t offset = 0;
#define WRITE(field) std::memcpy(bytes + offset, &s.field, sizeof(s.field)); offset += sizeof(s.field)
        WRITE(epoch); WRITE(revision); WRITE(sampledAt);
        WRITE(fraction); WRITE(previous); WRITE(rate); WRITE(accumulated); WRITE(target); WRITE(remaining);
        WRITE(day); WRITE(restricted); WRITE(restriction); WRITE(jumping); WRITE(frozen);
#undef WRITE
        return offset == WorldClockBytes;
    }
    bool DecodeWorldClock(const std::uint8_t* bytes, const std::size_t size, WorldClockMessage& s) noexcept
    {
        s = {};
        if (!bytes || size != WorldClockBytes) return false;
        std::size_t offset = 0;
#define READ(field) std::memcpy(&s.field, bytes + offset, sizeof(s.field)); offset += sizeof(s.field)
        READ(epoch); READ(revision); READ(sampledAt);
        READ(fraction); READ(previous); READ(rate); READ(accumulated); READ(target); READ(remaining);
        READ(day); READ(restricted); READ(restriction); READ(jumping); READ(frozen);
#undef READ
        if (Valid(s)) return true;
        s = {};
        return false;
    }
}

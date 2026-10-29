#include "EffectMessage.h"
#include <cmath>
#include <cstring>

namespace fable::multiplayer::protocol
{
    bool ValidEffectMessage(const EffectMessage& s) noexcept
    {
        const auto coordinate = [](float v) { return std::isfinite(v) && std::fabs(v) <= 1000000; };
        return s.actor && s.instance && s.revision && s.authorityEpoch && s.generation && s.mapEpoch && s.map &&
            s.particle > 0 && s.particle < 1000000 && s.flags <= 0xFFFF &&
            s.slot >= -1 && s.slot <= 1000000 && std::isfinite(s.extra) && std::fabs(s.extra) <= 1000000 &&
            s.kind <= 1 && std::isfinite(s.strength) && std::fabs(s.strength) <= 1000000 &&
            s.operation >= EffectOperation::Start && s.operation <= EffectOperation::Impact &&
            s.attachment <= EffectAttachment::World && std::memchr(s.bone, 0, sizeof(s.bone)) &&
            coordinate(s.position.x) && coordinate(s.position.y) && coordinate(s.position.z) &&
            (s.kind == 0 || s.operation == EffectOperation::Impact) &&
            ((s.operation == EffectOperation::Impact) == (s.attachment == EffectAttachment::World));
    }
    bool DecodeEffectMessage(const std::uint8_t* bytes, std::size_t size, EffectMessage& output) noexcept
    {
        if (!bytes || size != sizeof(EffectMessage)) return false;
        EffectMessage s; std::memcpy(&s, bytes, sizeof(s));
        if (!ValidEffectMessage(s)) return false;
        output = s; return true;
    }
}

#pragma once
#include "Game/Math/Vector3.h"
#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    enum class EffectOperation : std::uint8_t { Start = 1, Stop, Impact };
    enum class EffectAttachment : std::uint8_t { Hero = 0, Melee, Ranged, World };
#pragma pack(push, 1)
    struct EffectMessage final
    {
        std::uint64_t actor = 0, instance = 0, revision = 0, sampledAt = 0, worldFence = 0;
        std::uint32_t authorityEpoch = 0, generation = 0, mapEpoch = 0;
        std::uint32_t particle = 0, flags = 0;
        std::int32_t slot = 0;
        float extra = 0;
        std::uint32_t kind = 0, secondary = 0;
        float strength = 0;
        std::uint16_t map = 0;
        EffectAttachment attachment = EffectAttachment::Hero;
        EffectOperation operation = EffectOperation::Start;
        game::Vector3 position;
        char bone[64] = {};
    };
#pragma pack(pop)
    static_assert(sizeof(EffectMessage) == 160);
    bool ValidEffectMessage(const EffectMessage&) noexcept;
    bool DecodeEffectMessage(const std::uint8_t*, std::size_t, EffectMessage&) noexcept;
}

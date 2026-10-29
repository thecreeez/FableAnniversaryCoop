#pragma once
#include <cstdint>

namespace fable::game::creature::animation::native
{
    struct AnimationPlaybackRequest final
    {
        std::int32_t channel = 0;
        std::uint8_t immediate = 0;
        std::uint8_t option = 0;
        std::uint16_t reserved = 0;
        std::int32_t animationId = 0;
        float playbackRate = 1.0f;
        float blendIn = 1.0f;
        float blendOut = 1.0f;
        float startTime = 0.0f;
        float endTime = 0.0f;
        std::uint32_t flags = 0;
        std::int32_t variant = 0;
        std::int32_t priority = 100;
    };

    static_assert(
        sizeof(AnimationPlaybackRequest) == 0x2C,
        "Unexpected Fable animation request layout.");

}

#pragma once
#include "Game/Math/Vector3.h"
#include <atomic>
#include <cstdint>

namespace fable::game::persistence
{
    // A committed co-op checkpoint already contains the host's matching world sections.
    // During its load, current-session overlays must not replace the older checkpoint data.
    inline std::atomic_bool pairedCheckpointLoad{false};
    struct CheckpointHeroProjection final
    {
        std::uint64_t uid = 0;
        std::uint16_t map = 0;
        Vector3 position;
    };
    inline thread_local const CheckpointHeroProjection* checkpointHeroProjection = nullptr;
}

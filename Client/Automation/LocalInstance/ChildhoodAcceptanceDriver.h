#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Math/Vector3.h"
#include <cstdint>

namespace fable::game { class GameServiceRuntime; }
namespace fable::automation::local_instance
{
    // Observes a genuine New Game; never writes quest flags or life stage.
    class ChildhoodAcceptanceDriver final
    {
    public:
        void Initialize(game::GameServiceRuntime* services,
            const core::Diagnostics& diagnostics) noexcept;
        bool ProcessGameThreadIdle() noexcept;
        void Shutdown() noexcept;
    private:
        game::GameServiceRuntime* services_ = nullptr;
        core::Diagnostics diagnostics_;
        game::Vector3 origin_;
        std::uint64_t nextPoll_ = 0, saveAt_ = 0;
        unsigned phase_ = 0;
    };
}

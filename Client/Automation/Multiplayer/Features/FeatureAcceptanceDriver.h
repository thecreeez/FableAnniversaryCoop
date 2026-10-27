#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <atomic>
#include <cstdint>

namespace fable::multiplayer
{
    class MultiplayerRuntimeGraph;
}

namespace fable::automation::multiplayer::features
{
    // Drives the co-op feature checks on the host; the guest only observes.
    // The host performs, through the same native paths gameplay uses, a roll,
    // a block, a strength-experience gain, a kill-experience grant and a
    // quest card; the launcher scenario then requires the guest to replay
    // each one (remote Hero roll/block, 0.3x experience, the quest card).
    // Every native call runs at the game-thread idle boundary.
    class FeatureAcceptanceDriver final
    {
    public:
        void Initialize(
            bool enabled,
            bool host,
            ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer,
            const core::Diagnostics& diagnostics) noexcept;
        void Tick(bool remotePresentationReady) noexcept;
        bool ProcessGameThreadIdle() noexcept;
        void Shutdown() noexcept;

    private:
        enum class Phase : std::uint8_t
        {
            WaitingForWorld,
            Settle,
            Roll,
            Block,
            StatExperience,
            KillExperience,
            QuestCard,
            Done,
            Failed,
        };

        static constexpr std::uint64_t SettleMilliseconds = 10'000;
        static constexpr std::uint64_t StepMilliseconds = 4'000;
        static constexpr std::int32_t ExperienceAmount = 100;

        void Enter(Phase phase, std::uint64_t now) noexcept;
        void Fail(const char* reason) noexcept;

        ::fable::multiplayer::MultiplayerRuntimeGraph* multiplayer_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::atomic<Phase> phase_{Phase::WaitingForWorld};
        std::uint64_t phaseStartedAt_ = 0;
        bool host_ = false;
        bool enabled_ = false;
    };
}

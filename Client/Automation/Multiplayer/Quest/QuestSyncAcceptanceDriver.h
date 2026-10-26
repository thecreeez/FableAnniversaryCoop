#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fable::multiplayer
{
    class MultiplayerRuntimeGraph;
}

namespace fable::automation::multiplayer::quest
{
    // Proves shared quest progress in both directions with real game clients:
    // the host starts a quest through the retail CQuestManager entry point and
    // the guest must see it active; the guest then completes it the same way
    // and the host must see it completed. Every native call runs at the
    // game-thread idle boundary.
    class QuestSyncAcceptanceDriver final
    {
    public:
        static constexpr std::size_t CandidateCount = 5;

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
            Baseline,
            HostActivate,
            GuestAwaitActive,
            GuestComplete,
            HostAwaitCompleted,
            HostWriteField,
            GuestAwaitField,
            HostAwaitField,
            HostWriteWorldFlag,
            GuestAwaitWorldFlag,
            Done,
            Failed,
        };

        static constexpr std::uint64_t HostActivateDelayMilliseconds = 10'000;
        static constexpr std::uint64_t PollMilliseconds = 500;
        static constexpr std::uint64_t PhaseTimeoutMilliseconds = 90'000;

        void Fail(const char* reason) noexcept;
        void Enter(Phase phase, std::uint64_t now) noexcept;

        ::fable::multiplayer::MultiplayerRuntimeGraph* multiplayer_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::atomic<Phase> phase_{Phase::WaitingForWorld};
        std::uint64_t phaseStartedAt_ = 0;
        std::uint64_t nextPollAt_ = 0;
        bool baselineActive_[CandidateCount] = {};
        bool baselineCompleted_[CandidateCount] = {};
        std::size_t chosen_ = CandidateCount;
        std::size_t completionTarget_ = CandidateCount;
        bool completionRequested_ = false;
        bool deactivationRequested_ = false;
        std::uint64_t lastProbeAt_ = 0;
        bool host_ = false;
        bool enabled_ = false;
    };
}

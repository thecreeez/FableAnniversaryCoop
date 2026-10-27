#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <atomic>
#include <cstdint>

namespace fable::game
{
    class EntityService;
}

namespace fable::multiplayer
{
    class MultiplayerRuntimeGraph;
}

namespace fable::automation::multiplayer::features
{
    // Shared cutscene acceptance on the wasp fixture (both Heroes at Lookout
    // Point, next to the Picnic Area). The host enters the Picnic Area, whose
    // quest cutscene is held with the waiting box; the driver then dismisses
    // the box the way a player's click does, which must lend control back and
    // move the host back to Lookout Point. The host enters again, the guest
    // follows once it has seen that, and the cutscene must play on both.
    class CutsceneAcceptanceDriver final
    {
    public:
        void Initialize(
            bool enabled,
            bool host,
            game::EntityService& entities,
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
            HostEnter,
            HostAwaitPrompt,
            HostAwaitRetreat,
            HostReenter,
            GuestWatchHost,
            GuestEnter,
            Done,
            Failed,
        };

        static constexpr std::uint64_t SettleMilliseconds = 10'000;
        static constexpr std::uint64_t PromptShownMilliseconds = 3'000;
        static constexpr std::uint64_t PhaseTimeoutMilliseconds = 90'000;
        static constexpr std::uint64_t TravelRetryMilliseconds = 1'000;
        static constexpr std::uint32_t TravelAttemptLimit = 15;

        void Enter(Phase phase, std::uint64_t now) noexcept;
        void Fail(const char* reason) noexcept;
        // Moves the local Hero through the region exit to the named map.
        // Returns true once the Hero is there.
        bool TravelTo(const char* mapName, std::uint64_t now) noexcept;
        [[nodiscard]] std::uint16_t LocalMapId() const noexcept;
        [[nodiscard]] bool LocalMapIs(const char* mapName) const noexcept;

        game::EntityService* entities_ = nullptr;
        ::fable::multiplayer::MultiplayerRuntimeGraph* multiplayer_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::atomic<Phase> phase_{Phase::WaitingForWorld};
        std::uint64_t phaseStartedAt_ = 0;
        std::uint64_t nextTravelAt_ = 0;
        std::uint32_t travelAttempts_ = 0;
        std::uint16_t picnicMapId_ = 0;
        std::uint16_t lookoutMapId_ = 0;
        // Guest: host map visits observed (picnic, lookout, picnic again).
        int hostVisits_ = 0;
        std::uint16_t lastHostMap_ = 0;
        std::uint64_t promptSeenAt_ = 0;
        bool host_ = false;
        bool enabled_ = false;
    };
}

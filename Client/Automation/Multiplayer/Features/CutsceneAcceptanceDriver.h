#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <atomic>
#include <vector>
#include <cstdint>

namespace fable::game
{
    class EntityService;
    class PlayerService;
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
    // Then enemies, keeping the players far apart and only ever moving them
    // to spots a wasp or a player occupies: the host is moved to a wasp it
    // simulates until it is hit; the wasp is moved to the guest until the
    // guest is hit; the guest then moves to the host and the host to the
    // wasp, which must attack the host again.
    class CutsceneAcceptanceDriver final
    {
    public:
        void Initialize(
            bool enabled,
            bool host,
            game::EntityService& entities,
            game::PlayerService& players,
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
            AwaitCutsceneEnd,
            HostToWasp,
            HostAwaitWaspAttack,
            HostAwaitGuestReady,
            HostAwaitGuestAttacked,
            HostAwaitGuestArrival,
            HostAwaitGuestDeparture,
            HostAwaitWaspAttackAgain,
            GuestAwaitHostArrival,
            GuestAwaitHostRetreat,
            GuestAwaitHostReturn,
            GuestAwaitHit,
            GuestFlaskProbe,
            GuestAwaitFlaskRecovery,
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
        bool TeleportLocalHero(const float (&position)[3], const char* reason) noexcept;
        // Host: the nearest wasp this process simulates; false if none.
        bool FindWasp(std::uint64_t& uid, float (&position)[3]) noexcept;
        bool FindCorpseSwarm(const float (&nearby)[3], float (&position)[3]) noexcept;
        bool SwarmDamagedPlayer(std::uint64_t actor) const noexcept;
        std::vector<std::uint64_t> swarmUids_;
        float swarmCenter_[3] = {};
        bool swarmSelected_ = false;
        bool WaspPosition(float (&position)[3]) noexcept;
        // Guest: the wasp nearest a point (the host's last fighting spot).
        bool FindWaspNear(const float (&nearby)[3], std::uint64_t& uid, float (&position)[3]) noexcept;
        // True once the local Hero lost health since the baseline; heals it.
        bool LocalHeroWasHit() noexcept;
        [[nodiscard]] float DistanceToRemote() const noexcept;
        [[nodiscard]] bool RemoteHost(std::uint64_t& actorId, float (&position)[3]) const noexcept;

        game::EntityService* entities_ = nullptr;
        game::PlayerService* players_ = nullptr;
        float healthBaseline_ = 0.0f;
        float retreatAt_[3] = {};
        int flaskBaseline_ = -1;
        std::uint64_t orbProbeUid_ = 0, orbCollectAt_ = 0;
        bool orbReplicaSeen_ = false, orbReplicaRemoved_ = false;
        std::uint64_t balancedSince_ = 0;
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
        bool sawCutscene_ = false;
        bool skipRequestedForTest_ = false;
        std::uint64_t waspUid_ = 0;
        std::uint64_t nextWaspTeleportAt_ = 0;
        // Guest: where the host was while fighting away from the guest.
        float hostFightAt_[3] = {};
        bool hostFightSeen_ = false;

        bool host_ = false;
        bool enabled_ = false;
    };
}

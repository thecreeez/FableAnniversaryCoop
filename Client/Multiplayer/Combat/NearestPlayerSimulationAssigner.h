#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Multiplayer/Protocol/CombatHitMessage.h"
#include "Multiplayer/Protocol/PlayerState.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fable::game
{
    class EntityService;
}

namespace fable::multiplayer::authority
{
    class AuthorityReplication;
}

namespace fable::multiplayer::entities
{
    class EntityLifecycleReplication;
    class EntityPresenceReplication;
}

namespace fable::multiplayer::replication
{
    class LocalHeroReplication;
    class RemotePlayerChannels;
}

namespace fable::multiplayer::combat
{
    class PlayerCombatantDirectory;

    // Host: every NPC is simulated by the nearest player on its map, so its
    // retail AI perceives that player (the native AI only sees its local
    // Hero). An NPC damaged from range goes to the attacker for a while, so a
    // ranged player can pull aggro off a melee player.
    class NearestPlayerSimulationAssigner final
    {
    public:
        void Initialize(
            PeerRole role,
            game::EntityService& entities,
            authority::AuthorityReplication& authority,
            entities::EntityLifecycleReplication& lifecycle,
            entities::EntityPresenceReplication& presence,
            PlayerCombatantDirectory& combatants,
            replication::LocalHeroReplication& localHero,
            replication::RemotePlayerChannels& remotePlayers,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        void Process();
        // Game thread: a hit applied anywhere in the session.
        static void ObserveAppliedHit(void* context, const protocol::CombatHitMessage& hit);

    private:
        static constexpr std::uint64_t ProcessIntervalMilliseconds = 500;
        static constexpr std::uint64_t RemoteStateFreshMilliseconds = 3'000;
        static constexpr std::uint64_t MinimumHoldMilliseconds = 1'500;
        static constexpr std::uint64_t RangedAggroMilliseconds = 10'000;
        static constexpr float SwitchMargin = 4.0f;
        static constexpr float RangedDistance = 5.0f;

        struct Player final
        {
            std::uint64_t actorId = 0;
            std::uint16_t mapId = 0;
            game::Vector3 position = {};
        };
        struct Aggro final
        {
            std::uint64_t actorId = 0;
            std::uint64_t until = 0;
        };
        struct Candidate final
        {
            std::uint64_t actorId = 0;
            std::uint64_t since = 0;
            std::uint32_t generation = 0;
            std::uint32_t mapEpoch = 0;
        };
        std::unordered_map<std::uint64_t, Candidate> candidates_;
        struct PendingHit final
        {
            std::uint64_t sourceActorId = 0;
            std::uint64_t targetUid = 0;
        };

        [[nodiscard]] std::vector<Player> CollectPlayers(std::uint64_t now) const;
        void ApplyRangedHits(const std::vector<Player>& players, std::uint64_t now);

        PeerRole role_ = PeerRole::Guest;
        game::EntityService* entities_ = nullptr;
        authority::AuthorityReplication* authority_ = nullptr;
        entities::EntityLifecycleReplication* lifecycle_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        PlayerCombatantDirectory* combatants_ = nullptr;
        replication::LocalHeroReplication* localHero_ = nullptr;
        replication::RemotePlayerChannels* remotePlayers_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::uint64_t nextProcessAt_ = 0;
        std::unordered_map<std::uint64_t, Aggro> aggro_;
        std::unordered_map<std::uint64_t, std::uint64_t> lastChangedAt_;
        std::vector<PendingHit> pendingHits_;
    };
}

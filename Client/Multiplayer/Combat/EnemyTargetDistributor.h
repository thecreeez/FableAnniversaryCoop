#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include "Multiplayer/Protocol/PlayerState.h"
#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Math/Vector3.h"
#include "CombatAggressionState.h"

#include <cstdint>
#include <unordered_map>

namespace fable::game
{
    class EntityService;
}

namespace fable::multiplayer
{
    class UdpPeer;
    namespace authority
    {
        class EntitySimulationAuthority;
    }
    namespace entities
    {
        class EntityPresenceReplication;
        class EntityLifecycleReplication;
        class EntityNetworkIdentityRegistry;
    }
    namespace replication
    {
        class EntityActionReplication;
        class LocalHeroReplication;
        class RemotePlayerChannels;
    }
}

namespace fable::multiplayer::protocol { struct CombatHitMessage; }

namespace fable::multiplayer::combat
{
    class PlayerCombatantDirectory;

    // Owner-only targeting: living players in range, balanced swarm pressure,
    // sticky distance choices and damage-driven revenge. Script choreography
    // retains ownership of its own target override. Game thread only.
    class EnemyTargetDistributor final : public ReliableMessageSink
    {
    public:
        ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        { static constexpr protocol::PacketType types[] = {protocol::PacketType::EnemyTarget}; return {types, 1}; }
        bool HandleReliableMessage(const TransportMessage& message) override;
        void Initialize(
            PeerRole role, UdpPeer& transport,
            entities::EntityLifecycleReplication& lifecycle,
            entities::EntityNetworkIdentityRegistry& identities,
            game::EntityService& entities,
            replication::EntityActionReplication& entityActions,
            authority::EntitySimulationAuthority& simulation,
            entities::EntityPresenceReplication& presence,
            PlayerCombatantDirectory& combatants,
            replication::LocalHeroReplication& localHero,
            replication::RemotePlayerChannels& remotePlayers,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        void Process();
        std::size_t NativeTargetCount(std::uint64_t actor) const noexcept
        {
            std::size_t count = 0;
            for (const auto& entry : nativeAssignments_) if (entry.second.actorId == actor && entry.second.readReported) ++count;
            return count;
        }
        static void ObserveAppliedHit(void* context, const protocol::CombatHitMessage& hit);

    private:
        static constexpr std::uint64_t ProcessIntervalMilliseconds = 500;
        static constexpr std::uint64_t EngagementMilliseconds = 10'000;
        static constexpr std::uint64_t RemoteStateFreshMilliseconds = 3'000;
        // Another player must be this much nearer before an enemy switches.
        static constexpr float SwitchMargin = 4.0f;
        static constexpr std::uint64_t ReassertMilliseconds = 2'000;

        struct Assignment final
        {
            void* creature = nullptr;
            void* targetCreature = nullptr;
            std::uint64_t actorId = 0;
            bool overridden = false;
            std::uint64_t appliedAt = 0;
            std::uint64_t selectedAt = 0;
            std::uint32_t incarnation = 0;
            std::uint32_t generation = 0, mapEpoch = 0;
            std::uint16_t mapId = 0;
            std::uint64_t revision = 0, publishedAt = 0;
            bool readReported = false;
            int targetEnemyRelation = -2;
            int localEnemyRelation = -2;
            bool addedEnemy = false;
            std::uint64_t targetUid = 0;
            std::uint64_t playerConnectionNonce = 0;
            std::uint32_t playerAuthorityEpoch = 0, playerGeneration = 0,
                playerMapEpoch = 0;
        };

        struct Threat { float damage = 0; std::uint64_t at = 0; };
        struct Revenge
        {
            std::uint32_t generation = 0, mapEpoch = 0;
            std::uint64_t revision = 0, actor = 0, lockedUntil = 0, lastHit = 0;
            float maximumHealth = 0;
            std::unordered_map<std::uint64_t, Threat> attackers;
        };
        std::unordered_map<std::uint64_t, Revenge> revenge_;
        CombatAggressionState aggression_;
        std::uint64_t SourceConnectionNonce(std::uint64_t actor) const noexcept;
        void Release(std::uint64_t thingUid, Assignment& assignment) noexcept;

        void ApplyPlans(std::uint64_t now);
        #pragma pack(push, 1)
        struct Plan
        {
            std::uint64_t uid = 0, actor = 0, revision = 0;
            std::uint32_t generation = 0, mapEpoch = 0;
            std::uint16_t mapId = 0;
        };
        #pragma pack(pop)
        struct ReceivedPlan { Plan plan; std::uint64_t at; };
        PeerRole role_ = PeerRole::Guest;
        UdpPeer* transport_ = nullptr;
        entities::EntityLifecycleReplication* lifecycle_ = nullptr;
        entities::EntityNetworkIdentityRegistry* identities_ = nullptr;
        std::uint64_t nextRevision_ = 0;
        std::unordered_map<std::uint64_t, ReceivedPlan> plans_;
        std::unordered_map<std::uint64_t, Assignment> nativeAssignments_;
        game::EntityService* entities_ = nullptr;
        replication::EntityActionReplication* entityActions_ = nullptr;
        authority::EntitySimulationAuthority* simulation_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        PlayerCombatantDirectory* combatants_ = nullptr;
        replication::LocalHeroReplication* localHero_ = nullptr;
        replication::RemotePlayerChannels* remotePlayers_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::uint64_t nextProcessAt_ = 0;
        std::unordered_map<std::uint64_t, Assignment> assignments_;
    };
}

#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Math/Vector3.h"

#include <cstdint>
#include <unordered_map>

namespace fable::game
{
    class EntityService;
}

namespace fable::multiplayer
{
    namespace authority
    {
        class EntitySimulationAuthority;
    }
    namespace entities
    {
        class EntityPresenceReplication;
    }
    namespace replication
    {
        class EntityActionReplication;
        class LocalHeroReplication;
        class RemotePlayerChannels;
    }
}

namespace fable::multiplayer::combat
{
    class PlayerCombatantDirectory;

    // Enemies attack every player, not just the map owner. Fable's AI picks
    // its target among creatures it perceives, and on the map owner that is
    // effectively the local Hero. For each NPC the owner simulates that is
    // engaged with a player, aim it at the nearest player on the map through
    // CTCTargetingAI's script target override (a remote Hero's presentation
    // creature, whose hits the combat replication forwards to that player),
    // and hand it back to retail targeting when the local Hero is nearest.
    // Game thread.
    class EnemyTargetDistributor final
    {
    public:
        void Initialize(
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
            std::uint64_t actorId = 0;
            bool overridden = false;
            std::uint64_t appliedAt = 0;
        };

        void Release(std::uint64_t thingUid, Assignment& assignment) noexcept;

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

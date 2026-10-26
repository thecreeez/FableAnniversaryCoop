#include "EnemyTargetDistributor.h"

#include "Game/Creature/Combat/Native/AiTargetingComponent.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Multiplayer/Authority/EntitySimulationAuthority.h"
#include "Multiplayer/Combat/PlayerCombatantDirectory.h"
#include "Multiplayer/Entities/EntityPresenceReplication.h"
#include "Multiplayer/Entities/LiveEntityRegistry.h"
#include "Multiplayer/Replication/EntityActionReplication.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    struct Candidate final
    {
        std::uint64_t actorId = 0;
        void* creature = nullptr;
        fable::game::Vector3 position = {};
        bool local = false;
    };

    float PlanarDistance(const fable::game::Vector3& a, const fable::game::Vector3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }
}

namespace fable::multiplayer::combat
{
    void EnemyTargetDistributor::Initialize(
        game::EntityService& entities,
        replication::EntityActionReplication& entityActions,
        authority::EntitySimulationAuthority& simulation,
        entities::EntityPresenceReplication& presence,
        PlayerCombatantDirectory& combatants,
        replication::LocalHeroReplication& localHero,
        replication::RemotePlayerChannels& remotePlayers,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        entities_ = &entities;
        entityActions_ = &entityActions;
        simulation_ = &simulation;
        presence_ = &presence;
        combatants_ = &combatants;
        localHero_ = &localHero;
        remotePlayers_ = &remotePlayers;
        diagnostics_ = diagnostics;
    }

    void EnemyTargetDistributor::Shutdown() noexcept
    {
        for (auto& [uid, assignment] : assignments_)
        {
            Release(uid, assignment);
        }
        assignments_.clear();
        entities_ = nullptr;
        entityActions_ = nullptr;
        simulation_ = nullptr;
        presence_ = nullptr;
        combatants_ = nullptr;
        localHero_ = nullptr;
        remotePlayers_ = nullptr;
        nextProcessAt_ = 0;
    }

    void EnemyTargetDistributor::Release(const std::uint64_t thingUid, Assignment& assignment) noexcept
    {
        if (!assignment.overridden || assignment.creature == nullptr || entities_ == nullptr) return;
        // Only touch a creature that is still the same live Thing.
        const entities::LiveEntityRecord* const live = presence_ != nullptr
            ? presence_->LiveEntities().Find(thingUid) : nullptr;
        if (live != nullptr && live->thing == assignment.creature)
        {
            void* const targeting = game::entity::native::ThingComponentAccess::Find(
                assignment.creature, game::entity::native::ThingComponentType::Targeting);
            (void)game::creature::combat::native::AiTargetingComponent::SetScriptTargetOverride(
                entities_->GameModule(), targeting, nullptr);
        }
        assignment.overridden = false;
    }

    void EnemyTargetDistributor::Process()
    {
        if (entities_ == nullptr || entityActions_ == nullptr || localHero_ == nullptr) return;
        const std::uint64_t now = GetTickCount64();
        if (now < nextProcessAt_) return;
        nextProcessAt_ = now + ProcessIntervalMilliseconds;

        const PlayerState* const local = localHero_->CurrentState();
        void* const localCreature = localHero_->NativeHero();
        if (local == nullptr || localCreature == nullptr || local->mapId == 0) return;

        std::vector<Candidate> candidates;
        candidates.push_back({local->actorId, localCreature, local->position, true});
        for (const replication::RemotePlayerSnapshot& remote : remotePlayers_->Snapshots())
        {
            const PlayerState& state = remote.state;
            if (state.mapId != local->mapId || remote.receivedAt == 0 ||
                now - remote.receivedAt > RemoteStateFreshMilliseconds)
            {
                continue;
            }
            void* const creature = combatants_->FindCreature(state.actorId);
            if (creature != nullptr) candidates.push_back({state.actorId, creature, state.position, false});
        }

        const std::vector<replication::EntityActionReplication::Engagement> engaged =
            entityActions_->EngagedCreatures(now, EngagementMilliseconds);
        std::unordered_map<std::uint64_t, Assignment> next;
        for (const auto& engagement : engaged)
        {
            const entities::LiveEntityRecord* const live = presence_->LiveEntities().Find(engagement.thingUid);
            if (live == nullptr || live->thing != engagement.creature || !live->creature ||
                !simulation_->IsLocallySimulated(live->thing))
            {
                continue;
            }
            game::Entity* const entity = entities_->FindByUid(engagement.thingUid);
            if (entity == nullptr) continue;
            const bool valid = entity->IsValid();
            const game::Vector3 position = valid ? entity->GetPosition() : game::Vector3{};
            entity->Release();
            if (!valid || !std::isfinite(position.x) || !std::isfinite(position.y)) continue;

            Assignment assignment;
            const auto previous = assignments_.find(engagement.thingUid);
            if (previous != assignments_.end()) assignment = previous->second;
            assignment.creature = live->thing;

            const Candidate* best = nullptr;
            float bestDistance = 0.0f;
            const Candidate* current = nullptr;
            float currentDistance = 0.0f;
            for (const Candidate& candidate : candidates)
            {
                const float distance = PlanarDistance(position, candidate.position);
                if (best == nullptr || distance < bestDistance)
                {
                    best = &candidate;
                    bestDistance = distance;
                }
                if (candidate.actorId == assignment.actorId)
                {
                    current = &candidate;
                    currentDistance = distance;
                }
            }
            const Candidate* chosen = best;
            if (current != nullptr && currentDistance <= bestDistance + SwitchMargin) chosen = current;
            if (chosen == nullptr) continue;

            const bool changed = chosen->actorId != assignment.actorId;
            assignment.actorId = chosen->actorId;
            void* const targeting = game::entity::native::ThingComponentAccess::Find(
                live->thing, game::entity::native::ThingComponentType::Targeting);
            if (chosen->local)
            {
                // Retail targeting already prefers the local Hero.
                if (assignment.overridden) Release(engagement.thingUid, assignment);
            }
            else if (changed || !assignment.overridden || now - assignment.appliedAt >= ReassertMilliseconds)
            {
                assignment.overridden = game::creature::combat::native::AiTargetingComponent::
                    SetScriptTargetOverride(entities_->GameModule(), targeting, chosen->creature);
                assignment.appliedAt = now;
            }
            if (changed)
            {
                char detail[192] = {};
                std::snprintf(detail, sizeof(detail), "thing_uid=%016llX target_actor=%llu local=%d distance=%.1f overridden=%d",
                    static_cast<unsigned long long>(engagement.thingUid),
                    static_cast<unsigned long long>(chosen->actorId), chosen->local ? 1 : 0,
                    PlanarDistance(position, chosen->position), assignment.overridden ? 1 : 0);
                diagnostics_.Event("MultiplayerEnemyTargetAssigned", detail);
            }
            next[engagement.thingUid] = assignment;
        }
        // Enemies no longer engaged go back to retail targeting.
        for (auto& [uid, assignment] : assignments_)
        {
            if (next.find(uid) == next.end()) Release(uid, assignment);
        }
        assignments_.swap(next);
    }
}

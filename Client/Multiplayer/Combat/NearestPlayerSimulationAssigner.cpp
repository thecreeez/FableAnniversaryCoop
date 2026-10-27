#include "NearestPlayerSimulationAssigner.h"

#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Multiplayer/Authority/AuthorityReplication.h"
#include "Multiplayer/Combat/PlayerCombatantDirectory.h"
#include "Multiplayer/Entities/EntityLifecycleReplication.h"
#include "Multiplayer/Entities/EntityPresenceReplication.h"
#include "Multiplayer/Entities/LiveEntityRegistry.h"
#include "Multiplayer/Entities/WorldEntityDirectory.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>

namespace
{
    float PlanarDistance(const fable::game::Vector3& a, const fable::game::Vector3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    bool ReadPosition(fable::game::EntityService& entities, const std::uint64_t uid, fable::game::Vector3& position)
    {
        fable::game::Entity* const entity = entities.FindByUid(uid);
        if (entity == nullptr) return false;
        const bool valid = entity->IsValid();
        position = valid ? entity->GetPosition() : fable::game::Vector3{};
        entity->Release();
        return valid && std::isfinite(position.x) && std::isfinite(position.y);
    }
}

namespace fable::multiplayer::combat
{
    void NearestPlayerSimulationAssigner::Initialize(
        const PeerRole role,
        game::EntityService& entities,
        authority::AuthorityReplication& authority,
        entities::EntityLifecycleReplication& lifecycle,
        entities::EntityPresenceReplication& presence,
        PlayerCombatantDirectory& combatants,
        replication::LocalHeroReplication& localHero,
        replication::RemotePlayerChannels& remotePlayers,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        role_ = role;
        entities_ = &entities;
        authority_ = &authority;
        lifecycle_ = &lifecycle;
        presence_ = &presence;
        combatants_ = &combatants;
        localHero_ = &localHero;
        remotePlayers_ = &remotePlayers;
        diagnostics_ = diagnostics;
    }

    void NearestPlayerSimulationAssigner::Shutdown() noexcept
    {
        role_ = PeerRole::Guest;
        entities_ = nullptr;
        authority_ = nullptr;
        lifecycle_ = nullptr;
        presence_ = nullptr;
        combatants_ = nullptr;
        localHero_ = nullptr;
        remotePlayers_ = nullptr;
        nextProcessAt_ = 0;
        aggro_.clear();
        lastChangedAt_.clear();
        pendingHits_.clear();
    }

    void NearestPlayerSimulationAssigner::ObserveAppliedHit(
        void* const context, const protocol::CombatHitMessage& hit)
    {
        auto* const self = static_cast<NearestPlayerSimulationAssigner*>(context);
        if (self == nullptr || self->role_ != PeerRole::Host ||
            hit.sourceDomain != protocol::CombatActionDomain::Player ||
            hit.targetKind != protocol::CombatParticipantKind::WorldEntity ||
            hit.sourceId == 0 || hit.targetId == 0 || self->pendingHits_.size() >= 256)
        {
            return;
        }
        self->pendingHits_.push_back({hit.sourceId, hit.targetId});
    }

    std::vector<NearestPlayerSimulationAssigner::Player> NearestPlayerSimulationAssigner::CollectPlayers(
        const std::uint64_t now) const
    {
        std::vector<Player> players;
        const PlayerState* const local = localHero_->CurrentState();
        if (local != nullptr && local->mapId != 0 && local->actorId != 0)
        {
            players.push_back({local->actorId, local->mapId, local->position});
        }
        for (const replication::RemotePlayerSnapshot& remote : remotePlayers_->Snapshots())
        {
            if (remote.state.mapId == 0 || remote.state.actorId == 0 || remote.receivedAt == 0 ||
                now - remote.receivedAt > RemoteStateFreshMilliseconds)
            {
                continue;
            }
            players.push_back({remote.state.actorId, remote.state.mapId, remote.state.position});
        }
        return players;
    }

    void NearestPlayerSimulationAssigner::ApplyRangedHits(const std::vector<Player>& players, const std::uint64_t now)
    {
        for (const PendingHit& hit : pendingHits_)
        {
            const Player* attacker = nullptr;
            for (const Player& player : players)
            {
                if (player.actorId == hit.sourceActorId) attacker = &player;
            }
            game::Vector3 target = {};
            if (attacker == nullptr || !ReadPosition(*entities_, hit.targetUid, target)) continue;
            const float distance = PlanarDistance(attacker->position, target);
            if (distance < RangedDistance) continue;
            Aggro& aggro = aggro_[hit.targetUid];
            if (aggro.actorId != hit.sourceActorId)
            {
                char detail[160] = {};
                std::snprintf(detail, sizeof(detail), "thing_uid=%016llX attacker=%llu distance=%.1f",
                    static_cast<unsigned long long>(hit.targetUid),
                    static_cast<unsigned long long>(hit.sourceActorId), distance);
                diagnostics_.Event("MultiplayerNpcRangedAggro", detail);
            }
            aggro = {hit.sourceActorId, now + RangedAggroMilliseconds};
        }
        pendingHits_.clear();
    }

    void NearestPlayerSimulationAssigner::Process()
    {
        if (role_ != PeerRole::Host || entities_ == nullptr || authority_ == nullptr) return;
        const std::uint64_t now = GetTickCount64();
        if (now < nextProcessAt_) return;
        nextProcessAt_ = now + ProcessIntervalMilliseconds;

        const std::vector<Player> players = CollectPlayers(now);
        ApplyRangedHits(players, now);
        if (players.size() < 2)
        {
            return;
        }

        for (const entities::LiveEntityRecord& live : presence_->LiveEntities().Snapshot())
        {
            if (live.thing == nullptr || !live.creature ||
                !entities::LiveEntityRegistry::IsReplicable(live) ||
                entities::LiveEntityRegistry::IsPlayerPresentation(live) ||
                combatants_->FindActor(live.thing) != 0)
            {
                continue;
            }
            const entities::WorldEntityRecord* const world = lifecycle_->Directory().Find(live.thingUid);
            if (world == nullptr || !world->live || !world->available || !world->creature ||
                world->generation == 0 || world->mapEpoch == 0 || world->mapName.empty() ||
                world->mapId != live.mapId)
            {
                continue;
            }
            const authority::EntityAuthorityKey key{world->thingUid, world->generation};
            const std::uint64_t owner = authority_->EntitySimulationOwner(key, world->mapName, world->mapEpoch);
            if (owner == 0) continue;
            game::Vector3 position = {};
            if (!ReadPosition(*entities_, live.thingUid, position)) continue;

            const Player* best = nullptr;
            float bestDistance = 0.0f;
            const Player* current = nullptr;
            float currentDistance = 0.0f;
            const Player* aggressor = nullptr;
            const auto aggro = aggro_.find(world->thingUid);
            const bool aggroLive = aggro != aggro_.end() && now < aggro->second.until;
            for (const Player& player : players)
            {
                if (player.mapId != world->mapId) continue;
                const float distance = PlanarDistance(position, player.position);
                if (best == nullptr || distance < bestDistance)
                {
                    best = &player;
                    bestDistance = distance;
                }
                if (player.actorId == owner)
                {
                    current = &player;
                    currentDistance = distance;
                }
                if (aggroLive && player.actorId == aggro->second.actorId) aggressor = &player;
            }
            if (best == nullptr) continue;

            const Player* chosen = best;
            const char* reason = "nearest";
            if (aggressor != nullptr)
            {
                chosen = aggressor;
                reason = "ranged-aggro";
            }
            else if (current != nullptr &&
                (currentDistance <= bestDistance + SwitchMargin ||
                    now - lastChangedAt_[world->thingUid] < MinimumHoldMilliseconds))
            {
                chosen = current;
            }
            if (chosen->actorId == owner) continue;
            if (!authority_->AssignEntitySimulation(key, world->mapName, world->mapEpoch, chosen->actorId)) continue;
            lastChangedAt_[world->thingUid] = now;
            char detail[224] = {};
            std::snprintf(detail, sizeof(detail), "thing_uid=%016llX script=%s owner=%llu reason=%s distance=%.1f",
                static_cast<unsigned long long>(world->thingUid), live.scriptName.c_str(),
                static_cast<unsigned long long>(chosen->actorId), reason,
                PlanarDistance(position, chosen->position));
            diagnostics_.Event("MultiplayerNpcSimulationHandoff", detail);
        }
        if (aggro_.size() > 512)
        {
            for (auto iterator = aggro_.begin(); iterator != aggro_.end();)
            {
                iterator = now >= iterator->second.until ? aggro_.erase(iterator) : std::next(iterator);
            }
        }
    }
}

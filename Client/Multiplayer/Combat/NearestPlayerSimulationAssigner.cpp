#include "NearestPlayerSimulationAssigner.h"

#include "Game/Entity/Entity.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
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
        candidates_.clear();
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
            const auto* victim = lifecycle_->Directory().Find(hit.targetUid);
            if (attacker == nullptr || victim == nullptr || !victim->hasTransform ||
                victim->mapId != attacker->mapId) continue;
            target = victim->position;
            const float distance = PlanarDistance(attacker->position, target);
            if (distance < RangedDistance) continue;
            Aggro& aggro = aggro_[hit.targetUid];
            if (aggro.actorId != 0 && aggro.actorId != hit.sourceActorId && aggro.until > now)
            {
                bool incumbentPresent = false;
                for (const auto& player : players)
                    if (player.actorId == aggro.actorId && player.mapId == victim->mapId) incumbentPresent = true;
                if (incumbentPresent) continue;
            }
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
        if (role_ != PeerRole::Host || authority_ == nullptr || lifecycle_ == nullptr || presence_ == nullptr) return;
        const auto now = GetTickCount64();
        if (now < nextProcessAt_) return;
        nextProcessAt_ = now + ProcessIntervalMilliseconds;
        pendingHits_.clear();
        const auto* host = localHero_->CurrentState();
        for (const auto& world : lifecycle_->Directory().Snapshot())
        {
            if (!world.live || !world.available || !world.creature || world.mapEpoch == 0) continue;
            const auto* lease = authority_->FindMapLease(world.mapId);
            std::uint64_t owner = lease != nullptr ? lease->actorId : 0;
            // Host simulates its loaded map; another map keeps its stable map
            // owner. Target selection is a separate reliable host-authored plan.
            if (host != nullptr && host->mapId == world.mapId)
            {
                const auto* local = presence_->LiveEntities().Find(world.thingUid);
                if (local == nullptr || local->thing == nullptr) continue;
                owner = host->actorId;
            }
            const authority::EntityAuthorityKey key{world.thingUid, world.generation};
            if (owner != 0 && authority_->EntitySimulationOwner(key, world.mapName, world.mapEpoch) != owner)
                (void)authority_->AssignEntitySimulation(key, world.mapName, world.mapEpoch, owner);
        }
    }
}

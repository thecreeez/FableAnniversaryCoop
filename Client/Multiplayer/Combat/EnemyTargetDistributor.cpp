#include "Multiplayer/Entities/EntityLifecycleReplication.h"
#include "Multiplayer/Entities/EntityNetworkIdentityRegistry.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"
#include <cstring>
#include <unordered_set>
#include "Multiplayer/Protocol/CombatHitMessage.h"
#include "Game/Creature/Control/ScriptControl.h"
#include <algorithm>
#include <limits>
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
    fable::multiplayer::ReliableMessageSink* ResolveEnemyTargets(fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    { return &contexts.actions.enemyTargets; }

    struct Candidate final
    {
        std::uint64_t actorId = 0;
        void* creature = nullptr;
        fable::game::Vector3 position = {};
        bool local = false;
        std::uint16_t mapId = 0;
    };

    bool IsAlive(void* creature) noexcept
    {
        if (creature == nullptr) return false;
        __try
        {
            const float health = *reinterpret_cast<const float*>(static_cast<unsigned char*>(creature) + 0xD0);
            return std::isfinite(health) && health > 0.01f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    float PlanarDistance(const fable::game::Vector3& a, const fable::game::Vector3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkEnemyTargets, 0x1025u,
    "enemy-targets", 813u, "multiplayer-enemy-targets", ResolveEnemyTargets);

namespace fable::multiplayer::combat
{
    void EnemyTargetDistributor::Initialize(
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
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        role_ = role;
        transport_ = &transport;
        lifecycle_ = &lifecycle;
        identities_ = &identities;
        entities_ = &entities;
        entityActions_ = &entityActions;
        simulation_ = &simulation;
        presence_ = &presence;
        combatants_ = &combatants;
        localHero_ = &localHero;
        remotePlayers_ = &remotePlayers;
        diagnostics_ = diagnostics;
    }

    void EnemyTargetDistributor::ObserveAppliedHit(void* context, const protocol::CombatHitMessage& hit)
    {
        auto* self = static_cast<EnemyTargetDistributor*>(context);
        const float damage = hit.healthBefore - hit.healthAfter;
        if (self == nullptr || hit.sourceDomain != protocol::CombatActionDomain::Player ||
            hit.targetKind != protocol::CombatParticipantKind::WorldEntity ||
            hit.sourceId == 0 || hit.targetId == 0 || !std::isfinite(damage) || damage <= 0 ||
            hit.targetGeneration == 0 || hit.targetMapEpoch == 0) return;
        if (self->revenge_.size() >= 8192 && self->revenge_.find(hit.targetId) == self->revenge_.end()) return;
        auto& revenge = self->revenge_[hit.targetId];
        if (revenge.generation != hit.targetGeneration || revenge.mapEpoch != hit.targetMapEpoch)
        {
            revenge = {};
            revenge.generation = hit.targetGeneration;
            revenge.mapEpoch = hit.targetMapEpoch;
        }
        if (hit.hostTargetRevision <= revenge.revision) return;
        revenge.revision = hit.hostTargetRevision;
        revenge.lastHit = GetTickCount64();
        revenge.maximumHealth = hit.maximumHealth;
        auto& threat = revenge.attackers[hit.sourceId];
        threat.damage *= std::exp(-static_cast<float>(revenge.lastHit - threat.at) / 5000.0f);
        threat.damage += damage;
        threat.at = revenge.lastHit;
    }

    void EnemyTargetDistributor::Shutdown() noexcept
    {
        for (auto& [uid, assignment] : assignments_)
        {
            Release(uid, assignment);
        }
        for (auto& entry : nativeAssignments_) Release(entry.first, entry.second);
        nativeAssignments_.clear();
        plans_.clear();
        transport_ = nullptr;
        lifecycle_ = nullptr;
        identities_ = nullptr;
        nextRevision_ = 0;
        assignments_.clear();
        revenge_.clear();
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
        game::creature::combat::native::AiTargetingComponent::ClearCombatTarget(assignment.creature);
        if (!assignment.overridden || assignment.creature == nullptr || entities_ == nullptr) return;
        // Only touch a creature that is still the same live Thing.
        const entities::LiveEntityRecord* const live = presence_ != nullptr
            ? presence_->LiveEntities().Find(thingUid) : nullptr;
        if (live != nullptr && live->thing == assignment.creature)
        {
            void* const targeting = game::entity::native::ThingComponentAccess::Find(
                assignment.creature, game::entity::native::ThingComponentType::Targeting);
            if (game::creature::combat::native::AiTargetingComponent::GetScriptTargetOverride(
                    entities_->GameModule(), targeting) == assignment.targetCreature)
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

        for (auto it = revenge_.begin(); it != revenge_.end(); )
        {
            if (now - it->second.lastHit > 10000) it = revenge_.erase(it);
            else ++it;
        }
        if (role_ != PeerRole::Host) { ApplyPlans(now); return; }
        const PlayerState* const local = localHero_->CurrentState();
        if (local == nullptr || local->mapId == 0) { ApplyPlans(now); return; }
        std::vector<Candidate> candidates;
        if (IsAlive(localHero_->NativeHero())) candidates.push_back({local->actorId, localHero_->NativeHero(), local->position, true, local->mapId});
        for (const auto& remote : remotePlayers_->Snapshots())
        {
            const auto& state = remote.state;
            if (state.mapId == 0 || remote.receivedAt == 0 || now - remote.receivedAt > RemoteStateFreshMilliseconds) continue;
            void* body = combatants_->FindCreature(state.actorId);
            if (state.mapId != local->mapId || IsAlive(body))
                candidates.push_back({state.actorId, body, state.position, false, state.mapId});
        }
        std::unordered_set<std::uint64_t> engaged;
        for (const auto& entry : entityActions_->EngagedCreatures(now, EngagementMilliseconds))
            engaged.insert(identities_->CanonicalizeLocalObservation(entry.thingUid));
        for (const auto& entry : revenge_) engaged.insert(entry.first);
        auto records = lifecycle_->Directory().Snapshot();
        std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) { return left.thingUid < right.thingUid; });
        std::unordered_map<std::uint64_t, unsigned> pressure;
        for (const auto& entry : assignments_) if (engaged.count(entry.first)) ++pressure[entry.second.actorId];
        std::unordered_map<std::uint64_t, Assignment> next;
        for (const auto& world : records)
        {
            if (!world.live || !world.available || !world.creature || !world.hasTransform || !engaged.count(world.thingUid)) continue;
            const auto* live = presence_->LiveEntities().Find(world.thingUid);
            if (live != nullptr && (!IsAlive(live->thing) || game::ScriptControl::HasNativeControl(live->thing))) continue;
            const auto position = world.position;
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) continue;
            const replication::EntityActionReplication::Engagement engagement{world.thingUid, live != nullptr ? live->thing : nullptr};
            Assignment assignment;
            const auto previous = assignments_.find(world.thingUid);
            if (previous != assignments_.end())
            {
                if (pressure[previous->second.actorId]) --pressure[previous->second.actorId];
                if (previous->second.generation == world.generation && previous->second.mapEpoch == world.mapEpoch)
                    assignment = previous->second;
            }
            assignment.generation = world.generation;
            assignment.mapEpoch = world.mapEpoch;
            assignment.mapId = world.mapId;
            const Candidate* best = nullptr;
            float bestScore = (std::numeric_limits<float>::max)();
            const Candidate* current = nullptr;
            float currentScore = 0.0f;
            for (const Candidate& candidate : candidates)
            {
                if (candidate.mapId != world.mapId) continue;
                const float distance = PlanarDistance(position, candidate.position);
                if (!std::isfinite(distance) || distance > 40.0f ||
                    std::fabs(position.z - candidate.position.z) > 12.0f) continue;
                // Spread a close swarm across living players while retaining
                // locality. A stable per-NPC preference avoids equal-distance
                // ties sending every wasp to the same Hero.
                const auto seed = engagement.thingUid ^ (candidate.actorId * 0x9E3779B97F4A7C15ull);
                const float score = distance + pressure[candidate.actorId] * 5.0f +
                    static_cast<float>((seed ^ (seed >> 32)) & 255u) / 128.0f;
                if (best == nullptr || score < bestScore)
                {
                    best = &candidate;
                    bestScore = score;
                }
                if (candidate.actorId == assignment.actorId)
                {
                    current = &candidate;
                    currentScore = score;
                }
            }
            const Candidate* chosen = best;
            if (current != nullptr && (now - assignment.selectedAt < 3000 ||
                currentScore <= bestScore + SwitchMargin)) chosen = current;
            auto retaliation = revenge_.find(engagement.thingUid);
            if (retaliation != revenge_.end())
            {
                auto& revenge = retaliation->second;
                const Candidate* incumbent = nullptr;
                const Candidate* challenger = nullptr;
                float incumbentThreat = 0, highestThreat = 0;
                for (const auto& candidate : candidates)
                {
                    if (candidate.mapId != world.mapId) continue;
                    const float distance = PlanarDistance(position, candidate.position);
                    if (distance > 30 || std::fabs(position.z - candidate.position.z) > 12) continue;
                    const auto found = revenge.attackers.find(candidate.actorId);
                    if (found == revenge.attackers.end() || now - found->second.at > 8000) continue;
                    // A far shooter cannot monopolize an enemy surrounded by
                    // nearby melee players; avoid endless ranged kiting.
                    if (distance > 18 && best != nullptr && PlanarDistance(position, best->position) < 7) continue;
                    const float threat = found->second.damage *
                        std::exp(-static_cast<float>(now - found->second.at) / 5000.0f);
                    if (candidate.actorId == revenge.actor) { incumbent = &candidate; incumbentThreat = threat; }
                    if (challenger == nullptr || threat > highestThreat) { challenger = &candidate; highestThreat = threat; }
                }
                if (challenger != nullptr)
                {
                    const bool replace = incumbent == nullptr || (now >= revenge.lockedUntil &&
                        highestThreat > incumbentThreat * 1.5f + revenge.maximumHealth * 0.05f);
                    const auto* priority = replace ? challenger : incumbent;
                    if (priority != nullptr)
                    {
                        if (priority->actorId != revenge.actor)
                        {
                            revenge.actor = priority->actorId;
                            revenge.lockedUntil = now + 6000;
                            char detail[160] = {};
                            std::snprintf(detail, sizeof(detail), "thing_uid=%016llX actor=%llu lock_ms=6000 threat=%.2f",
                                static_cast<unsigned long long>(engagement.thingUid),
                                static_cast<unsigned long long>(revenge.actor), highestThreat);
                            diagnostics_.Event("MultiplayerEnemyRevengeTargetChanged", detail);
                        }
                        chosen = priority;
                    }
                }
            }
            if (chosen == nullptr) continue;
            const bool changed = chosen->actorId != assignment.actorId;
            if (changed) assignment.selectedAt = now;
            ++pressure[chosen->actorId];
            assignment.actorId = chosen->actorId;
            if (changed || assignment.revision == 0) assignment.revision = ++nextRevision_;
            Plan plan{world.thingUid, chosen->actorId, assignment.revision, world.generation, world.mapEpoch, world.mapId};
            plans_[world.thingUid] = {plan, now};
            if (changed || now - assignment.publishedAt >= 1000)
            {
                if (transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::EnemyTarget,
                    reinterpret_cast<const std::uint8_t*>(&plan), sizeof(plan))) assignment.publishedAt = now;
            }
            if (changed)
            {
                char detail[192] = {};
                std::snprintf(detail, sizeof(detail), "thing_uid=%016llX target_actor=%llu distance=%.1f authority=host generation=%u epoch=%u",
                    static_cast<unsigned long long>(world.thingUid), static_cast<unsigned long long>(chosen->actorId),
                    PlanarDistance(position, chosen->position), world.generation, world.mapEpoch);
                diagnostics_.Event("MultiplayerEnemyTargetPlanned", detail);
            }
            next[world.thingUid] = assignment;
        }
        assignments_.swap(next);
        ApplyPlans(now);
    }
    bool EnemyTargetDistributor::HandleReliableMessage(const TransportMessage& message)
    {
        if (message.type != protocol::PacketType::EnemyTarget) return false;
        if (role_ == PeerRole::Host || message.payloadSize != sizeof(Plan)) return true;
        bool fromHost = false;
        for (const auto& peer : remotePlayers_->Snapshots())
            if (peer.state.actorId == message.sourceActorId && peer.state.role == PeerRole::Host) fromHost = true;
        if (!fromHost) return true;
        Plan plan;
        std::memcpy(&plan, message.payload.data(), sizeof(plan));
        if (!plan.uid || !plan.actor || !plan.revision || !plan.generation || !plan.mapEpoch || !plan.mapId) return true;
        if (plans_.size() >= 8192 && plans_.find(plan.uid) == plans_.end()) return true;
        auto found = plans_.find(plan.uid);
        if (found != plans_.end() && plan.revision < found->second.plan.revision) return true;
        plans_[plan.uid] = {plan, GetTickCount64()};
        return true;
    }

    void EnemyTargetDistributor::ApplyPlans(std::uint64_t now)
    {
        using game::creature::combat::native::AiTargetingComponent;
        const auto* local = localHero_->CurrentState();
        std::unordered_map<std::uint64_t, Assignment> next;
        for (auto it = plans_.begin(); it != plans_.end(); )
        {
            if (now - it->second.at > 10000) { it = plans_.erase(it); continue; }
            const Plan plan = it->second.plan;
            ++it;
            if (local == nullptr || plan.mapId != local->mapId) continue;
            const auto* world = lifecycle_->Directory().Find(plan.uid);
            const auto* live = presence_->LiveEntities().Find(plan.uid);
            if (world == nullptr || !world->live || world->generation != plan.generation || world->mapEpoch != plan.mapEpoch ||
                live == nullptr || !IsAlive(live->thing) || game::ScriptControl::HasNativeControl(live->thing) ||
                !simulation_->IsLocallySimulated(live->thing)) continue;
            void* target = plan.actor == local->actorId ? localHero_->NativeHero() : combatants_->FindCreature(plan.actor);
            if (!IsAlive(target)) continue;
            Assignment binding;
            const auto old = nativeAssignments_.find(plan.uid);
            if (old != nativeAssignments_.end() && old->second.creature == live->thing && old->second.targetCreature == target)
                binding = old->second;
            binding.creature = live->thing;
            binding.targetCreature = target;
            binding.actorId = plan.actor;
            if (!AiTargetingComponent::BindCombatTarget(entities_->GameModule(), live->thing, target))
            {
                diagnostics_.Event("MultiplayerNativeEnemyTargetBlocked", "native AI getter bytes or vtable did not match");
                continue;
            }
            if (!binding.readReported && AiTargetingComponent::ConsumeCombatTargetRead(live->thing))
            {
                char detail[160] = {};
                std::snprintf(detail, sizeof(detail), "thing_uid=%016llX target_actor=%llu; native combat/seek getter consumed host plan",
                    static_cast<unsigned long long>(plan.uid), static_cast<unsigned long long>(plan.actor));
                diagnostics_.Event("MultiplayerNativeEnemyTargetRead", detail);
                binding.readReported = true;
            }
            next[plan.uid] = binding;
        }
        for (auto& entry : nativeAssignments_) if (next.find(entry.first) == next.end()) Release(entry.first, entry.second);
        nativeAssignments_.swap(next);
    }

}

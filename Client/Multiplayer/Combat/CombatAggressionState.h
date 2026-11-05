#pragma once

#include "Multiplayer/Protocol/CombatHitMessage.h"
#include "Multiplayer/Protocol/PlayerState.h"

#include <cmath>
#include <unordered_map>

namespace fable::multiplayer::combat
{
    // Evidence for creating an actor-specific native enemy relation. Target
    // selection alone is never evidence of aggression by an innocent player.
    class CombatAggressionState final
    {
    public:
        bool Observe(const protocol::CombatHitMessage& hit, std::uint64_t now,
            std::uint64_t connectionNonce)
        {
            if (hit.phase != protocol::CombatHitPhase::Result ||
                hit.sourceDomain != protocol::CombatActionDomain::Player ||
                hit.targetKind != protocol::CombatParticipantKind::WorldEntity ||
                hit.sourceId == 0 || hit.targetId == 0 || connectionNonce == 0 ||
                hit.sourceAuthorityEpoch == 0 || hit.sourceGeneration == 0 ||
                hit.sourceMapEpoch == 0 || hit.targetGeneration == 0 ||
                hit.targetMapEpoch == 0 || hit.sourceMapId == 0 ||
                hit.sourceMapId != hit.targetMapId || hit.hostTargetRevision == 0 ||
                !std::isfinite(hit.healthBefore) || !std::isfinite(hit.healthAfter) ||
                hit.healthAfter < 0 || hit.healthAfter >= hit.healthBefore)
                return false;
            if (entries_.size() >= 8192 && entries_.count(hit.targetId) == 0)
                return false;
            auto& entry = entries_[hit.targetId];
            if (entry.generation != hit.targetGeneration ||
                entry.mapEpoch != hit.targetMapEpoch)
            {
                entry = {};
                entry.generation = hit.targetGeneration;
                entry.mapEpoch = hit.targetMapEpoch;
            }
            if (hit.hostTargetRevision <= entry.revision) return false;
            entry.revision = hit.hostTargetRevision;
            if (entry.attackers.size() >= 32 &&
                entry.attackers.count(hit.sourceId) == 0) return false;
            entry.attackers[hit.sourceId] = {hit, now, connectionNonce};
            return true;
        }

        bool Allows(std::uint64_t uid, std::uint32_t generation,
            std::uint32_t mapEpoch, const PlayerState& actor,
            std::uint64_t now, std::uint64_t connectionNonce) const noexcept
        {
            const auto entry = entries_.find(uid);
            if (entry == entries_.end() || entry->second.generation != generation ||
                entry->second.mapEpoch != mapEpoch) return false;
            const auto attacker = entry->second.attackers.find(actor.actorId);
            if (attacker == entry->second.attackers.end()) return false;
            const auto& evidence = attacker->second;
            const auto& hit = evidence.hit;
            return connectionNonce != 0 && connectionNonce == evidence.connectionNonce &&
                now >= evidence.at && now - evidence.at <= 10'000 &&
                actor.authorityEpoch == hit.sourceAuthorityEpoch &&
                actor.actorGeneration == hit.sourceGeneration &&
                actor.mapEpoch == hit.sourceMapEpoch && actor.mapId == hit.sourceMapId;
        }

        void Clear() noexcept { entries_.clear(); }

    private:
        struct Evidence
        {
            protocol::CombatHitMessage hit;
            std::uint64_t at = 0, connectionNonce = 0;
        };
        struct Entry
        {
            std::uint32_t generation = 0, mapEpoch = 0;
            std::uint64_t revision = 0;
            std::unordered_map<std::uint64_t, Evidence> attackers;
        };
        std::unordered_map<std::uint64_t, Entry> entries_;
    };
}

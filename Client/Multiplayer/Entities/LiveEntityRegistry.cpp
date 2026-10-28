#include <cmath>
#include "LiveEntityRegistry.h"
#include "Game/Entity/Native/ThingComponentAccess.h"

#include <utility>

namespace
{
    std::int32_t ReadGroundOrb(void* orb) noexcept
    {
        __try { const auto value = *reinterpret_cast<std::int32_t*>(static_cast<unsigned char*>(orb) + 0x10); return value >= 0 && value <= 1000000 ? value : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    bool ReadGroundPosition(void* thing, fable::game::Vector3& position) noexcept
    {
        // CThing::GetPosition RVA01B2B440: *(thing+6C)+0C.
        __try
        {
            void* physics = *reinterpret_cast<void**>(static_cast<unsigned char*>(thing) + 0x6C);
            if (physics == nullptr) return false;
            position = *reinterpret_cast<fable::game::Vector3*>(static_cast<unsigned char*>(physics) + 0x0C);
            return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::int32_t ReadGroundGold(void* gold) noexcept
    {
        __try { return *reinterpret_cast<const std::int32_t*>(static_cast<const unsigned char*>(gold) + 0x0C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

namespace fable::multiplayer::entities
{
    bool LiveEntityRegistry::Apply(
        const game::entity::presence::ThingPresenceEvent& event,
        LiveEntityChange& change)
    {
        change = {};
        if (event.thingUid == 0 || event.thing == nullptr)
        {
            return false;
        }

        auto existing = records_.find(event.thingUid);
        if (event.phase ==
            game::entity::presence::ThingPresencePhase::Registered)
        {
            const bool wasAbsent = existing == records_.end();
            if (existing != records_.end() &&
                existing->second.thing == event.thing)
            {
                existing->second.mapId = event.mapId;
                existing->second.villageUid = event.villageUid;
                existing->second.definitionIndex = event.definitionIndex;
                existing->second.scriptName = event.scriptName.data();
                existing->second.position = event.position;
                existing->second.facing = event.facing;
                existing->second.hasTransform = event.hasTransform;
                existing->second.gamePersistent = event.gamePersistent;
                existing->second.levelPersistent = event.levelPersistent;
                existing->second.creature = event.creature;
                existing->second.hasHeroMorph = event.hasHeroMorph;
                existing->second.hasVillageMembership =
                    event.hasVillageMembership;
                existing->second.summonedCreature =
                    existing->second.summonedCreature ||
                    event.summonedCreature;
                existing->second.abilityOwnedTransient =
                    existing->second.abilityOwnedTransient ||
                    event.abilityOwnedTransient;
                existing->second.mapwhoComponent = event.component;
                change.kind = LiveEntityChangeKind::Rebound;
                change.record = existing->second;
                MarkChanged();
                return true;
            }

            LiveEntityRecord record;
            record.thingUid = event.thingUid;
            record.villageUid = event.villageUid;
            record.localIncarnation = NextIncarnation();
            record.mapId = event.mapId;
            record.definitionIndex = event.definitionIndex;
            record.scriptName = event.scriptName.data();
            record.position = event.position;
            record.facing = event.facing;
            record.hasTransform = event.hasTransform;
            record.gamePersistent = event.gamePersistent;
            record.levelPersistent = event.levelPersistent;
            record.creature = event.creature;
            record.hasHeroMorph = event.hasHeroMorph;
            record.hasVillageMembership = event.hasVillageMembership;
            record.summonedCreature = event.summonedCreature;
            record.abilityOwnedTransient = event.abilityOwnedTransient;
            record.thing = event.thing;
            if (!record.creature)
            {
                using namespace game::entity::native;
                void* gold = ThingComponentAccess::Find(event.thing, static_cast<ThingComponentType>(0xCC));
                record.groundLoot = ThingComponentAccess::Find(event.thing, static_cast<ThingComponentType>(0x69)) != nullptr || gold != nullptr || ThingComponentAccess::FindByVtableRva(
                    event.thing, GetModuleHandleW(nullptr), 0x02B0DC8C) != nullptr;
                if (gold != nullptr)
                {
                    record.lootGold = ReadGroundGold(gold);
                }
            }
            record.mapwhoComponent = event.component;
            if (existing != records_.end())
            {
                uidByThing_.erase(existing->second.thing);
            }
            records_[event.thingUid] = record;
            uidByThing_[event.thing] = event.thingUid;
            change.kind = wasAbsent
                ? LiveEntityChangeKind::Registered
                : LiveEntityChangeKind::Rebound;
            change.record = record;
            MarkChanged();
            return true;
        }

        if (existing == records_.end() ||
            existing->second.thing != event.thing)
        {
            return false;
        }
        // The retail crossing path can assign CThing+0x9A to the destination
        // immediately before CTCMapwho unregisters the source incarnation.
        // Preserve that final synchronous context instead of returning the
        // older registration snapshot.
        if (event.mapId != 0)
        {
            existing->second.mapId = event.mapId;
        }
        existing->second.definitionIndex = event.definitionIndex;
        existing->second.villageUid = event.villageUid;
        existing->second.scriptName = event.scriptName.data();
        if (event.hasTransform)
        {
            existing->second.position = event.position;
            existing->second.facing = event.facing;
            existing->second.hasTransform = true;
        }
        existing->second.gamePersistent = event.gamePersistent;
        existing->second.levelPersistent = event.levelPersistent;
        existing->second.creature = event.creature;
        existing->second.hasHeroMorph = event.hasHeroMorph;
        existing->second.hasVillageMembership = event.hasVillageMembership;
        existing->second.summonedCreature =
            existing->second.summonedCreature || event.summonedCreature;
        existing->second.abilityOwnedTransient =
            existing->second.abilityOwnedTransient ||
            event.abilityOwnedTransient;
        existing->second.mapwhoComponent = event.component;
        change.kind = LiveEntityChangeKind::Unregistered;
        change.record = existing->second;
        uidByThing_.erase(existing->second.thing);
        records_.erase(existing);
        MarkChanged();
        return true;
    }

    const LiveEntityRecord* LiveEntityRegistry::Find(
        std::uint64_t thingUid) const noexcept
    {
        const auto match = records_.find(thingUid);
        return match != records_.end() ? &match->second : nullptr;
    }

    const LiveEntityRecord* LiveEntityRegistry::FindByThing(
        const void* thing) const noexcept
    {
        if (thing == nullptr)
        {
            return nullptr;
        }
        const auto uid = uidByThing_.find(thing);
        return uid != uidByThing_.end() ? Find(uid->second) : nullptr;
    }

    std::uint64_t LiveEntityRegistry::Revision() const noexcept
    {
        return revision_;
    }

    bool LiveEntityRegistry::Remap(
        std::uint64_t localUid,
        std::uint64_t canonicalUid) noexcept
    {
        if (localUid == 0 || canonicalUid == 0)
        {
            return false;
        }
        if (localUid == canonicalUid)
        {
            return true;
        }
        const auto local = records_.find(localUid);
        if (local == records_.end())
        {
            // The alias may be installed synchronously while CreateCreature's
            // native registration event is still waiting in the bridge queue.
            return true;
        }
        if (records_.find(canonicalUid) != records_.end())
        {
            return false;
        }
        LiveEntityRecord record = std::move(local->second);
        records_.erase(local);
        record.thingUid = canonicalUid;
        if (record.thing != nullptr)
        {
            uidByThing_[record.thing] = canonicalUid;
        }
        records_.emplace(canonicalUid, std::move(record));
        MarkChanged();
        return true;
    }

    std::vector<LiveEntityRecord> LiveEntityRegistry::Snapshot() const
    {
        std::vector<LiveEntityRecord> result;
        result.reserve(records_.size());
        for (const auto& entry : records_)
        {
            result.push_back(entry.second);
        }
        return result;
    }

    std::size_t LiveEntityRegistry::Size() const noexcept
    {
        return records_.size();
    }

    std::vector<LiveEntityChange> LiveEntityRegistry::RefreshGroundLoot()
    {
        std::vector<LiveEntityChange> changed;
        for (auto& entry : records_)
        {
            auto& record = entry.second;
            if (record.creature || record.thing == nullptr) continue;
            using namespace game::entity::native;
            void* orb = ThingComponentAccess::Find(record.thing, static_cast<ThingComponentType>(0x69));
            void* gold = ThingComponentAccess::Find(record.thing, static_cast<ThingComponentType>(0xCC));
            if (orb == nullptr && gold == nullptr) continue;
            auto updated = record;
            updated.groundLoot = true;
            if (orb != nullptr) updated.lootExperience = ReadGroundOrb(orb);
            if (gold != nullptr) updated.lootGold = ReadGroundGold(gold);
            game::Vector3 position;
            if (orb != nullptr && ReadGroundPosition(record.thing, position) &&
                (!record.hasTransform || record.position.HorizontalDistanceTo(position) >= 0.10f || std::fabs(record.position.z - position.z) >= 0.10f))
            {
                updated.position = position;
                updated.hasTransform = true;
            }
            if (updated.groundLoot != record.groundLoot || updated.lootExperience != record.lootExperience ||
                updated.lootGold != record.lootGold || updated.position.x != record.position.x ||
                updated.position.y != record.position.y || updated.position.z != record.position.z)
            {
                record = updated;
                changed.push_back({LiveEntityChangeKind::Rebound, record});
                MarkChanged();
            }
        }
        return changed;
    }

    bool LiveEntityRegistry::IsReplicable(
        const LiveEntityRecord& record) noexcept
    {
        const bool networkIdentityBearing = record.creature || record.groundLoot ||
            record.gamePersistent || record.levelPersistent ||
            record.hasVillageMembership || !record.scriptName.empty();
        return record.thingUid != 0 && record.thing != nullptr &&
            networkIdentityBearing && !record.summonedCreature &&
            !record.abilityOwnedTransient &&
            record.scriptName != "HeroSummonnedCreature" &&
            record.scriptName != "HeroSummonedCreature" &&
            !IsPlayerPresentation(record);
    }

    bool LiveEntityRegistry::IsPlayerPresentation(
        const LiveEntityRecord& record) noexcept
    {
        return record.scriptName == "SCRIPT_NAME_HERO" ||
            record.scriptName == "SCRIPT_NAME_ALBION_TOGETHER_REMOTE_PLAYER";
    }

    void LiveEntityRegistry::Clear() noexcept
    {
        if (!records_.empty())
        {
            MarkChanged();
        }
        records_.clear();
        uidByThing_.clear();
        nextIncarnation_ = 0;
    }

    std::uint32_t LiveEntityRegistry::NextIncarnation() noexcept
    {
        ++nextIncarnation_;
        if (nextIncarnation_ == 0)
        {
            ++nextIncarnation_;
        }
        return nextIncarnation_;
    }

    void LiveEntityRegistry::MarkChanged() noexcept
    {
        ++revision_;
        if (revision_ == 0)
        {
            ++revision_;
        }
    }
}

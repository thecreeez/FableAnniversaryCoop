#include "KillExperienceShare.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Multiplayer/Entities/EntityPresenceReplication.h"
#include "Multiplayer/Entities/EntityNetworkIdentityRegistry.h"
#include "Multiplayer/Entities/EntityLifecycleReplication.h"

#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveKillExperienceSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.actions.killExperience;
    }

    // CTCHeroExperience::AddGeneralExperience(amount): sub esp,8; push esi;
    // mov esi,ecx; cmp byte ptr [esi+44h],0
    constexpr std::uintptr_t AddGeneralRva = 0x019BB310;
    constexpr std::uint8_t AddGeneralPrologue[] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x44, 0x00};
    // Return address of the kill handler's call (after "call AddGeneral").
    constexpr std::uintptr_t KillGrantReturnRva = 0x01AC4606;
    constexpr std::int32_t HeroExperienceComponent = 0x68;
    // CTCHeroExperience stat experience: Get(index), Set(index, value);
    // 0 strength, 1 skill, 2 will.
    constexpr std::uintptr_t GetStatRva = 0x019B8CC0;
    constexpr std::uintptr_t SetStatRva = 0x019B8FE0;
    using GetStatFn = std::int32_t(__thiscall*)(void*, std::int32_t);
    using SetStatFn = void(__thiscall*)(void*, std::int32_t, std::int32_t);

    int OrbAmount(void* component) noexcept
    {
        __try { const int amount = *reinterpret_cast<int*>(static_cast<unsigned char*>(component) + 0x10); return amount > 0 && amount <= 1000000 ? amount : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    void* OrbOwner(void* component) noexcept
    {
        __try { return component != nullptr ? *reinterpret_cast<void**>(static_cast<unsigned char*>(component) + 4) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool CollectOrbNative(std::uintptr_t base, void* component, void* hero) noexcept
    {
        __try
        {
            const auto* code = reinterpret_cast<const unsigned char*>(base + 0x0191E9C0);
            if (code[0] != 0x6A || code[1] != 0xFF || code[2] != 0x68 ||
                *reinterpret_cast<const std::uintptr_t*>(code + 3) != base + 0x0251F418) return false;
            reinterpret_cast<void(__thiscall*)(void*, void*)>(base + 0x0191E9C0)(component, hero);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadStats(std::uintptr_t base, void* experience, std::int32_t (&values)[3]) noexcept
    {
        __try
        {
            for (int index = 0; index < 3; ++index)
            {
                values[index] = reinterpret_cast<GetStatFn>(base + GetStatRva)(experience, index);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool AddStat(std::uintptr_t base, void* experience, std::int32_t index, std::int32_t amount) noexcept
    {
        __try
        {
            const std::int32_t current = reinterpret_cast<GetStatFn>(base + GetStatRva)(experience, index);
            reinterpret_cast<SetStatFn>(base + SetStatRva)(experience, index, current + amount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkKillExperience,
    0x1014u,
    "kill-experience",
    819u,
    "multiplayer-kill-experience-dispatch",
    ResolveKillExperienceSink);

namespace fable::multiplayer::combat
{
    KillExperienceShare* KillExperienceShare::active_ = nullptr;
    thread_local KillExperienceShare::Grant KillExperienceShare::pickupGrant_{};
    thread_local bool KillExperienceShare::inPickup_ = false;

    void KillExperienceShare::Initialize(
        const PeerRole role, UdpPeer& transport, replication::LocalHeroReplication& localHero,
        game::EntityService& gameEntities, entities::EntityPresenceReplication& presence,
        entities::EntityNetworkIdentityRegistry& identities, entities::EntityLifecycleReplication& lifecycle,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        entities_ = &gameEntities;
        presence_ = &presence;
        identities_ = &identities;
        lifecycle_ = &lifecycle;
        role_ = role;
        transport_ = &transport;
        localHero_ = &localHero;
        diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        killReturn_ = base + KillGrantReturnRva;
        active_ = this;
        if (!add_.Install(reinterpret_cast<void*>(base + AddGeneralRva), AddGeneralPrologue, sizeof(AddGeneralPrologue),
                reinterpret_cast<void*>(&AddObserved), sizeof(AddGeneralPrologue)))
        {
            diagnostics_.Event("KillExperienceShareBlocked", "experience grant prologue did not match");
            active_ = nullptr;
            return;
        }
        original_ = reinterpret_cast<AddFn>(add_.Original());
        // Inventory gold mutation. Only calls from the retail ground pickup
        // routine are shared; quest rewards, chests and commerce stay distinct.
        const std::uint8_t goldPrefix[] = {0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08};
        if (goldHook_.Install(reinterpret_cast<void*>(base + 0x019CC8CF), goldPrefix, sizeof(goldPrefix),
                reinterpret_cast<void*>(&GoldObserved), sizeof(goldPrefix)))
            originalGold_ = reinterpret_cast<GoldFn>(goldHook_.Original());
        else diagnostics_.Event("GroundGoldShareBlocked", "gold mutation entry bytes did not match");
        const std::uint32_t handler = static_cast<std::uint32_t>(base + 0x02555F59);
        std::uint8_t pickupPrefix[7] = {0x6A, 0xFF, 0x68};
        std::memcpy(pickupPrefix + 3, &handler, sizeof(handler));
        if (pickupHook_.Install(reinterpret_cast<void*>(base + 0x01B5F600), pickupPrefix, sizeof(pickupPrefix),
                reinterpret_cast<void*>(&PickupObserved), sizeof(pickupPrefix)))
            originalPickup_ = reinterpret_cast<PickupFn>(pickupHook_.Original());
        else diagnostics_.Event("GroundPickupShareBlocked", "pickup entry bytes did not match");
        const unsigned char recognize[] = {0x56, 0x8B, 0xF1, 0x8B, 0x06};
        const unsigned char merge[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10};
        unsigned char update[7] = {0x6A, 0xFF, 0x68};
        const auto updateHandler = base + 0x02526160;
        std::memcpy(update + 3, &updateHandler, sizeof(updateHandler));
        if (orbRecognizeHook_.Install(reinterpret_cast<void*>(base + 0x0191D790), recognize, sizeof(recognize),
            reinterpret_cast<void*>(&OrbRecognized), sizeof(recognize))) originalOrbRecognize_ = reinterpret_cast<OrbRecognizeFn>(orbRecognizeHook_.Original());
        if (orbMergeHook_.Install(reinterpret_cast<void*>(base + 0x0191DD40), merge, sizeof(merge),
            reinterpret_cast<void*>(&OrbMerge), sizeof(merge))) originalOrbMerge_ = reinterpret_cast<OrbCollectFn>(orbMergeHook_.Original());
        if (orbUpdateHook_.Install(reinterpret_cast<void*>(base + 0x0191E440), update, sizeof(update),
            reinterpret_cast<void*>(&OrbUpdated), sizeof(update))) originalOrbUpdate_ = reinterpret_cast<OrbUpdateFn>(orbUpdateHook_.Original());
        diagnostics_.Event(originalOrbRecognize_ && originalOrbMerge_ && originalOrbUpdate_ ? "ExperienceOrbSyncReady" : "ExperienceOrbSyncBlocked",
            "canonical orb amounts, host pickup reservation, shared native collection; replica attraction/merging fenced");
        diagnostics_.Event("KillExperienceShareReady", "killer 1x, other players 0.3x");
    }

    void KillExperienceShare::Shutdown() noexcept
    {
        if (!orbRecognizeHook_.Shutdown() || !orbMergeHook_.Shutdown() || !orbUpdateHook_.Shutdown()) return;
        originalOrbRecognize_ = nullptr;
        originalOrbMerge_ = nullptr;
        originalOrbUpdate_ = nullptr;
        for (auto& fraction : experienceRemainder_) fraction = 0;
        if (!pickupHook_.Shutdown() || !goldHook_.Shutdown() || !add_.Shutdown()) return;
        originalPickup_ = nullptr;
        consumedLoot_.clear();
        lootClaims_.clear();
        localPickupRequests_.clear();
        entities_ = nullptr;
        presence_ = nullptr;
        identities_ = nullptr;
        lifecycle_ = nullptr;
        originalGold_ = nullptr;
        localGold_.clear();
        lastGrant_.clear();
        nextGrantId_ = 0;
        original_ = nullptr;
        if (active_ == this) active_ = nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        localKills_.clear();
        inbound_.clear();
        outbound_.clear();
        transport_ = nullptr;
        localHero_ = nullptr;
    }

    void __fastcall KillExperienceShare::AddObserved(void* const experience, void* const edx, const std::int32_t amount)
    {
        (void)edx;
        KillExperienceShare* const self = active_;
        if (self == nullptr || self->original_ == nullptr) return;
        // The hook jumps here from the patched entry, so this is the
        // retail caller's return address.
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto base = self->killReturn_ - KillGrantReturnRva;
        // An orb collected on its creation frame may not yet have a canonical
        // presence row. Its real PlayerCreature contact award still shares XP.
        if ((caller == self->killReturn_ || (caller >= base + 0x01B605D0 && caller < base + 0x01B60787)) && amount > 0)
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->localKills_.size() < QueueLimit) self->localKills_.push_back(amount);
        }
        self->original_(experience, amount);
    }

    bool __fastcall KillExperienceShare::OrbRecognized(void* item, void*, void** component)
    {
        auto* self = active_;
        if (self == nullptr || self->originalOrbRecognize_ == nullptr) return false;
        if (inPickup_) return self->originalOrbRecognize_(item, component);
        const auto* live = self->presence_->LiveEntities().FindByThing(item);
        if (live == nullptr || !live->groundLoot) return self->originalOrbRecognize_(item, component);
        void* orb = game::entity::native::ThingComponentAccess::Find(item,
            static_cast<game::entity::native::ThingComponentType>(0x69));
        if (orb == nullptr || OrbAmount(orb) == 0) return self->originalOrbRecognize_(item, component);
        Grant request;
        request.itemUid = live->thingUid;
        request.general = OrbAmount(orb);
        request.mapId = live->mapId;
        request.capturedAt = GetTickCount64();
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->consumedLoot_.count(request.itemUid)) return false;
        const auto previous = self->localPickupRequests_.find(request.itemUid);
        if (previous != self->localPickupRequests_.end() && request.capturedAt - previous->second < 5000) return false;
        if (self->localGold_.size() >= QueueLimit) return false;
        self->localPickupRequests_[request.itemUid] = request.capturedAt;
        request.operation = 1;
        self->localGold_.push_back(request);
        return false; // Native contact must wait for the host's reservation.
    }

    void __fastcall KillExperienceShare::OrbMerge(void* component, void*, void* other)
    {
        auto* self = active_;
        if (self == nullptr || self->originalOrbMerge_ == nullptr) return;
        // Keep one canonical UID per fragment. Independent native merging
        // destroys UIDs and changes amounts differently on each client.
        if (self->presence_->LiveEntities().FindByThing(OrbOwner(component)) != nullptr) return;
        self->originalOrbMerge_(component, other);
    }

    void __fastcall KillExperienceShare::OrbUpdated(void* component, void*)
    {
        auto* self = active_;
        if (self == nullptr || self->originalOrbUpdate_ == nullptr) return;
        const auto* state = self->localHero_->CurrentState();
        const auto* live = self->presence_->LiveEntities().FindByThing(OrbOwner(component));
        const auto* world = live != nullptr ? self->lifecycle_->Directory().Find(live->thingUid) : nullptr;
        if (state != nullptr && world != nullptr && world->groundLoot && world->simulationOwnerActorId != state->actorId) return;
        self->originalOrbUpdate_(component);
    }

    void __fastcall KillExperienceShare::PickupObserved(void* hero, void*, void* item)
    {
        auto* self = active_;
        if (self == nullptr || self->originalPickup_ == nullptr) return;
        const auto* live = self->presence_->LiveEntities().FindByThing(item);
        if (live == nullptr || !live->groundLoot || inPickup_)
        {
            self->originalPickup_(hero, item);
            return;
        }
        Grant grant;
        grant.itemUid = self->identities_->CanonicalizeLocalObservation(live->thingUid);
        grant.mapId = live->mapId;
        grant.capturedAt = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->consumedLoot_.count(grant.itemUid) != 0) return;
        }
        // Reserve first. No native inventory mutation happens until the
        // host grants this canonical item to exactly one collector.
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto found = self->localPickupRequests_.find(grant.itemUid);
        if (found != self->localPickupRequests_.end() && grant.capturedAt - found->second < 5000) return;
        if (self->localGold_.size() >= QueueLimit) return;
        self->localPickupRequests_[grant.itemUid] = grant.capturedAt;
        grant.operation = 1;
        self->localGold_.push_back(grant);
    }

    void __fastcall KillExperienceShare::GoldObserved(void* inventory, void*, std::int32_t amount, std::uint8_t notify)
    {
        auto* self = active_;
        if (self == nullptr || self->originalGold_ == nullptr) return;
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        self->originalGold_(inventory, amount, notify);
        if (amount <= 0 || amount > 1000000 || caller < 0x01B5F600 || caller >= 0x01B5FF96) return;
        if (inPickup_) pickupGrant_.gold += amount;
    }

    void KillExperienceShare::TrackStatExperience(void* const experience)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::int32_t current[StatCount] = {};
        if (experience == nullptr || !ReadStats(base, experience, current))
        {
            trackedExperience_ = nullptr;
            return;
        }
        if (experience != trackedExperience_)
        {
            // New Hero or first frame: only start tracking.
            trackedExperience_ = experience;
            std::memcpy(statBaseline_, current, sizeof(statBaseline_));
            return;
        }
        Grant grant;
        bool earned = false;
        for (int index = 0; index < StatCount; ++index)
        {
            const std::int32_t delta = current[index] - statBaseline_[index];
            if (delta > 0 && delta < 1'000'000)
            {
                grant.stats[index] = delta;
                earned = true;
            }
        }
        std::memcpy(statBaseline_, current, sizeof(statBaseline_));
        if (earned && outbound_.size() < QueueLimit) outbound_.push_back(grant);
    }

    void KillExperienceShare::InjectKillForTest(const std::int32_t amount)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (amount > 0 && localKills_.size() < QueueLimit) localKills_.push_back(amount);
    }

    bool KillExperienceShare::CollectOrbForAcceptance(std::uint64_t uid)
    {
        const auto* live = presence_ != nullptr ? presence_->LiveEntities().Find(uid) : nullptr;
        if (live == nullptr || live->lootExperience <= 0) return false;
        void* component = nullptr;
        (void)OrbRecognized(live->thing, nullptr, &component);
        return true;
    }

    bool KillExperienceShare::Process()
    {
        if (transport_ == nullptr || original_ == nullptr) return true;
        if (transport_->HasFailed()) return false;
        // Keep reliable awards pending across world teardown/loading.
        if (localHero_ == nullptr || !localHero_->IsWorldReady() || localHero_->CurrentState() == nullptr) return true;
        std::deque<std::int32_t> kills;
        std::deque<Grant> inbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            kills.swap(localKills_);
            inbound.swap(inbound_);
            while (!localGold_.empty() && outbound_.size() < QueueLimit)
            {
                Grant candidate = localGold_.front();
                if (candidate.operation == 3)
                {
                    const auto* live = presence_->LiveEntities().Find(candidate.itemUid);
                    if (live != nullptr && GetTickCount64() - candidate.capturedAt < 2000) break;
                    if (live != nullptr) candidate.operation = 4; // inventory refused it
                }
                const auto* state = localHero_->CurrentState();
                candidate.originActorId = state->actorId;
                candidate.originNonce = transport_->ConnectionNonce();
                candidate.grantId = ++nextGrantId_;
                if (role_ == PeerRole::Host) inbound.push_back(candidate);
                else outbound_.push_back(candidate);
                localGold_.pop_front();
            }
        }
        for (const std::int32_t amount : kills)
        {
            Grant grant;
            grant.general = amount;
            if (outbound_.size() < QueueLimit) outbound_.push_back(grant);
        }
        void* const hero = localHero_ != nullptr ? localHero_->NativeHero() : nullptr;
        void* const experience = hero != nullptr
            ? game::entity::native::ThingComponentAccess::Find(
                hero, static_cast<game::entity::native::ThingComponentType>(HeroExperienceComponent))
            : nullptr;
        // Own earnings first, so shares applied below are not sent back.
        TrackStatExperience(experience);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const PlayerState* localState = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        const auto claimNow = GetTickCount64();
        for (auto it = lootClaims_.begin(); it != lootClaims_.end();)
            it = claimNow >= it->second.until ? lootClaims_.erase(it) : std::next(it);
        while (!inbound.empty())
        {
            Grant grant = inbound.front();
            inbound.pop_front();
            const auto now = GetTickCount64();
            if (grant.operation == 1)
            {
                if (role_ != PeerRole::Host || consumedLoot_.count(grant.itemUid) != 0) continue;
                const auto* record = lifecycle_->Directory().Find(grant.itemUid);
                if (record == nullptr || !record->groundLoot || !record->live || !record->available || record->mapId != grant.mapId) continue;
                auto claim = lootClaims_.find(grant.itemUid);
                if (claim != lootClaims_.end() && now < claim->second.until) continue;
                if (lootClaims_.size() >= 4096 && claim == lootClaims_.end()) continue;
                lootClaims_[grant.itemUid] = {grant.originActorId, grant.originNonce, now + 5000};
                grant.operation = 2;
                if (localState != nullptr && grant.originActorId == localState->actorId) inbound.push_back(grant);
                else outbound_.push_back(grant);
                continue;
            }
            if (grant.operation == 2)
            {
                if (localState == nullptr || grant.originActorId != localState->actorId) continue;
                const auto* item = presence_->LiveEntities().Find(grant.itemUid);
                if (item == nullptr || item->thing == nullptr || localState->mapId != grant.mapId) continue;
                pickupGrant_ = grant;
                pickupGrant_.operation = 3;
                pickupGrant_.capturedAt = now;
                inPickup_ = true;
                void* orb = game::entity::native::ThingComponentAccess::Find(item->thing,
                    static_cast<game::entity::native::ThingComponentType>(0x69));
                if (orb != nullptr && OrbAmount(orb) > 0)
                {
                    const int amount = OrbAmount(orb);
                    if (!CollectOrbNative(base, orb, hero)) pickupGrant_.operation = 4;
                    else
                    {
                        pickupGrant_.general = amount;
                        original_(experience, amount);
                        diagnostics_.Event("ExperienceOrbCollected", "native orb reward committed after host reservation");
                    }
                }
                else originalPickup_(hero, item->thing);
                inPickup_ = false;
                std::lock_guard<std::mutex> lock(mutex_);
                if (localGold_.size() < QueueLimit) localGold_.push_back(pickupGrant_);
                pickupGrant_ = {};
                continue;
            }
            if (grant.operation == 3 || grant.operation == 4)
            {
                if (role_ != PeerRole::Host) continue;
                auto claim = lootClaims_.find(grant.itemUid);
                if (claim == lootClaims_.end() || claim->second.actor != grant.originActorId || claim->second.nonce != grant.originNonce) continue;
                lootClaims_.erase(claim);
                if (grant.operation == 4) continue;
                grant.operation = 0;
            }
            if (grant.itemUid != 0)
            {
                if (role_ == PeerRole::Host && !consumedLoot_.insert(grant.itemUid).second)
                {
                    // A simultaneous second gold pickup was already credited
                    // by retail on its collector. Return a collector-only refund.
                    if (grant.gold > 0 && outbound_.size() < QueueLimit)
                    {
                        Grant refund = grant;
                        refund.itemUid = 0;
                        refund.gold = -grant.gold;
                        outbound_.push_back(refund);
                    }
                    continue;
                }
                if (role_ == PeerRole::Host && !lifecycle_->CommitGroundPickup(grant.itemUid, grant.mapId)) continue;
                consumedLoot_.insert(grant.itemUid);
                const auto uid = identities_->FindLocal(grant.itemUid);
                game::Entity* item = entities_->FindByUid(uid != 0 ? uid : grant.itemUid);
                if (item != nullptr) { item->RequestDestroy(false); item->Release(); }
            }
            if (grant.itemUid != 0)
            {
                char lootDetail[160] = {};
                std::snprintf(lootDetail, sizeof(lootDetail), "item_uid=%016llX map_id=%u gold=%d collector=%llu",
                    static_cast<unsigned long long>(grant.itemUid), grant.mapId, grant.gold,
                    static_cast<unsigned long long>(grant.originActorId));
                diagnostics_.Event("GroundLootCollectionCommitted", lootDetail);
            }
            const bool ownGrant = localState != nullptr && grant.originActorId == localState->actorId;
            const auto share = [this](std::int32_t amount, int index)
            {
                const double exact = static_cast<double>(amount) * PeerShare + experienceRemainder_[index];
                const auto whole = static_cast<std::int32_t>(std::floor(exact + 1e-8));
                experienceRemainder_[index] = exact - whole;
                return whole;
            };
            Grant applied;
            if (experience != nullptr && !ownGrant)
            {
                applied.general = share(grant.general, 0);
                if (applied.general > 0) original_(experience, applied.general);
                for (int index = 0; index < StatCount; ++index)
                {
                    applied.stats[index] = share(grant.stats[index], index + 1);
                    if (applied.stats[index] > 0) AddStat(base, experience, index, applied.stats[index]);
                }
            }
            if (ownGrant && grant.gold < 0 && localState != nullptr && originalGold_ != nullptr)
            {
                void* inventory = game::entity::native::ThingComponentAccess::Find(hero,
                    static_cast<game::entity::native::ThingComponentType>(4));
                if (inventory != nullptr) originalGold_(inventory, grant.gold, 1);
            }
            if (!ownGrant && grant.gold > 0 && localState != nullptr &&
                localState->mapId == grant.mapId && originalGold_ != nullptr)
            {
                void* inventory = game::entity::native::ThingComponentAccess::Find(hero,
                    static_cast<game::entity::native::ThingComponentType>(4));
                if (inventory != nullptr) originalGold_(inventory, grant.gold, 1);
            }
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail),
                "general=%d strength=%d skill=%d will=%d share=(%d,%d,%d,%d)",
                grant.general, grant.stats[0], grant.stats[1], grant.stats[2],
                applied.general, applied.stats[0], applied.stats[1], applied.stats[2]);
            diagnostics_.Event("KillExperienceShared", detail);
            // The host fans a guest's earnings out to the other guests.
            if (role_ == PeerRole::Host && outbound_.size() < QueueLimit) outbound_.push_back(grant);
        }
        // Re-baseline after shares so they are not mistaken for earnings.
        if (experience != nullptr && experience == trackedExperience_)
        {
            std::int32_t current[StatCount] = {};
            if (ReadStats(base, experience, current)) std::memcpy(statBaseline_, current, sizeof(statBaseline_));
        }
        while (!outbound_.empty())
        {
            Grant& grant = outbound_.front();
            if (grant.originActorId == 0)
            {
                if (localState == nullptr) return true;
                grant.originActorId = localState->actorId;
                grant.grantId = ++nextGrantId_;
                grant.originNonce = transport_->ConnectionNonce();
                if (grant.mapId == 0) grant.mapId = localState->mapId;
                if (role_ == PeerRole::Host && grant.itemUid != 0 && grant.operation == 0)
                {
                    if (!consumedLoot_.insert(grant.itemUid).second)
                    {
                        if (grant.gold > 0 && originalGold_ != nullptr)
                        {
                            void* inventory = game::entity::native::ThingComponentAccess::Find(hero,
                                static_cast<game::entity::native::ThingComponentType>(4));
                            if (inventory != nullptr) originalGold_(inventory, -grant.gold, 1);
                        }
                        outbound_.pop_front();
                        continue;
                    }
                    (void)lifecycle_->CommitGroundPickup(grant.itemUid, grant.mapId);
                }
            }
            const Grant wire = grant;
            if (!transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::KillExperience,
                    reinterpret_cast<const std::uint8_t*>(&wire), sizeof(wire)))
            {
                return !transport_->HasFailed();
            }
            outbound_.pop_front();
        }
        return true;
    }

    bool KillExperienceShare::HandleReliableMessage(const TransportMessage& message)
    {
        if (message.type != protocol::PacketType::KillExperience) return false;
        Grant grant{};
        if (message.payloadSize != sizeof(grant)) return true;
        std::memcpy(&grant, message.payload.data(), sizeof(grant));
        if (grant.operation > 4 || (grant.operation != 0 && grant.itemUid == 0) ||
            (role_ == PeerRole::Host && grant.operation == 2) ||
            grant.originActorId == 0 || grant.grantId == 0 ||
            (role_ == PeerRole::Host && (grant.originActorId != message.sourceActorId || grant.originNonce != message.connectionNonce))) return true;
        const auto sane = [](std::int32_t value) { return value >= 0 && value <= 1000000; };
        if (!sane(grant.general) || grant.gold < -1000000 || grant.gold > 1000000 ||
            (role_ == PeerRole::Host && grant.gold < 0)) return true;
        bool any = grant.itemUid != 0 || grant.general > 0 || grant.gold != 0;
        for (const auto value : grant.stats) { if (!sane(value)) return true; any = any || value > 0; }
        if (!any || (grant.gold > 0 && grant.mapId == 0)) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& previous = lastGrant_[grant.originActorId];
        if ((previous.first == grant.originNonce && grant.grantId <= previous.second) || inbound_.size() >= QueueLimit) return true;
        previous = {grant.originNonce, grant.grantId};
        inbound_.push_back(grant);
        return true;
    }
}

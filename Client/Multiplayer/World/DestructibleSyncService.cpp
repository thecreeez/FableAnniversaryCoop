#include "DestructibleSyncService.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/Entity/Native/ThingComponentProvisioner.h"
#include "Multiplayer/Entities/EntityPresenceReplication.h"
#include "Multiplayer/Entities/EntityNetworkIdentityRegistry.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"
#include <cstring>

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveDestructible(fable::multiplayer::MultiplayerSessionContexts& c) noexcept
    { return &c.world.destructibles; }
}
FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkDestructible, 0x1021u, "destructibles", 825u,
    "multiplayer-destructibles-dispatch", ResolveDestructible);

namespace fable::multiplayer::world
{
    DestructibleSyncService* DestructibleSyncService::active_ = nullptr;
    thread_local bool DestructibleSyncService::replaying_ = false;
    bool DestructibleSyncService::Initialize(PeerRole role, UdpPeer& transport, entities::EntityPresenceReplication& presence,
        entities::EntityNetworkIdentityRegistry& identities, replication::LocalHeroReplication& hero,
        replication::RemotePlayerChannels& peers, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_) return false;
        role_ = role; transport_ = &transport; presence_ = &presence; identities_ = &identities;
        hero_ = &hero; peers_ = &peers; diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        unsigned char prefix[7] = {0x6A, 0xFF, 0x68}; const auto handler = base + 0x0254B458;
        std::memcpy(prefix + 3, &handler, 4);
        active_ = this;
        if (!hook_.Install(reinterpret_cast<void*>(base + 0x01AC7F10), prefix, 7, reinterpret_cast<void*>(&Smash), 7))
        { active_ = nullptr; diagnostics_.Event("MultiplayerDestructibleHookBlocked", "CTCSmashableBase::vf30 bytes mismatch"); return false; }
        original_ = reinterpret_cast<SmashFn>(hook_.Original());
        // CTCWobble::vf7 starts a visual-only half-second oscillation.
        void* expectedWobble = reinterpret_cast<void*>(base + 0x01B29040);
        void* replacementWobble = reinterpret_cast<void*>(&Wobble);
        if (!wobblePatch_.Install(reinterpret_cast<void*>(base + 0x02B0CA54 + 7 * sizeof(void*)),
            &expectedWobble, sizeof(expectedWobble), &replacementWobble, sizeof(replacementWobble)))
        { diagnostics_.Event("MultiplayerDestructibleHookBlocked", "CTCWobble::vf7 slot mismatch"); Shutdown(); return false; }
        originalWobble_ = reinterpret_cast<SmashFn>(expectedWobble);
        diagnostics_.Event("MultiplayerDestructibleHookReady", "native smashable objects replicated through host ledger");
        return true;
    }
    void __fastcall DestructibleSyncService::Smash(void* component, void*)
    {
        auto* self = active_;
        if (!self || !self->original_) return;
        if (!replaying_ && !(self->role_ == PeerRole::Guest && self->checkpoints_ && self->checkpoints_->IsBarrierActive()))
        {
            void* thing = *reinterpret_cast<void**>(static_cast<unsigned char*>(component) + 4);
            const auto* live = self->presence_->LiveEntities().FindByThing(thing);
            if (live)
            {
                Broken b{self->identities_->Canonicalize(live->thingUid), self->checkpoints_ ? self->checkpoints_->WorldFence() : 0,
                    live->mapId, live->definitionIndex};
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->local_.size() < 256) self->local_.push_back(b);
            }
        }
        self->original_(component);
    }
    void __fastcall DestructibleSyncService::Wobble(void* component, void*)
    {
        auto* self = active_; if (!self || !self->originalWobble_) return;
        self->originalWobble_(component);
        if (replaying_ || !self->hero_->IsWorldReady() ||
            (self->checkpoints_ && self->checkpoints_->IsBarrierActive())) return;
        void* thing = *reinterpret_cast<void**>(static_cast<unsigned char*>(component) + 4);
        using namespace game::entity::native;
        if (!ThingComponentAccess::Find(thing, static_cast<ThingComponentType>(0x4E))) return;
        const auto* live = self->presence_->LiveEntities().FindByThing(thing);
        if (!live) return;
        Broken event{self->identities_->Canonicalize(live->thingUid), self->checkpoints_ ? self->checkpoints_->WorldFence() : 0,
            live->mapId, live->definitionIndex, 2};
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->local_.size() < 256) self->local_.push_back(event);
    }
    bool DestructibleSyncService::Apply(const Broken& b)
    {
        if (hero_->MapId() != b.map || !hero_->IsWorldReady()) return false;
        auto uid = identities_->FindLocal(b.uid); if (!uid) uid = b.uid;
        const auto* live = presence_->LiveEntities().Find(uid);
        if (!live || live->definitionIndex != b.definition) return false;
        void* component = game::entity::native::ThingComponentAccess::Find(live->thing,
            static_cast<game::entity::native::ThingComponentType>(0x4E));
        if (!component) return false;
        if (b.operation == 2)
        {
            using namespace game::entity::native;
            if (ThingComponentAccess::Find(live->thing, static_cast<ThingComponentType>(0xF7))) return true;
            replaying_ = true;
            void* wobble = ThingComponentProvisioner::AddNamed(GetModuleHandleW(nullptr), live->thing, "CTCWobble");
            replaying_ = false;
            if (wobble) diagnostics_.Event("MultiplayerDestructibleWobbleApplied", "retail visual-only Wobble component; no repeated OnHit/damage/loot");
            return wobble != nullptr;
        }
        auto** table = *reinterpret_cast<void***>(component);
        replaying_ = true; reinterpret_cast<SmashFn>(table[30])(component); replaying_ = false;
        diagnostics_.Event("MultiplayerDestructibleApplied", "retail smash animation/replacement/destruction applied once");
        return true;
    }
    bool DestructibleSyncService::Process()
    {
        if (!transport_) return true;
        std::deque<Broken> local, inbound;
        { std::lock_guard<std::mutex> lock(mutex_); local.swap(local_); inbound.swap(inbound_); }
        for (const auto& b : local)
        {
            if (checkpoints_ && b.fence != checkpoints_->WorldFence()) continue;
            if (b.operation == 2) { outbound_.push_back(b); continue; }
            locallyBroken_.insert(b.uid);
            if (role_ == PeerRole::Host) committed_[b.uid] = b;
            outbound_.push_back(b);
        }
        for (const auto& b : inbound)
        {
            if (checkpoints_ && b.fence != checkpoints_->WorldFence()) continue;
            if (b.operation == 2)
            {
                Apply(b);
                if (role_ == PeerRole::Host) outbound_.push_back(b);
                continue;
            }
            if (role_ == PeerRole::Host)
            {
                if (committed_.find(b.uid) != committed_.end()) continue;
                if (!locallyBroken_.count(b.uid) && !Apply(b)) continue;
                committed_[b.uid] = b; locallyBroken_.insert(b.uid); outbound_.push_back(b);
            }
            else
            {
                committed_[b.uid] = b;
                // Play the native break visual on receipt, rather than waiting
                // for the half-second repair sweep. Unloaded objects still retry.
                if (!locallyBroken_.count(b.uid) && Apply(b)) locallyBroken_.insert(b.uid);
            }
        }
        if (role_ == PeerRole::Host && peerRevision_ != transport_->PeerSetRevision())
        {
            peerRevision_ = transport_->PeerSetRevision();
            for (const auto& entry : committed_) outbound_.push_back(entry.second);
        }
        if (GetTickCount64() >= nextRepairAt_)
        {
            nextRepairAt_ = GetTickCount64() + 500;
            for (const auto& entry : committed_)
                if (!locallyBroken_.count(entry.first) && Apply(entry.second)) locallyBroken_.insert(entry.first);
        }
        if (checkpoints_ && checkpoints_->IsBarrierActive()) return true;
        for (unsigned int budget = 0; budget < 8 && !outbound_.empty(); ++budget)
        {
            const auto& b = outbound_.front(); unsigned char bytes[21];
            std::memcpy(bytes, &b.uid, 8); std::memcpy(bytes + 8, &b.fence, 8);
            std::memcpy(bytes + 16, &b.map, 2); std::memcpy(bytes + 18, &b.definition, 2); bytes[20] = b.operation;
            if (!transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::Destructible, bytes, sizeof(bytes))) break;
            outbound_.pop_front();
        }
        return true;
    }
    bool DestructibleSyncService::HandleReliableMessage(const TransportMessage& m)
    {
        if (m.payloadSize != 21 || !peers_) return false;
        Broken b; std::memcpy(&b.uid, m.payload.data(), 8); std::memcpy(&b.fence, m.payload.data() + 8, 8);
        std::memcpy(&b.map, m.payload.data() + 16, 2); std::memcpy(&b.definition, m.payload.data() + 18, 2); b.operation = m.payload[20];
        if (!b.uid || !b.map || b.operation < 1 || b.operation > 2) return false;
        if (checkpoints_ && (b.fence != checkpoints_->WorldFence() ||
            (role_ == PeerRole::Host && checkpoints_->IsBarrierActive()))) return true;
        bool authorized = false;
        for (const auto& p : peers_->Snapshots())
            if (p.state.actorId == m.sourceActorId &&
                (role_ == PeerRole::Guest ? p.state.role == PeerRole::Host : p.state.mapId == b.map)) authorized = true;
        if (!authorized) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (inbound_.size() < 256) inbound_.push_back(b);
        return true;
    }
    void DestructibleSyncService::ResetForReload() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        committed_.clear(); locallyBroken_.clear(); inbound_.clear(); outbound_.clear(); local_.clear();
    }
    void DestructibleSyncService::Shutdown() noexcept
    {
        if (!(wobblePatch_.Shutdown() & hook_.Shutdown())) return;
        if (active_ == this) active_ = nullptr;
        original_ = originalWobble_ = nullptr; transport_ = nullptr; ResetForReload();
        checkpoints_ = nullptr; peerRevision_ = nextRepairAt_ = 0;
    }
}

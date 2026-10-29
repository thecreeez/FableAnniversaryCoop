#include "EffectSyncService.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/HeroPawn/Equipment/Native/HeroWeaponComponent.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <intrin.h>

namespace
{
    using Component = fable::game::entity::native::ThingComponentAccess;
    using Type = fable::game::entity::native::ThingComponentType;
    std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }
    void* Parent(void* component) noexcept
    { __try { return *reinterpret_cast<void**>(static_cast<unsigned char*>(component) + 4); } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; } }
    void* HitSource(void* parameters) noexcept
    { __try { auto p = *reinterpret_cast<void***>(static_cast<unsigned char*>(parameters) + 0x5C); return p ? *p : nullptr; } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; } }
    unsigned int Particle(void* component) noexcept
    { __try { return *reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(component) + 0x1C); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; } }
    bool Bone(const fable::game::native::CharString* value, char (&text)[64]) noexcept
    {
        __try
        {
            if (!value || !value->stringData) { text[0] = 0; return true; }
            const auto* rep = static_cast<unsigned char*>(value->stringData);
            const auto length = *reinterpret_cast<const unsigned int*>(rep + 8);
            const auto* data = *reinterpret_cast<const char* const*>(rep + 4);
            if (length >= sizeof(text) || !data || data[length]) return false;
            std::memcpy(text, data, length + 1); return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void HitScope(void(__thiscall* hit)(void*, void*), void* target, void* parameters, bool* scope, bool capture)
    {
        const bool previous = *scope;
        __try { *scope = previous || capture; hit(target, parameters); }
        __finally { *scope = previous; }
    }
    struct ExistingParticle
    {
        void* component = nullptr; void* target = nullptr;
        unsigned int flags = 0; int slot = 0; float extra = 0;
        fable::game::native::CharString* bone = nullptr;
    };
    unsigned int ExistingParticles(void* thing, ExistingParticle (&output)[128]) noexcept
    {
        unsigned int count = 0;
        __try
        {
            auto* attacher = static_cast<unsigned char*>(Component::Find(thing, static_cast<Type>(0xD8)));
            if (!attacher) return 0;
            auto* begin = *reinterpret_cast<unsigned char**>(attacher + 0x10);
            auto* end = *reinterpret_cast<unsigned char**>(attacher + 0x14);
            if (!begin || !end || end < begin || (end - begin) % 8 || end - begin > 4096) return 0;
            for (auto* entry = begin; entry != end && count < 128; entry += 8)
            {
                void* emitter = reinterpret_cast<void*(__thiscall*)(void*)>(Base() + 0x012E6EA0)(entry);
                auto* component = emitter ? static_cast<unsigned char*>(Component::Find(emitter, Type::ParticleEmitter)) : nullptr;
                if (!component || !(component[0x51] & 1)) continue;
                auto& value = output[count++]; value.component = component;
                value.target = reinterpret_cast<void*(__thiscall*)(void*)>(Base() + 0x012E6EA0)(component + 0x28);
                value.flags = *reinterpret_cast<unsigned int*>(component + 0x30);
                value.bone = reinterpret_cast<fable::game::native::CharString*>(component + 0x34);
                value.slot = *reinterpret_cast<int*>(component + 0x38); value.extra = *reinterpret_cast<float*>(component + 0x3C);
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return count; }
        return count;
    }
    fable::multiplayer::ReliableMessageSink* ResolveEffects(fable::multiplayer::MultiplayerSessionContexts& c) noexcept { return &c.world.effects; }
}
FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkEffects, 0x1024u, "visual-effects", 828u,
    "multiplayer-effect-dispatch", ResolveEffects);

namespace fable::multiplayer::presentation
{
    EffectSyncService* EffectSyncService::active_ = nullptr;
    thread_local bool EffectSyncService::replaying_ = false;
    thread_local bool EffectSyncService::impactScope_ = false;
    EffectSyncService::NativeRef::NativeRef()
    { reinterpret_cast<void(__thiscall*)(void*)>(Base() + 0x012E6E90)(value); }
    EffectSyncService::NativeRef::~NativeRef()
    { reinterpret_cast<void(__thiscall*)(void*)>(Base() + 0x012E6F60)(value); }
    void EffectSyncService::NativeRef::Set(void* thing) noexcept
    { reinterpret_cast<void(__thiscall*)(void*, void*)>(Base() + 0x012E6EE0)(value, thing); }
    void* EffectSyncService::NativeRef::Get() noexcept
    { return reinterpret_cast<void*(__thiscall*)(void*)>(Base() + 0x012E6EA0)(value); }

    bool EffectSyncService::Initialize(PeerRole role, MultiplayerSessionContexts& contexts, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_) return false;
        role_ = role; contexts_ = &contexts; diagnostics_ = diagnostics; base_ = Base(); active_ = this;
        const auto install = [this](core::hooking::InlineHook& hook, std::uintptr_t rva, std::uintptr_t handler, void* callback)
        {
            unsigned char prefix[7] = {0x6A, 0xFF, 0x68}; const auto address = base_ + handler;
            std::memcpy(prefix + 3, &address, 4);
            return hook.Install(reinterpret_cast<void*>(base_ + rva), prefix, 7, callback, 7);
        };
        const unsigned char destroyPrefix[] = {0x56, 0x8B, 0xF1, 0x8D, 0x4E, 0x20};
        if (!install(spawn_, 0x01923AA0, 0x024A63C8, reinterpret_cast<void*>(&Spawn)) ||
            !install(materialSpawn_, 0x01925C50, 0x025268EB, reinterpret_cast<void*>(&MaterialSpawn)) ||
            !install(attach_, 0x01924F80, 0x02526897, reinterpret_cast<void*>(&Attach)) ||
            !install(stop_, 0x01924B30, 0x02524740, reinterpret_cast<void*>(&Stop)) ||
            !destroy_.Install(reinterpret_cast<void*>(base_ + 0x01924F30), destroyPrefix, sizeof(destroyPrefix), reinterpret_cast<void*>(&Destroy), sizeof(destroyPrefix)) ||
            !install(objectHit_, 0x01B57A50, 0x02555751, reinterpret_cast<void*>(&ObjectHit)))
        { diagnostics_.Event("MultiplayerEffectHookBlocked", "particle/object-hit native prologue mismatch"); Shutdown(); return false; }
        originalMaterialSpawn_ = reinterpret_cast<MaterialSpawnFn>(materialSpawn_.Original());
        originalSpawn_ = reinterpret_cast<SpawnFn>(spawn_.Original()); originalAttach_ = reinterpret_cast<AttachFn>(attach_.Original());
        originalStop_ = reinterpret_cast<StopFn>(stop_.Original()); originalDestroy_ = reinterpret_cast<StopFn>(destroy_.Original());
        originalHit_ = reinterpret_cast<HitFn>(objectHit_.Original());
        diagnostics_.Event("MultiplayerEffectSyncReady", "owner Hero/weapon particle lifecycle and destructible impact visuals; no replicated gameplay mutations");
        return true;
    }
    void* __cdecl EffectSyncService::Spawn(unsigned int particle, const game::Vector3* position, unsigned char independent)
    {
        auto* self = active_; if (!self || !self->originalSpawn_) return nullptr;
        void* result = self->originalSpawn_(particle, position, independent);
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - self->base_;
        // Derived SmashableBarrier::vf30 already runs on the receiving peer.
        // Its break particle must not also become an impact burst packet.
        if (!replaying_ && impactScope_ && result && position && !(caller >= 0x01AC7BA0 && caller < 0x01AC7C5C))
            self->ObserveImpact(result, particle, *position);
        return result;
    }
    // Material hit visuals use (resource, variation, strength, position, independent).
    // This path does not call the ordinary particle-definition spawn function.
    void* __cdecl EffectSyncService::MaterialSpawn(unsigned int resource, unsigned int variation, float strength,
        const game::Vector3* position, unsigned char independent)
    {
        auto* self = active_; if (!self || !self->originalMaterialSpawn_) return nullptr;
        void* result = self->originalMaterialSpawn_(resource, variation, strength, position, independent);
        if (!replaying_ && impactScope_ && result && position)
            self->ObserveImpact(result, resource, *position, 1, variation, strength);
        return result;
    }
    void __fastcall EffectSyncService::Attach(void* component, void*, void* target, unsigned int flags,
        const game::native::CharString* bone, int slot, float extra)
    {
        auto* self = active_; if (!self || !self->originalAttach_) return;
        self->originalAttach_(component, target, flags, bone, slot, extra);
        if (!replaying_) self->ObserveAttach(component, target, flags, bone, slot, extra);
    }
    void __fastcall EffectSyncService::Stop(void* component, void*)
    {
        auto* self = active_; if (!self || !self->originalStop_) return;
        if (!replaying_) self->ObserveStop(component);
        self->originalStop_(component);
    }
    void __fastcall EffectSyncService::Destroy(void* component, void*)
    {
        auto* self = active_; if (!self || !self->originalDestroy_) return;
        if (!replaying_) self->ObserveStop(component);
        self->originalDestroy_(component);
    }
    void __fastcall EffectSyncService::ObjectHit(void* target, void*, void* parameters)
    {
        auto* self = active_; if (!self || !self->originalHit_) return;
        bool capture = false;
        if (!replaying_ && self->contexts_ && !self->contexts_->world.checkpoints.IsBarrierActive() &&
            Component::Find(target, static_cast<Type>(0x4E)))
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            const auto source = self->bindings_.find(HitSource(parameters));
            capture = source != self->bindings_.end() && source->second.local;
        }
        HitScope(self->originalHit_, target, parameters, &impactScope_, capture);
    }
    bool EffectSyncService::SameVisual(const Wire& a, const Wire& b) const noexcept
    {
        return a.actor == b.actor && a.authorityEpoch == b.authorityEpoch && a.generation == b.generation && a.mapEpoch == b.mapEpoch &&
            a.worldFence == b.worldFence && a.map == b.map && a.attachment == b.attachment && a.particle == b.particle &&
            a.kind == b.kind && a.secondary == b.secondary && a.strength == b.strength && a.flags == b.flags && a.slot == b.slot && a.extra == b.extra && std::strcmp(a.bone, b.bone) == 0;
    }
    void EffectSyncService::ObserveAttach(void* component, void* target, unsigned int flags,
        const game::native::CharString* bone, int slot, float extra)
    {
        if (!contexts_ || contexts_->world.checkpoints.IsBarrierActive()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto binding = bindings_.find(target); if (binding == bindings_.end()) return;
        Wire state = binding->second.identity;
        state.particle = Particle(component); state.flags = flags; state.slot = slot; state.extra = extra;
        state.instance = ++instance_; state.revision = ++revision_;
        if (!Bone(bone, state.bone) || !protocol::ValidEffectMessage(state)) return;
        const auto previous = sources_.find(component);
        if (previous != sources_.end() && SameVisual(previous->second->state, state)) return;
        const auto observed = observed_.find(component);
        if (observed != observed_.end() && SameVisual(observed->second->state, state)) return;
        auto record = std::make_unique<Record>(); record->state = state;
        record->receivedAt = GetTickCount64(); record->emitter.Set(Parent(component));
        impactCandidates_.erase(component);
        if (binding->second.local)
        {
            if (sources_.size() >= 1024 || local_.size() >= 2048) return;
            sources_[component] = std::move(record); local_.push_back(state);
        }
        else
        {
            // Native action replay can already have produced the same emitter.
            // Reuse it when the owner baseline arrives; discard a later duplicate.
            for (const auto& entry : replicas_)
                if (entry.second && SameVisual(entry.second->state, state) && entry.second->emitter.Get() &&
                    entry.second->emitter.Get() == Parent(component)) return;
            for (const auto& entry : replicas_)
                if (entry.second && SameVisual(entry.second->state, state) && entry.second->emitter.Get() &&
                    !entry.second->borrowed && GetTickCount64() - entry.second->receivedAt < 500)
                { DestroyEmitter(*record); return; }
            if (observed_.size() < 1024) observed_[component] = std::move(record);
        }
    }
    void EffectSyncService::ObserveStop(void* component)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto source = sources_.find(component);
        if (source != sources_.end())
        {
            Wire state = source->second->state; state.operation = protocol::EffectOperation::Stop; state.revision = ++revision_;
            if (local_.size() < 2048) local_.push_back(state);
            sources_.erase(source);
        }
        observed_.erase(component); impactCandidates_.erase(component);
    }
    void EffectSyncService::ObserveImpact(void* thing, unsigned int particle, const game::Vector3& position,
        unsigned int kind, unsigned int secondary, float strength)
    {
        void* component = Component::Find(thing, Type::ParticleEmitter); if (!component) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto root = bindings_.find(contexts_->players.localHero.NativeHero()); if (root == bindings_.end()) return;
        Wire state = root->second.identity; state.instance = ++instance_; state.revision = ++revision_;
        state.kind = kind; state.secondary = secondary; state.strength = strength;
        state.particle = particle; state.position = position; state.operation = protocol::EffectOperation::Impact;
        state.attachment = protocol::EffectAttachment::World;
        if (protocol::ValidEffectMessage(state) && impactCandidates_.size() < 256) impactCandidates_[component] = state;
    }
    bool EffectSyncService::MakeEmitter(Record& record, void* target) noexcept
    {
        bool okay = false; const bool previous = replaying_; replaying_ = true;
        game::native::CharString bone;
        __try
        {
            void* thing = record.state.kind == 1
                ? originalMaterialSpawn_(record.state.particle, record.state.secondary, record.state.strength, &record.state.position, 1)
                : originalSpawn_(record.state.particle, &record.state.position,
                    record.state.operation == protocol::EffectOperation::Impact ? 1 : 0);
            if (thing)
            {
                record.emitter.Set(thing);
                if (target)
                {
                    void* component = Component::Find(thing, Type::ParticleEmitter);
                    if (component)
                    {
                        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(base_ + 0x012B7800)(&bone, record.state.bone, -1);
                        originalAttach_(component, target, record.state.flags, &bone, record.state.slot, record.state.extra);
                        okay = true;
                    }
                }
                else okay = record.state.operation == protocol::EffectOperation::Impact;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { okay = false; }
        if (bone.stringData) reinterpret_cast<void(__thiscall*)(void*)>(base_ + 0x012B75D0)(&bone);
        replaying_ = previous;
        if (!okay) DestroyEmitter(record);
        return okay;
    }
    void EffectSyncService::DestroyEmitter(Record& record) noexcept
    {
        const bool previous = replaying_; replaying_ = true;
        __try
        {
            if (void* thing = record.emitter.Get())
                reinterpret_cast<void(__thiscall*)(void*, unsigned char)>(base_ + 0x01B2E530)(thing, 0);
            record.emitter.Set(nullptr);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        replaying_ = previous;
    }
    void EffectSyncService::RefreshBindings()
    {
        auto& c = *contexts_;
        std::lock_guard<std::mutex> lock(mutex_); bindings_.clear();
        const auto bind = [this, &c](void* thing, const PlayerState& state, bool local)
        {
            if (!thing) return;
            Binding binding; binding.local = local; auto& id = binding.identity;
            id.actor = state.actorId; id.authorityEpoch = state.authorityEpoch; id.generation = state.actorGeneration;
            id.mapEpoch = state.mapEpoch; id.map = state.mapId; id.position = state.position;
            id.worldFence = c.world.checkpoints.WorldFence(); bindings_[thing] = binding;
            game::hero_pawn::equipment::native::HeroWeaponInspection weapons;
            if (game::hero_pawn::equipment::native::HeroWeaponComponent::Inspect(thing, weapons))
            {
                if (weapons.meleeWeapon) { binding.identity.attachment = protocol::EffectAttachment::Melee; bindings_[weapons.meleeWeapon] = binding; }
                if (weapons.rangedWeapon) { binding.identity.attachment = protocol::EffectAttachment::Ranged; bindings_[weapons.rangedWeapon] = binding; }
            }
        };
        if (const auto* local = c.players.localHero.CurrentState()) bind(c.players.localHero.NativeHero(), *local, true);
        for (const auto& peer : c.transport.remotePlayerChannels.Snapshots())
            bind(c.players.playerCombatants.FindCreature(peer.state.actorId), peer.state, false);
    }
    bool EffectSyncService::IsCurrent(const Wire& s) const
    {
        const auto* local = contexts_->players.localHero.CurrentState();
        const auto* state = local && local->actorId == s.actor ? local : contexts_->transport.remotePlayerChannels.Find(s.actor);
        return state && state->authorityEpoch == s.authorityEpoch && state->actorGeneration == s.generation &&
            state->mapEpoch == s.mapEpoch && state->mapId == s.map && s.worldFence == contexts_->world.checkpoints.WorldFence();
    }
    void EffectSyncService::CaptureExistingEmitters()
    {
        std::vector<void*> things;
        { std::lock_guard<std::mutex> lock(mutex_); for (const auto& entry : bindings_) things.push_back(entry.first); }
        for (void* thing : things)
        {
            ExistingParticle particles[128];
            const auto count = ExistingParticles(thing, particles);
            for (unsigned int i = 0; i < count; ++i)
                ObserveAttach(particles[i].component, particles[i].target, particles[i].flags, particles[i].bone, particles[i].slot, particles[i].extra);
        }
    }
    bool EffectSyncService::Send(const Wire& state)
    {
        return contexts_->transport.transport.SubmitReliable(reliable_stream::Control, protocol::PacketType::Effect,
            reinterpret_cast<const std::uint8_t*>(&state), sizeof(state));
    }
    bool EffectSyncService::HandleReliableMessage(const TransportMessage& m)
    {
        Wire state; if (!protocol::DecodeEffectMessage(m.payload.data(), m.payloadSize, state)) return false;
        if (!contexts_ || contexts_->world.checkpoints.IsBarrierActive() || !IsCurrent(state)) return true;
        bool authorized = false;
        for (const auto& peer : contexts_->transport.remotePlayerChannels.Snapshots())
            if (peer.state.actorId == m.sourceActorId &&
                (role_ == PeerRole::Host ? peer.state.role == PeerRole::Guest && state.actor == m.sourceActorId : peer.state.role == PeerRole::Host)) authorized = true;
        if (!authorized) return true;
        const auto now = contexts_->transport.transport.SessionTimeMilliseconds();
        if (state.operation != protocol::EffectOperation::Stop && contexts_->transport.transport.HasSynchronizedSessionClock() &&
            now > state.sampledAt && now - state.sampledAt > 3000) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (inbound_.size() < 2048) inbound_.push_back(state);
        return true;
    }
    bool EffectSyncService::Process()
    {
        if (!contexts_) return true;
        auto& c = *contexts_;
        if (c.world.checkpoints.IsBarrierActive()) return true;
        RefreshBindings();
        if (GetTickCount64() >= nextCapture_) { nextCapture_ = GetTickCount64() + 1000; CaptureExistingEmitters(); }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = GetTickCount64();
        for (const auto& impact : impactCandidates_) local_.push_back(impact.second);
        impactCandidates_.clear();
        for (auto it = sources_.begin(); it != sources_.end();)
        {
            if (!it->second->emitter.Get() || !IsCurrent(it->second->state))
            {
                Wire state = it->second->state; state.operation = protocol::EffectOperation::Stop; state.revision = ++revision_;
                local_.push_back(state); it = sources_.erase(it);
            }
            else ++it;
        }
        for (auto it = observed_.begin(); it != observed_.end();)
            if (!it->second->emitter.Get() || now - it->second->receivedAt > 5000) it = observed_.erase(it); else ++it;
        for (auto& state : local_)
        {
            if (!IsCurrent(state)) continue;
            state.sampledAt = c.transport.transport.SessionTimeMilliseconds();
            const Key key{state.actor, state.instance, state.authorityEpoch, state.generation, state.mapEpoch};
            if (state.operation == protocol::EffectOperation::Start)
            { if (desired_.size() < 4096 || desired_.count(key)) desired_[key] = state; }
            else desired_.erase(key);
            if (outbound_.size() < 4096) outbound_.push_back(state);
        }
        local_.clear();
        const auto* local = c.players.localHero.CurrentState();
        for (const auto& state : inbound_)
        {
            if (!IsCurrent(state)) continue;
            const Key key{state.actor, state.instance, state.authorityEpoch, state.generation, state.mapEpoch};
            if (seen_[key] >= state.revision) continue;
            seen_[key] = state.revision;
            if (role_ == PeerRole::Host && outbound_.size() < 4096) outbound_.push_back(state);
            if (local && state.actor == local->actorId) continue;
            if (state.operation == protocol::EffectOperation::Start)
            { if (desired_.size() < 4096 || desired_.count(key)) desired_[key] = state; }
            else
            {
                desired_.erase(key);
                const auto replica = replicas_.find(key);
                if (replica != replicas_.end()) { if (replica->second) DestroyEmitter(*replica->second); replicas_.erase(replica); }
                if (state.operation == protocol::EffectOperation::Impact && local && local->mapId == state.map && c.players.localHero.IsWorldReady())
                {
                    Record burst; burst.state = state;
                    if (MakeEmitter(burst, nullptr)) diagnostics_.Event("MultiplayerDestructibleImpactEffectApplied", "native visual-only particle; no repeated hit/damage/drop");
                }
            }
        }
        inbound_.clear();
        for (auto it = replicas_.begin(); it != replicas_.end();)
        {
            if (!it->second) { it = replicas_.erase(it); continue; }
            if (!local || !c.players.localHero.IsWorldReady() || it->second->state.map != local->mapId ||
                !IsCurrent(it->second->state) || !c.transport.transport.HasPeer())
            { DestroyEmitter(*it->second); it = replicas_.erase(it); }
            else ++it;
        }
        for (auto it = desired_.begin(); it != desired_.end();)
        {
            const auto& state = it->second;
            if (!IsCurrent(state)) { it = desired_.erase(it); continue; }
            if (local && c.transport.transport.HasPeer() && state.actor != local->actorId &&
                local->mapId == state.map && c.players.localHero.IsWorldReady())
            {
                auto& replica = replicas_[it->first];
                if (!replica || !replica->emitter.Get())
                {
                    replica.reset();
                    const auto observed = std::find_if(observed_.begin(), observed_.end(), [this, &state](const auto& entry) { return SameVisual(entry.second->state, state); });
                    if (observed != observed_.end())
                    { replica = std::move(observed->second); observed_.erase(observed); replica->state = state; replica->borrowed = true; }
                    else
                    {
                        void* target = nullptr;
                        for (const auto& binding : bindings_)
                            if (binding.second.identity.actor == state.actor && binding.second.identity.attachment == state.attachment) { target = binding.first; break; }
                        if (target)
                        {
                            auto created = std::make_unique<Record>(); created->state = state; created->receivedAt = now;
                            if (MakeEmitter(*created, target))
                            { replica = std::move(created); diagnostics_.Event("MultiplayerPlayerEffectApplied", "native particle attached to current remote Hero/weapon; source lifetime retained"); }
                        }
                    }
                }
            }
            ++it;
        }
        const auto peerRevision = c.transport.transport.PeerSetRevision();
        if (now >= nextBaseline_ || peerRevision != peerRevision_)
        {
            nextBaseline_ = now + 1000; peerRevision_ = peerRevision;
            for (const auto& entry : desired_)
                if ((role_ == PeerRole::Host || (local && entry.second.actor == local->actorId)) && outbound_.size() < 4096)
                { Wire state = entry.second; state.sampledAt = c.transport.transport.SessionTimeMilliseconds(); outbound_.push_back(state); }
        }
        for (unsigned int budget = 0; budget < 32 && !outbound_.empty(); ++budget)
        { if (!Send(outbound_.front())) break; outbound_.pop_front(); }
        // Starts older than three seconds are rejected above, so retain only a
        // bounded replay window for already stopped/one-shot instance IDs.
        if (seen_.size() > 16384)
            for (auto it = seen_.begin(); it != seen_.end() && seen_.size() > 8192;)
                if (!desired_.count(it->first)) it = seen_.erase(it); else ++it;
        return true;
    }
    void EffectSyncService::ResetForReload() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : replicas_) if (entry.second) DestroyEmitter(*entry.second);
        replicas_.clear(); desired_.clear(); observed_.clear(); sources_.clear(); impactCandidates_.clear(); bindings_.clear();
        local_.clear(); inbound_.clear(); outbound_.clear(); seen_.clear(); nextBaseline_ = nextCapture_ = 0;
    }
    void EffectSyncService::Shutdown() noexcept
    {
        ResetForReload();
        if (!(objectHit_.Shutdown() & destroy_.Shutdown() & stop_.Shutdown() & attach_.Shutdown() & materialSpawn_.Shutdown() & spawn_.Shutdown())) return;
        if (active_ == this) active_ = nullptr;
        originalSpawn_ = nullptr; originalMaterialSpawn_ = nullptr; originalAttach_ = nullptr; originalStop_ = originalDestroy_ = nullptr; originalHit_ = nullptr;
        contexts_ = nullptr; instance_ = revision_ = peerRevision_ = 0;
    }
}

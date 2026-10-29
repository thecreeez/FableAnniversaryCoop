#include "WorldClockSyncService.h"
#include "Core/Target/ExecutableValidator.h"
#include "Game/Native/Addresses.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <intrin.h>

namespace
{
    using State = fable::multiplayer::protocol::WorldClockMessage;
    // CGameTimeManager: scalar state only. Never transfer its owning token tree at+30.
    bool ReadNative(void* clock, State& s) noexcept
    {
        __try
        {
            const auto p = static_cast<const unsigned char*>(clock);
            s.fraction = *reinterpret_cast<const float*>(p + 8);
            s.previous = *reinterpret_cast<const float*>(p + 12);
            s.rate = *reinterpret_cast<const float*>(p + 16);
            s.day = *reinterpret_cast<const int*>(p + 20);
            s.restricted = p[24]; s.accumulated = *reinterpret_cast<const float*>(p + 28);
            s.restriction = p[32]; s.jumping = p[33];
            s.target = *reinterpret_cast<const float*>(p + 36);
            s.remaining = s.jumping ? *reinterpret_cast<const float*>(p + 40) : 0;
            s.frozen = *reinterpret_cast<const unsigned int*>(p + 52) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool WriteNative(void* clock, const State& s) noexcept
    {
        __try
        {
            auto p = static_cast<unsigned char*>(clock);
            *reinterpret_cast<float*>(p + 8) = s.fraction;
            *reinterpret_cast<float*>(p + 12) = s.previous;
            *reinterpret_cast<float*>(p + 16) = s.rate;
            *reinterpret_cast<int*>(p + 20) = s.day;
            p[24] = s.restricted; *reinterpret_cast<float*>(p + 28) = s.accumulated;
            p[32] = s.restriction; p[33] = s.jumping;
            *reinterpret_cast<float*>(p + 36) = s.target;
            *reinterpret_cast<float*>(p + 40) = s.remaining;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    fable::multiplayer::ReliableMessageSink* ResolveClock(fable::multiplayer::MultiplayerSessionContexts& c) noexcept
    { return &c.world.worldClock; }
}
FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkWorldClock, 0x1020u, "world-clock", 824u,
    "multiplayer-world-clock-dispatch", ResolveClock);

namespace fable::multiplayer::world
{
    WorldClockSyncService* WorldClockSyncService::active_ = nullptr;

    bool WorldClockSyncService::Initialize(const PeerRole role, UdpPeer& transport,
        replication::LocalHeroReplication& hero, replication::RemotePlayerChannels& peers,
        const std::uint64_t epoch, const core::Diagnostics& diagnostics) noexcept
    {
        if (tick_.IsInstalled() || active_ != nullptr) return false;
        diagnostics_ = diagnostics;
        const HMODULE module = GetModuleHandleW(nullptr);
        if (!core::target::ValidateFableExecutable(module, nullptr)) return false;
        base_ = reinterpret_cast<std::uintptr_t>(module);
        role_ = role; transport_ = &transport; hero_ = &hero; peers_ = &peers; epoch_ = epoch;
        // CGameTimeManager::Update(float), RVA018B85C0: full instruction boundaries.
        const unsigned char prefix[] = {0x80, 0x79, 0x21, 0x00, 0xF3, 0x0F, 0x10, 0x41, 0x08};
        active_ = this;
        if (!tick_.Install(reinterpret_cast<void*>(base_ + 0x018B85C0), prefix, sizeof(prefix),
                reinterpret_cast<void*>(&Tick), sizeof(prefix)))
        {
            active_ = nullptr;
            diagnostics_.Event("MultiplayerWorldClockHookBlocked", "native clock tick prologue mismatch");
            return false;
        }
        original_ = reinterpret_cast<TickFn>(tick_.Original());
        const unsigned char jumpPrefix[] = {0x0F, 0x57, 0xC9, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04};
        if (!jump_.Install(reinterpret_cast<void*>(base_ + 0x018B83B0), jumpPrefix, sizeof(jumpPrefix),
            reinterpret_cast<void*>(&Jump), sizeof(jumpPrefix)))
        { diagnostics_.Event("MultiplayerSleepHookBlocked", "native time-jump prologue mismatch"); Shutdown(); return false; }
        originalJump_ = reinterpret_cast<JumpFn>(jump_.Original());
        diagnostics_.Event("MultiplayerWorldClockHookReady", "host-authoritative native world clock");
        return true;
    }

    void __fastcall WorldClockSyncService::Jump(void* clock, void*, float fraction, float duration)
    {
        auto* self = active_;
        if (!self || !self->originalJump_) return;
        // CTCActionUseBed::vf28 calls CGameTimeManager::Jump at01929F4E (return01929F53).
        // Guest quest scripts are replicas, not independent authors of global campaign time.
        if (self->role_ == PeerRole::Guest &&
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == self->base_ + 0x01929F53)
        {
            if (self->checkpoints_ && self->checkpoints_->IsBarrierActive()) return;
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->outgoingSleep_ = {++self->nextSleepRevision_, self->incoming_.epoch, fraction, duration, self->hero_->MapId()};
            self->outgoingSleep_.fence = self->checkpoints_ ? self->checkpoints_->WorldFence() : 0;
            self->diagnostics_.Event("MultiplayerGuestSleepRequested", "retail bed requested host-authoritative time jump");
            return;
        }
        self->originalJump_(clock, fraction, duration);
        self->nextSend_ = 0;
    }

    void* WorldClockSyncService::Clock() const noexcept
    {
        if (!base_) return nullptr;
        __try
        {
            const auto manager = *reinterpret_cast<std::uintptr_t*>(base_ + game::native::rva::ThingManagerSlot);
            if (!manager) return nullptr;
            const auto world = *reinterpret_cast<std::uintptr_t*>(manager + 0x30);
            return world ? *reinterpret_cast<void**>(world + 0x1C) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void __fastcall WorldClockSyncService::Tick(void* clock, void*, const float dt)
    {
        auto* self = active_;
        if (!self || !self->original_) return;
        bool overrideFreeze = false;
        unsigned char frozen = 0;
        if (self->role_ == PeerRole::Host) self->lastNativeTickAt_.store(GetTickCount64(), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            overrideFreeze = self->role_ == PeerRole::Guest && self->appliedRevision_ != 0 &&
                GetTickCount64() - self->receivedAt_ < 5000 && clock == self->Clock();
            frozen = self->authoritative_.frozen;
        }
        if (overrideFreeze && (frozen & 2)) return;
        // Update only does arithmetic; temporarily supply host freeze count without touching
        // the guest's token tree. Restore before any other native subsystem runs.
        auto* count = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(clock) + 0x34);
        const auto localCount = *count;
        if (overrideFreeze) *count = (frozen & 1) ? 1 : 0;
        self->original_(clock, dt);
        if (overrideFreeze) *count = localCount;
    }

    bool WorldClockSyncService::Process()
    {
        if (!original_ || !transport_ || !hero_->IsWorldReady()) return true;
        void* clock = Clock();
        if (!clock) return true;
        const auto now = GetTickCount64();
        if (role_ == PeerRole::Host)
        {
            SleepRequest sleep;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pendingSleeps_.empty()) { sleep = pendingSleeps_.front(); pendingSleeps_.pop_front(); }
            }
            if (sleep.revision && sleep.epoch == epoch_ && now - lastSleepAcceptedAt_ >= 1500 &&
                (!checkpoints_ || (sleep.fence == checkpoints_->WorldFence() && !checkpoints_->IsBarrierActive())))
            {
                originalJump_(clock, sleep.fraction, sleep.duration);
                lastSleepAcceptedAt_ = now; nextSend_ = 0;
                diagnostics_.Event("MultiplayerGuestSleepAppliedByHost", "retail time jump accepted; broadcast new day/time to every peer");
            }
            if (!transport_->HasPeer() || now < nextSend_) return true;
            State s;
            if (!ReadNative(clock, s))
            {
                char detail[80]; std::snprintf(detail, sizeof(detail), "clock=%p", clock);
                diagnostics_.Event("MultiplayerWorldClockReadBlocked", detail);
                nextSend_ = now + 1000;
                return false;
            }
            s.epoch = epoch_; s.revision = ++revision_; s.sampledAt = transport_->SessionTimeMilliseconds();
            if (now - lastNativeTickAt_.load(std::memory_order_relaxed) > 250) s.frozen |= 2;
            unsigned char bytes[protocol::WorldClockBytes];
            if (!protocol::EncodeWorldClock(s, bytes, sizeof(bytes)))
            {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "day=%d fraction=%g previous=%g rate=%g accumulated=%g target=%g remaining=%g restricted=%u restriction=%u jumping=%u frozen=%u epoch=%llu stamp=%llu",
                    s.day, s.fraction, s.previous, s.rate, s.accumulated, s.target, s.remaining,
                    s.restricted, s.restriction, s.jumping, s.frozen, s.epoch, s.sampledAt);
                diagnostics_.Event("MultiplayerWorldClockInvalidNativeState", detail);
                nextSend_ = now + 1000;
                return false;
            }
            if (!transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::WorldClock, bytes, sizeof(bytes)))
            {
                diagnostics_.Event("MultiplayerWorldClockSendDeferred", "reliable control stream backpressure");
                nextSend_ = now + 100;
                return true;
            }
            nextSend_ = now + 100;
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outgoingSleep_.revision && incoming_.epoch)
            {
                outgoingSleep_.epoch = incoming_.epoch;
                unsigned char bytes[34];
                std::memcpy(bytes, &outgoingSleep_.revision, 8); std::memcpy(bytes + 8, &outgoingSleep_.epoch, 8);
                std::memcpy(bytes + 16, &outgoingSleep_.fraction, 4); std::memcpy(bytes + 20, &outgoingSleep_.duration, 4);
                std::memcpy(bytes + 24, &outgoingSleep_.map, 2);
                std::memcpy(bytes + 26, &outgoingSleep_.fence, 8);
                if (transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::SleepRequest, bytes, sizeof(bytes))) outgoingSleep_ = {};
            }
        }
        State state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (incoming_.revision == 0 || incoming_.revision == appliedRevision_ || now - receivedAt_ > 2000)
                return true;
            state = incoming_;
            authoritative_ = state;
            appliedRevision_ = state.revision;
        }
        if (!WriteNative(clock, state)) return false;
        // Run the retail interpolation/rollover, with bounded host-clock transit compensation.
        const auto sessionNow = transport_->SessionTimeMilliseconds();
        const auto elapsed = sessionNow > state.sampledAt ? sessionNow - state.sampledAt : 0;
        const float advance = static_cast<float>(elapsed < 250 ? elapsed : 250) / 1000.0f;
        if (advance > 0) Tick(clock, nullptr, advance);
        if (state.revision == 1 || state.revision % 50 == 0)
        {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "revision=%llu day=%d fraction=%.6f frozen=%u jumping=%u map_id=%u",
                state.revision, state.day, state.fraction, state.frozen, state.jumping, hero_->MapId());
            diagnostics_.Event("MultiplayerWorldClockApplied", detail);
        }
        return true;
    }

    bool WorldClockSyncService::HandleReliableMessage(const TransportMessage& message)
    {
        if (message.type == protocol::PacketType::SleepRequest)
        {
            if (role_ != PeerRole::Host || !peers_ || message.payloadSize != 34) return true;
            SleepRequest s;
            const auto* bytes = message.payload.data();
            std::memcpy(&s.revision, bytes, 8); std::memcpy(&s.epoch, bytes + 8, 8);
            std::memcpy(&s.fraction, bytes + 16, 4); std::memcpy(&s.duration, bytes + 20, 4); std::memcpy(&s.map, bytes + 24, 2);
            std::memcpy(&s.fence, bytes + 26, 8);
            if (checkpoints_ && (s.fence != checkpoints_->WorldFence() || checkpoints_->IsBarrierActive())) return true;
            if (!s.revision || s.epoch != epoch_ || !std::isfinite(s.fraction) || s.fraction < 0 || s.fraction > 1 ||
                !std::isfinite(s.duration) || s.duration < 0 || s.duration > 30) return true;
            bool known = false;
            for (const auto& peer : peers_->Snapshots())
                if (peer.state.actorId == message.sourceActorId && peer.state.role == PeerRole::Guest && peer.state.mapId == s.map) known = true;
            if (!known) return true;
            std::lock_guard<std::mutex> lock(mutex_);
            auto& seen = sleepRevisions_[message.sourceActorId];
            if (seen.first == message.connectionNonce && s.revision <= seen.second) return true;
            seen = {message.connectionNonce, s.revision};
            if (pendingSleeps_.size() < 16) pendingSleeps_.push_back(s);
            return true;
        }
        if (role_ != PeerRole::Guest || !peers_) return true;
        // Only the roster's session host can author campaign time. Never latch the first sender.
        std::uint64_t host = 0;
        for (const auto& peer : peers_->Snapshots())
            if (peer.state.role == PeerRole::Host) { host = peer.state.actorId; break; }
        if (!host || message.sourceActorId != host) return true;
        State s;
        if (!protocol::DecodeWorldClock(message.payload.data(), message.payloadSize, s)) return false;
        const auto sessionNow = transport_->SessionTimeMilliseconds();
        if (transport_->HasSynchronizedSessionClock() && sessionNow > s.sampledAt && sessionNow - s.sampledAt > 2000)
            return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (hostActor_ != host || hostNonce_ != message.connectionNonce)
        {
            hostActor_ = host; hostNonce_ = message.connectionNonce;
            incoming_ = {}; appliedRevision_ = 0;
        }
        if (incoming_.revision && (s.epoch != incoming_.epoch || s.revision <= incoming_.revision)) return true;
        incoming_ = s; receivedAt_ = GetTickCount64();
        return true;
    }

    bool WorldClockSyncService::RequestSleepForTest(float fraction) noexcept
    {
        if (role_ != PeerRole::Guest || !hero_ || !hero_->IsWorldReady() || !std::isfinite(fraction) || fraction < 0 || fraction > 1) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!incoming_.epoch) return false;
        outgoingSleep_ = {++nextSleepRevision_, incoming_.epoch, fraction, 0, hero_->MapId()};
        outgoingSleep_.fence = checkpoints_ ? checkpoints_->WorldFence() : 0;
        return true;
    }

    bool WorldClockSyncService::ReadForTest(State& s) noexcept
    { void* clock = Clock(); return clock && ReadNative(clock, s); }

    void WorldClockSyncService::AdvanceForTest(const float seconds) noexcept
    {
        // Acceptance runs alongside a real open quest inventory, which stops CWorld::Update.
        // Exercise the actual retail clock arithmetic on the host, never fabricate guest state.
        void* clock = Clock();
        if (clock && original_ && role_ == PeerRole::Host && seconds > 0 && seconds <= 0.25f)
            Tick(clock, nullptr, seconds);
    }

    bool WorldClockSyncService::IsNightForTest() noexcept
    {
        void* clock = Clock();
        if (!clock) return false;
        using IsNight = bool (__thiscall*)(void*);
        return reinterpret_cast<IsNight>(base_ + 0x018B8410)(clock);
    }

    bool WorldClockSyncService::SetForTest(const float fraction, const float rate, const int day, const float duration) noexcept
    {
        void* clock = Clock();
        State s;
        if (!clock || !ReadNative(clock, s)) return false;
        s.rate = rate; s.day = day; s.jumping = 0; s.restricted = 0;
        if (!WriteNative(clock, s)) return false;
        using Jump = void (__thiscall*)(void*, float, float);
        reinterpret_cast<Jump>(base_ + 0x018B83B0)(clock, fraction, duration);
        *reinterpret_cast<int*>(static_cast<unsigned char*>(clock) + 20) = day;
        nextSend_ = 0;
        return true;
    }

    bool WorldClockSyncService::FreezeForTest(const bool freeze) noexcept
    {
        void* clock = Clock();
        if (!clock || (testFreezeClock_ && testFreezeClock_ != clock)) return false;
        using Freeze = void (__thiscall*)(void*, unsigned char, int*);
        if (freeze && !testFreezeClock_)
        {
            reinterpret_cast<Freeze>(base_ + 0x018B8980)(clock, 1, &testFreezeToken_);
            testFreezeClock_ = clock;
        }
        if (!freeze && testFreezeClock_)
        {
            reinterpret_cast<Freeze>(base_ + 0x018B8980)(clock, 0, &testFreezeToken_);
            testFreezeClock_ = nullptr;
        }
        return true;
    }

    void WorldClockSyncService::ResetForReload() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_ = {}; authoritative_ = {}; appliedRevision_ = receivedAt_ = nextSend_ = 0;
        outgoingSleep_ = {}; pendingSleeps_.clear();
    }

    void WorldClockSyncService::Shutdown() noexcept
    {
        if (testFreezeClock_ && testFreezeClock_ == Clock()) FreezeForTest(false);
        if (!(jump_.Shutdown() & tick_.Shutdown())) return;
        if (active_ == this) active_ = nullptr;
        original_ = nullptr; transport_ = nullptr; hero_ = nullptr; peers_ = nullptr; base_ = 0;
        originalJump_ = nullptr;
        checkpoints_ = nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_ = {}; authoritative_ = {}; epoch_ = revision_ = nextSend_ = 0;
        hostActor_ = hostNonce_ = receivedAt_ = appliedRevision_ = 0;
        testFreezeClock_ = nullptr; testFreezeToken_ = 0;
        lastNativeTickAt_.store(0, std::memory_order_relaxed);
        outgoingSleep_ = {}; pendingSleeps_.clear(); sleepRevisions_.clear(); nextSleepRevision_ = lastSleepAcceptedAt_ = 0;
    }
}

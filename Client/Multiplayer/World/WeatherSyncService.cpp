#include "WeatherSyncService.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    void NativeWeatherTick(void(__thiscall* tick)(void*, unsigned char), void* renderer, unsigned char update, bool apply)
    {
        auto* defaults = static_cast<unsigned char*>(renderer) + 0x119;
        const auto saved = *defaults;
        __try { if (apply) *defaults = 1; tick(renderer, update); }
        __finally { *defaults = saved; }
    }
    fable::multiplayer::ReliableMessageSink* ResolveWeather(fable::multiplayer::MultiplayerSessionContexts& c) noexcept
    { return &c.world.weather; }
    bool SameWeather(const fable::multiplayer::protocol::WeatherMessage& a,
        const fable::multiplayer::protocol::WeatherMessage& b) noexcept
    {
        return a.map == b.map && a.mapEpoch == b.mapEpoch && a.actorGeneration == b.actorGeneration &&
            a.worldFence == b.worldFence && a.enabled == b.enabled &&
            std::fabs(a.rain - b.rain) < 0.0001f && std::fabs(a.snow - b.snow) < 0.0001f && std::fabs(a.mist - b.mist) < 0.0001f;
    }
}
FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkWeather, 0x1023u, "regional-weather", 827u,
    "multiplayer-weather-dispatch", ResolveWeather);

namespace fable::multiplayer::world
{
    WeatherSyncService* WeatherSyncService::active_ = nullptr;
    bool WeatherSyncService::Initialize(PeerRole role, UdpPeer& transport, replication::LocalHeroReplication& hero,
        replication::RemotePlayerChannels& peers, persistence::SessionCheckpointService& checkpoints,
        std::uint64_t epoch, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_) return false;
        role_ = role; transport_ = &transport; hero_ = &hero; peers_ = &peers; checkpoints_ = &checkpoints;
        epoch_ = epoch; diagnostics_ = diagnostics; base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned char setter[] = {0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x04};
        const unsigned char mist[] = {0x0F, 0x57, 0xC9, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04};
        if (std::memcmp(reinterpret_cast<void*>(base_ + 0x016C71B0), setter, sizeof(setter)) ||
            std::memcmp(reinterpret_cast<void*>(base_ + 0x016C7260), setter, sizeof(setter)) ||
            std::memcmp(reinterpret_cast<void*>(base_ + 0x016C5F50), mist, sizeof(mist)))
        { diagnostics_.Event("MultiplayerWeatherHookBlocked", "native queued weather setter bytes mismatch"); return false; }
        const unsigned char prefix[] = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1};
        active_ = this;
        if (!hook_.Install(reinterpret_cast<void*>(base_ + 0x016C7760), prefix, sizeof(prefix),
            reinterpret_cast<void*>(&Tick), sizeof(prefix)))
        { active_ = nullptr; diagnostics_.Event("MultiplayerWeatherHookBlocked", "CEngineWeatherRenderer::vf7 bytes mismatch"); return false; }
        original_ = reinterpret_cast<TickFn>(hook_.Original());
        diagnostics_.Event("MultiplayerWeatherHookReady", "host same-map effective rain/snow/mist; native queued setters, no render-resource writes");
        return true;
    }
    void* WeatherSyncService::Renderer() const noexcept
    {
        __try { return base_ ? *reinterpret_cast<void**>(base_ + 0x03221354) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool WeatherSyncService::ReadNative(void* renderer, protocol::WeatherMessage& s) const noexcept
    {
        __try
        {
            const auto* p = static_cast<const unsigned char*>(renderer);
            if (!p) return false;
            s.rain = *reinterpret_cast<const float*>(p + 0x5C8);
            s.snow = *reinterpret_cast<const float*>(p + 0x5CC);
            s.mist = *reinterpret_cast<const float*>(p + 0x188);
            s.enabled = p[8] ? (p[0x5D0] ? 1 : 0) | (p[0x5D1] ? 2 : 0) | (p[0x5D2] ? 4 : 0) : 0;
            return std::isfinite(s.rain) && std::isfinite(s.snow) && std::isfinite(s.mist) &&
                s.rain >= 0 && s.rain <= 16 && s.snow >= 0 && s.snow <= 16 && s.mist >= 0 && s.mist <= 16;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool WeatherSyncService::ApplyNative(void* renderer, const protocol::WeatherMessage& s) const noexcept
    {
        __try
        {
            auto* p = static_cast<unsigned char*>(renderer);
            if (!p || !p[8]) return false; // Respect the local graphics weather switch.
            using Set = void(__thiscall*)(void*, float);
            const bool precipitation = *reinterpret_cast<void**>(base_ + 0x03232A1C) != nullptr;
            const bool mist = *reinterpret_cast<void**>(base_ + 0x03232A20) != nullptr;
            const float values[] = {s.rain, s.snow, s.mist};
            const std::uintptr_t offsets[] = {0x5C8, 0x5CC, 0x188};
            const std::uintptr_t setters[] = {0x016C71B0, 0x016C7260, 0x016C5F50};
            for (unsigned int i = 0; i < 3; ++i)
            {
                // Unsupported maps keep their own assets and exclusion masks. Never
                // bypass the Unreal bridge or create a rain/snow resource here.
                if (!(i == 2 ? mist : precipitation)) continue;
                const bool enabled = (s.enabled & (1u << i)) != 0;
                const bool current = p[0x5D0 + i] != 0;
                const float target = enabled ? values[i] : 0;
                if (enabled != current || (enabled && std::fabs(*reinterpret_cast<float*>(p + offsets[i]) - target) > 0.0001f))
                    reinterpret_cast<Set>(base_ + setters[i])(renderer, target);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void __fastcall WeatherSyncService::Tick(void* renderer, void*, unsigned char update)
    {
        auto* self = active_;
        if (!self || !self->original_) return;
        protocol::WeatherMessage state;
        bool apply = false, ready = false;
        const auto now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            ready = self->ready_ && renderer == self->Renderer();
            state = self->incoming_;
            apply = ready && self->role_ == PeerRole::Guest && state.revision &&
                now - self->receivedAt_ <= 3000 && state.map == self->localMap_ && state.worldFence == self->fence_;
        }
        // Skip only the retail region-default weather block while a fresh host
        // snapshot owns this map. Keep wind/particle movement and native override
        // slots intact; restore this scoped flag even if the native tick throws.
        NativeWeatherTick(self->original_, renderer, update, apply);
        if (!ready) return;
        if (self->role_ == PeerRole::Host)
        {
            protocol::WeatherMessage captured;
            if (self->ReadNative(renderer, captured))
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                captured.map = self->localMap_; captured.mapEpoch = self->localMapEpoch_;
                captured.actorGeneration = self->localActorGeneration_; captured.worldFence = self->fence_;
                self->captured_ = captured; self->capturedAt_ = now;
            }
        }
        else if (apply && self->ApplyNative(renderer, state))
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->applied_ != state.revision)
            {
                self->applied_ = state.revision;
                protocol::WeatherMessage actual;
                if (!self->ReadNative(renderer, actual)) return;
                char detail[200]; std::snprintf(detail, sizeof(detail), "map=%u requested=%u actual=%u rain=%.4f snow=%.4f mist=%.4f; native regional capabilities retained",
                    state.map, state.enabled, actual.enabled, actual.rain, actual.snow, actual.mist);
                self->diagnostics_.Event("MultiplayerWeatherNativeApplied", detail);
            }
        }
    }
    bool WeatherSyncService::Process()
    {
        if (!transport_ || !hero_ || !checkpoints_) return true;
        const auto now = GetTickCount64();
        const auto* hero = hero_->CurrentState();
        protocol::WeatherMessage state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = hero && hero_->IsWorldReady() && transport_->HasPeer() && !checkpoints_->IsBarrierActive();
            if (!ready_) { incoming_ = {}; capturedAt_ = 0; return true; }
            if (localMap_ != hero->mapId || localMapEpoch_ != hero->mapEpoch || localActorGeneration_ != hero->actorGeneration)
            { incoming_ = {}; capturedAt_ = receivedAt_ = 0; nextSend_ = 0; }
            localMap_ = hero->mapId; localMapEpoch_ = hero->mapEpoch; localActorGeneration_ = hero->actorGeneration;
            fence_ = checkpoints_->WorldFence();
            if (role_ != PeerRole::Host || !capturedAt_ || now - capturedAt_ > 2000 || now < nextSend_) return true;
            state = captured_;
        }
        const bool changed = !SameWeather(state, published_);
        if (!changed && now - publishedAt_ < 1000) return true;
        state.epoch = epoch_; state.revision = revision_ + 1; state.sampledAt = transport_->SessionTimeMilliseconds();
        std::uint8_t bytes[protocol::WeatherBytes];
        if (!protocol::EncodeWeather(state, bytes, sizeof(bytes))) return true;
        if (transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::Weather, bytes, sizeof(bytes)))
        {
            ++revision_; published_ = state; publishedAt_ = now; nextSend_ = now + 100;
            if (changed)
            {
                char detail[160]; std::snprintf(detail, sizeof(detail), "map=%u enabled=%u rain=%.4f snow=%.4f mist=%.4f",
                    state.map, state.enabled, state.rain, state.snow, state.mist);
                diagnostics_.Event("MultiplayerWeatherNativeCaptured", detail);
            }
        }
        else nextSend_ = now + 100;
        return true;
    }
    bool WeatherSyncService::HandleReliableMessage(const TransportMessage& m)
    {
        if (role_ != PeerRole::Guest || !peers_ || !checkpoints_) return true;
        protocol::WeatherMessage s;
        if (!protocol::DecodeWeather(m.payload.data(), m.payloadSize, s)) return false;
        if (checkpoints_->IsBarrierActive() || s.worldFence != checkpoints_->WorldFence()) return true;
        bool authorized = false;
        for (const auto& peer : peers_->Snapshots())
            if (peer.state.role == PeerRole::Host && peer.state.actorId == m.sourceActorId &&
                peer.state.mapId == s.map && peer.state.mapEpoch == s.mapEpoch && peer.state.actorGeneration == s.actorGeneration) authorized = true;
        if (!authorized) return true;
        const auto now = transport_->SessionTimeMilliseconds();
        if (transport_->HasSynchronizedSessionClock() && now > s.sampledAt && now - s.sampledAt > 2000) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (host_ != m.sourceActorId || nonce_ != m.connectionNonce)
        { host_ = m.sourceActorId; nonce_ = m.connectionNonce; lastSeenRevision_ = remoteEpoch_ = 0; incoming_ = {}; }
        if ((remoteEpoch_ && remoteEpoch_ != s.epoch) || s.revision <= lastSeenRevision_) return true;
        remoteEpoch_ = s.epoch; lastSeenRevision_ = s.revision;
        if (!ready_ || s.map != localMap_) return true;
        incoming_ = s; receivedAt_ = GetTickCount64();
        return true;
    }
    bool WeatherSyncService::ReadForTest(protocol::WeatherMessage& output) noexcept { return ReadNative(Renderer(), output); }
    void WeatherSyncService::ResetForReload() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_ = {}; captured_ = {}; published_ = {}; capturedAt_ = receivedAt_ = nextSend_ = applied_ = publishedAt_ = 0; ready_ = false;
    }
    void WeatherSyncService::Shutdown() noexcept
    {
        if (!hook_.Shutdown()) return;
        if (active_ == this) active_ = nullptr;
        original_ = nullptr; ResetForReload(); transport_ = nullptr; hero_ = nullptr; peers_ = nullptr; checkpoints_ = nullptr;
        epoch_ = revision_ = host_ = nonce_ = lastSeenRevision_ = remoteEpoch_ = 0;
        localMap_ = 0; localMapEpoch_ = localActorGeneration_ = 0;
    }
}

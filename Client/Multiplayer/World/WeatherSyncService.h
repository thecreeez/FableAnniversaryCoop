#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/WeatherMessage.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include <mutex>

namespace fable::multiplayer
{
    class UdpPeer;
    namespace replication { class LocalHeroReplication; class RemotePlayerChannels; }
    namespace persistence { class SessionCheckpointService; }
}
namespace fable::multiplayer::world
{
    class WeatherSyncService final : public ReliableMessageSink
    {
    public:
        ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        { static constexpr protocol::PacketType types[] = {protocol::PacketType::Weather}; return {types, 1}; }
        bool Initialize(PeerRole, UdpPeer&, replication::LocalHeroReplication&,
            replication::RemotePlayerChannels&, persistence::SessionCheckpointService&, std::uint64_t,
            const core::Diagnostics&) noexcept;
        bool Process();
        bool HandleReliableMessage(const TransportMessage&) override;
        void ResetForReload() noexcept;
        void Shutdown() noexcept;
        bool ReadForTest(protocol::WeatherMessage&) noexcept;
    private:
        using TickFn = void(__thiscall*)(void*, unsigned char);
        static void __fastcall Tick(void*, void*, unsigned char);
        static WeatherSyncService* active_;
        void* Renderer() const noexcept;
        bool ReadNative(void*, protocol::WeatherMessage&) const noexcept;
        bool ApplyNative(void*, const protocol::WeatherMessage&) const noexcept;
        core::hooking::InlineHook hook_;
        TickFn original_ = nullptr;
        PeerRole role_ = PeerRole::Guest;
        std::uintptr_t base_ = 0;
        UdpPeer* transport_ = nullptr;
        replication::LocalHeroReplication* hero_ = nullptr;
        replication::RemotePlayerChannels* peers_ = nullptr;
        persistence::SessionCheckpointService* checkpoints_ = nullptr;
        core::Diagnostics diagnostics_;
        std::mutex mutex_;
        protocol::WeatherMessage incoming_, captured_, published_;
        std::uint64_t epoch_ = 0, revision_ = 0, nextSend_ = 0, receivedAt_ = 0, capturedAt_ = 0;
        std::uint64_t publishedAt_ = 0;
        std::uint64_t host_ = 0, nonce_ = 0, lastSeenRevision_ = 0, remoteEpoch_ = 0, applied_ = 0;
        std::uint64_t fence_ = 0;
        std::uint32_t localMapEpoch_ = 0, localActorGeneration_ = 0;
        std::uint16_t localMap_ = 0;
        bool ready_ = false;
    };
}

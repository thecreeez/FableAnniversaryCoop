#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Protocol/WorldClockMessage.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include <mutex>
#include <atomic>
#include <deque>
#include <unordered_map>

namespace fable::multiplayer
{
    class UdpPeer;
    namespace replication { class LocalHeroReplication; class RemotePlayerChannels; }
    namespace persistence { class SessionCheckpointService; }
}
namespace fable::multiplayer::world
{
    class WorldClockSyncService final : public ReliableMessageSink
    {
    public:
        ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        {
            static constexpr protocol::PacketType types[] = {protocol::PacketType::WorldClock, protocol::PacketType::SleepRequest};
            return {types, 2};
        }
        bool Initialize(PeerRole, UdpPeer&, replication::LocalHeroReplication&,
            replication::RemotePlayerChannels&, std::uint64_t epoch, const core::Diagnostics&) noexcept;
        void Shutdown() noexcept;
        bool Process();
        bool HandleReliableMessage(const TransportMessage&) override;
        void ResetForReload() noexcept;
        void SetCheckpointScope(persistence::SessionCheckpointService* scope) noexcept { checkpoints_ = scope; }
        bool ReadForTest(protocol::WorldClockMessage&) noexcept;
        bool IsNightForTest() noexcept;
        bool SetForTest(float fraction, float rate, int day, float duration = 0) noexcept;
        bool FreezeForTest(bool freeze) noexcept;
        void AdvanceForTest(float seconds) noexcept;
        bool RequestSleepForTest(float fraction) noexcept;
        bool HostWorldPausedForTest() noexcept
        { std::lock_guard<std::mutex> lock(mutex_); return (authoritative_.frozen & 2) != 0; }
    private:
        using TickFn = void (__thiscall*)(void*, float);
        using JumpFn = void (__thiscall*)(void*, float, float);
        static void __fastcall Jump(void*, void*, float, float);
        static void __fastcall Tick(void*, void*, float);
        static WorldClockSyncService* active_;
        void* Clock() const noexcept;
        core::hooking::InlineHook tick_;
        core::hooking::InlineHook jump_;
        JumpFn originalJump_ = nullptr;
        struct SleepRequest { std::uint64_t revision = 0, epoch = 0; float fraction = 0, duration = 0; std::uint16_t map = 0; std::uint64_t fence = 0; };
        SleepRequest outgoingSleep_;
        std::deque<SleepRequest> pendingSleeps_;
        std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> sleepRevisions_;
        std::uint64_t nextSleepRevision_ = 0, lastSleepAcceptedAt_ = 0;
        TickFn original_ = nullptr;
        UdpPeer* transport_ = nullptr;
        replication::LocalHeroReplication* hero_ = nullptr;
        replication::RemotePlayerChannels* peers_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        PeerRole role_ = PeerRole::Guest;
        std::uintptr_t base_ = 0;
        std::uint64_t epoch_ = 0, revision_ = 0, nextSend_ = 0;
        std::uint64_t hostActor_ = 0, hostNonce_ = 0, receivedAt_ = 0, appliedRevision_ = 0;
        std::mutex mutex_;
        protocol::WorldClockMessage incoming_ = {};
        protocol::WorldClockMessage authoritative_ = {};
        void* testFreezeClock_ = nullptr;
        int testFreezeToken_ = 0;
        std::atomic<std::uint64_t> lastNativeTickAt_{0};
        persistence::SessionCheckpointService* checkpoints_ = nullptr;
    };
}

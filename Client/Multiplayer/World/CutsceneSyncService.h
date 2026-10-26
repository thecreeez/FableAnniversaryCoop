#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace fable::game
{
    class EntityService;
}

namespace fable::game::cutscene
{
    class CutsceneObserver;
}

namespace fable::multiplayer
{
    class UdpPeer;
    namespace replication
    {
        class LocalHeroReplication;
        class RemotePlayerChannels;
    }
}

namespace fable::multiplayer::world
{
    // Shared cutscenes. When a quest script starts a cutscene on any peer, the
    // script thread is held (the game keeps running) until every connected
    // player stands on the same map within GatherRadius of the initiator.
    // Then the cutscene name is sent to everyone and each peer plays the same
    // retail sequence (movie resource, cutscene mode on, start, mode off).
    class CutsceneSyncService final : public ReliableMessageSink
    {
    public:
        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes()
            const noexcept override
        {
            static constexpr protocol::PacketType types[] = {
                protocol::PacketType::Cutscene};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(
            PeerRole role,
            std::uint64_t localActorId,
            UdpPeer& transport,
            replication::LocalHeroReplication& localHero,
            replication::RemotePlayerChannels& remotePlayers,
            game::cutscene::CutsceneObserver& observer,
            game::EntityService& entities,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;

        // Game thread.
        bool Process();
        bool HandleReliableMessage(const TransportMessage& message) override;

    private:
        enum class HoldState : std::uint8_t
        {
            None,
            Waiting,
            Released,
        };

        static constexpr float GatherRadius = 20.0f;
        // Gathering must hold this long, on a settled local world, before the
        // cutscene is released (a fresh arrival reports a stale map).
        static constexpr std::uint64_t GatherStableMilliseconds = 1'500;
        // A peer told to play first lets its own quest script start the
        // cutscene; only after this does it force a replay.
        static constexpr std::uint64_t OwnScriptGraceMilliseconds = 5'000;
        static constexpr std::uint64_t PeerReleaseFreshMilliseconds = 60'000;
        static constexpr std::uint64_t RemoteStateFreshMilliseconds = 5'000;
        static constexpr std::uint64_t WaitingReportMilliseconds = 5'000;
        // A held script never waits forever: after this the cutscene plays
        // locally only, so a disconnected friend cannot soft-lock the story.
        static constexpr std::uint64_t HoldTimeoutMilliseconds = 10 * 60 * 1'000;

        static bool __cdecl Gate(void* context, const char* name);
        bool HoldUntilGathered(const char* name);
        bool AllPlayersGathered(std::string& missing) const;
        bool SendPlay(const std::string& name, std::uint16_t mapId);
        void ReplayAsync(const std::string& name);
        static DWORD WINAPI ReplayThread(void* parameter);

        UdpPeer* transport_ = nullptr;
        replication::LocalHeroReplication* localHero_ = nullptr;
        replication::RemotePlayerChannels* remotePlayers_ = nullptr;
        game::cutscene::CutsceneObserver* observer_ = nullptr;
        game::EntityService* entities_ = nullptr;
        // The initiator already played its cutscene when the host echoes it.
        std::string lastReleasedName_;
        std::uint64_t lastReleasedAt_ = 0;
        static constexpr std::uint64_t EchoWindowMilliseconds = 30'000;
        core::Diagnostics diagnostics_ = {};
        PeerRole role_ = PeerRole::Guest;
        std::uint64_t localActorId_ = 0;
        std::uint64_t nextRevision_ = 0;

        std::mutex holdMutex_;
        HoldState holdState_ = HoldState::None;
        std::string heldName_;
        std::uint64_t heldSince_ = 0;
        std::uint64_t nextWaitingReportAt_ = 0;

        struct PeerPlay final
        {
            std::string name;
            std::uint16_t mapId = 0;
            std::uint64_t receivedAt = 0;
            bool consumed = false;
        };
        struct OutboundPlay final
        {
            std::string name;
            std::uint16_t mapId = 0;
        };
        std::deque<PeerPlay> peerPlays_;
        std::deque<OutboundPlay> outboundPlays_;
        std::uint64_t gatheredSince_ = 0;
        std::atomic_bool enabled_{false};
    };
}

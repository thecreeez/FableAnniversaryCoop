#include "CutsceneSyncService.h"

#include "Game/Cutscene/CutsceneObserver.h"
#include "Game/Entity/EntityService.h"
#include "Multiplayer/Protocol/CutsceneMessage.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Replication/RemotePlayerChannels.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveCutsceneSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.cutsceneSync;
    }

    struct ReplayRequest final
    {
        fable::game::cutscene::CutsceneObserver* observer = nullptr;
        fable::core::Diagnostics diagnostics = {};
        char name[fable::multiplayer::protocol::MaximumCutsceneNameBytes] = {};
    };
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkCutscene,
    0x1012u,
    "cutscene",
    817u,
    "multiplayer-cutscene-dispatch",
    ResolveCutsceneSink);

namespace fable::multiplayer::world
{
    void CutsceneSyncService::Initialize(
        const PeerRole role,
        const std::uint64_t localActorId,
        UdpPeer& transport,
        replication::LocalHeroReplication& localHero,
        replication::RemotePlayerChannels& remotePlayers,
        game::cutscene::CutsceneObserver& observer,
        game::EntityService& entities,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        entities_ = &entities;
        role_ = role;
        localActorId_ = localActorId;
        transport_ = &transport;
        localHero_ = &localHero;
        remotePlayers_ = &remotePlayers;
        observer_ = &observer;
        diagnostics_ = diagnostics;
        observer.SetGate(&Gate, this);
        enabled_.store(true, std::memory_order_release);
    }

    void CutsceneSyncService::Shutdown() noexcept
    {
        enabled_.store(false, std::memory_order_release);
        if (observer_ != nullptr) observer_->SetGate(nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lock(holdMutex_);
            // Never leave a quest script parked if the session ends.
            if (holdState_ == HoldState::Waiting) holdState_ = HoldState::Released;
        }
        transport_ = nullptr;
        localHero_ = nullptr;
        remotePlayers_ = nullptr;
        observer_ = nullptr;
        entities_ = nullptr;
        lastReleasedName_.clear();
        lastReleasedAt_ = 0;
        diagnostics_ = {};
        role_ = PeerRole::Guest;
        localActorId_ = 0;
        nextRevision_ = 0;
        peerPlays_.clear();
        outboundPlays_.clear();
        gatheredSince_ = 0;
    }

    bool __cdecl CutsceneSyncService::Gate(void* const context, const char* const name)
    {
        return static_cast<CutsceneSyncService*>(context)->HoldUntilGathered(name);
    }

    bool CutsceneSyncService::HoldUntilGathered(const char* const name)
    {
        // Quest-script thread. Scripts run in lockstep with the game thread,
        // so the hold yields one script frame at a time; blocking here would
        // freeze the whole game.
        {
            std::lock_guard<std::mutex> lock(holdMutex_);
            if (!enabled_.load(std::memory_order_acquire) || holdState_ != HoldState::None)
            {
                return true;
            }
            // Another player already gathered everyone for this cutscene:
            // our own script reached it, so it plays now, natively.
            const std::uint64_t now = GetTickCount64();
            for (PeerPlay& play : peerPlays_)
            {
                if (!play.consumed && play.name == name &&
                    now - play.receivedAt < PeerReleaseFreshMilliseconds)
                {
                    play.consumed = true;
                    char detail[160] = {};
                    std::snprintf(detail, sizeof(detail), "name=%s source=own-script", name);
                    diagnostics_.Event("CutsceneReleasedByPeer", detail);
                    return true;
                }
            }
            holdState_ = HoldState::Waiting;
            gatheredSince_ = 0;
            heldName_ = name;
            heldSince_ = GetTickCount64();
            nextWaitingReportAt_ = 0;
        }
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "name=%s", name);
        diagnostics_.Event("CutsceneHeldForGathering", detail);
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(holdMutex_);
                if (holdState_ == HoldState::Released)
                {
                    holdState_ = HoldState::None;
                    heldName_.clear();
                    return true;
                }
            }
            if (observer_ == nullptr || !observer_->YieldScriptFrame())
            {
                // Without a yield primitive, never risk a frozen game.
                std::lock_guard<std::mutex> lock(holdMutex_);
                holdState_ = HoldState::None;
                heldName_.clear();
                diagnostics_.Event("CutsceneHoldAbandoned", "script frame yield unavailable; playing locally");
                return true;
            }
        }
    }

    bool CutsceneSyncService::AllPlayersGathered(std::string& missing) const
    {
        missing.clear();
        const PlayerState* const local = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        if (local == nullptr || remotePlayers_ == nullptr) return false;
        bool gathered = true;
        if (local->mapId == 0 || local->mapName.empty()) return false;
        const std::uint64_t now = GetTickCount64();
        for (const replication::RemotePlayerSnapshot& remote : remotePlayers_->Snapshots())
        {
            const PlayerState& state = remote.state;
            const float dx = state.position.x - local->position.x;
            const float dy = state.position.y - local->position.y;
            // Different regions can share coordinates, so the map must match
            // by id and name, and the remote report must be current (a peer
            // mid-transition still reports its old map for a moment).
            const bool fresh = remote.receivedAt != 0 && now - remote.receivedAt <= RemoteStateFreshMilliseconds;
            const bool nearby = fresh && state.mapId != 0 &&
                state.mapId == local->mapId && state.mapName == local->mapName &&
                std::sqrt(dx * dx + dy * dy) <= GatherRadius;
            if (!nearby)
            {
                gathered = false;
                if (!missing.empty()) missing += ",";
                missing += state.playerId.empty() ? std::to_string(state.actorId) : state.playerId;
            }
        }
        return gathered;
    }

    bool CutsceneSyncService::SendPlay(const std::string& name, const std::uint16_t mapId)
    {
        protocol::CutsceneMessage message;
        message.revision = ++nextRevision_;
        message.mapId = mapId;
        if (name.empty() || name.size() >= protocol::MaximumCutsceneNameBytes) return false;
        message.nameLength = static_cast<std::uint8_t>(name.size());
        std::memcpy(message.name, name.data(), name.size());
        std::array<std::uint8_t, protocol::CutsceneFixedBytes + protocol::MaximumCutsceneNameBytes> bytes = {};
        std::size_t encodedSize = 0;
        if (!protocol::EncodeCutsceneMessage(message, bytes.data(), bytes.size(), encodedSize)) return false;
        return transport_ != nullptr && transport_->SubmitReliable(
            reliable_stream::Control, protocol::PacketType::Cutscene, bytes.data(), encodedSize);
    }

    bool CutsceneSyncService::Process()
    {
        if (!enabled_.load(std::memory_order_acquire)) return true;
        if (transport_ == nullptr || transport_->HasFailed()) return false;
        const std::uint64_t now = GetTickCount64();
        if (observer_ != nullptr && entities_ != nullptr)
        {
            // The script interface exists only once the game world is up.
            observer_->BindScriptInterface(entities_->Interface().Resolve());
        }

        const PlayerState* const local = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        const std::uint16_t localMap = local != nullptr ? local->mapId : 0;
        const bool worldSettled = localHero_ != nullptr && localHero_->IsWorldReady() && local != nullptr;

        std::string releaseName;
        bool timedOut = false;
        {
            std::lock_guard<std::mutex> lock(holdMutex_);
            if (holdState_ == HoldState::Waiting)
            {
                std::string missing;
                const bool gathered = worldSettled && AllPlayersGathered(missing);
                if (!gathered)
                {
                    gatheredSince_ = 0;
                }
                else if (gatheredSince_ == 0)
                {
                    gatheredSince_ = now;
                }
                if (gathered && now - gatheredSince_ >= GatherStableMilliseconds)
                {
                    releaseName = heldName_;
                }
                else if (now - heldSince_ > HoldTimeoutMilliseconds)
                {
                    timedOut = true;
                    holdState_ = HoldState::Released;
                }
                else if (now >= nextWaitingReportAt_)
                {
                    nextWaitingReportAt_ = now + WaitingReportMilliseconds;
                    char detail[256] = {};
                    std::snprintf(detail, sizeof(detail), "name=%s waiting_for=%s held_ms=%llu",
                        heldName_.c_str(), missing.c_str(),
                        static_cast<unsigned long long>(now - heldSince_));
                    diagnostics_.Event("CutsceneWaitingForPlayers", detail);
                }
            }
        }
        if (timedOut)
        {
            diagnostics_.Event("CutsceneHoldTimedOut", "players never gathered; playing locally only");
        }
        if (!releaseName.empty())
        {
            outboundPlays_.push_back({releaseName, localMap});
            lastReleasedName_ = releaseName;
            lastReleasedAt_ = now;
            std::lock_guard<std::mutex> lock(holdMutex_);
            holdState_ = HoldState::Released;
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail), "name=%s", releaseName.c_str());
            diagnostics_.Event("CutsceneGatheredAndReleased", detail);
        }

        while (!outboundPlays_.empty())
        {
            if (!SendPlay(outboundPlays_.front().name, outboundPlays_.front().mapId))
            {
                return !transport_->HasFailed();
            }
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail), "name=%s map_id=%u",
                outboundPlays_.front().name.c_str(), static_cast<unsigned>(outboundPlays_.front().mapId));
            diagnostics_.Event("CutscenePlaySent", detail);
            outboundPlays_.pop_front();
        }

        // Peer releases: our own waiting script plays at once; otherwise give
        // our script a moment to reach the cutscene, then force it only when
        // we are actually on the initiator's map.
        std::deque<PeerPlay> remaining;
        for (PeerPlay& play : peerPlays_)
        {
            if (play.consumed) continue;
            if (play.name == lastReleasedName_ && now - lastReleasedAt_ < EchoWindowMilliseconds)
            {
                char detail[160] = {};
                std::snprintf(detail, sizeof(detail), "name=%s", play.name.c_str());
                diagnostics_.Event("CutsceneEchoIgnored", detail);
                continue;
            }
            bool releasedWaiting = false;
            {
                std::lock_guard<std::mutex> lock(holdMutex_);
                if (holdState_ == HoldState::Waiting && heldName_ == play.name)
                {
                    holdState_ = HoldState::Released;
                    releasedWaiting = true;
                }
            }
            if (releasedWaiting)
            {
                char detail[160] = {};
                std::snprintf(detail, sizeof(detail), "name=%s source=waiting-own-script", play.name.c_str());
                diagnostics_.Event("CutsceneReleasedByPeer", detail);
                continue;
            }
            if (now - play.receivedAt < OwnScriptGraceMilliseconds)
            {
                remaining.push_back(play);
                continue;
            }
            char detail[192] = {};
            if (worldSettled && localMap == play.mapId)
            {
                std::snprintf(detail, sizeof(detail), "name=%s map_id=%u", play.name.c_str(),
                    static_cast<unsigned>(play.mapId));
                diagnostics_.Event("CutsceneForcedReplay", detail);
                ReplayAsync(play.name);
            }
            else
            {
                std::snprintf(detail, sizeof(detail), "name=%s initiator_map=%u local_map=%u",
                    play.name.c_str(), static_cast<unsigned>(play.mapId), static_cast<unsigned>(localMap));
                diagnostics_.Event("CutsceneSkippedNotPresent", detail);
            }
        }
        peerPlays_.swap(remaining);
        return true;
    }

    bool CutsceneSyncService::HandleReliableMessage(const TransportMessage& transportMessage)
    {
        if (transportMessage.type != protocol::PacketType::Cutscene) return false;
        protocol::CutsceneMessage message;
        if (!protocol::DecodeCutsceneMessage(
                transportMessage.payload.data(), transportMessage.payloadSize, message))
        {
            diagnostics_.Event("CutsceneMessageRejected", "invalid cutscene packet");
            return true;
        }
        if (!enabled_.load(std::memory_order_acquire)) return true;
        try
        {
            peerPlays_.push_back({std::string(message.name, message.nameLength),
                message.mapId, GetTickCount64(), false});
            // The host fans a guest's cutscene out to every other guest.
            if (role_ == PeerRole::Host)
            {
                outboundPlays_.push_back({std::string(message.name, message.nameLength), message.mapId});
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    void CutsceneSyncService::ReplayAsync(const std::string& name)
    {
        // The retail start blocks for the whole cutscene, so it must not run
        // on the game thread.
        auto* const request = new (std::nothrow) ReplayRequest();
        if (request == nullptr || observer_ == nullptr) { delete request; return; }
        request->observer = observer_;
        request->diagnostics = diagnostics_;
        std::snprintf(request->name, sizeof(request->name), "%s", name.c_str());
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "name=%s", request->name);
        diagnostics_.Event("CutsceneReplayStarted", detail);
        const HANDLE thread = CreateThread(nullptr, 0, &ReplayThread, request, 0, nullptr);
        if (thread == nullptr)
        {
            delete request;
            return;
        }
        CloseHandle(thread);
    }

    DWORD WINAPI CutsceneSyncService::ReplayThread(void* const parameter)
    {
        auto* const request = static_cast<ReplayRequest*>(parameter);
        const std::uint64_t startedAt = GetTickCount64();
        const bool played = request->observer->Play(request->name);
        char detail[192] = {};
        std::snprintf(detail, sizeof(detail), "name=%s played=%s duration_ms=%llu",
            request->name, played ? "true" : "false",
            static_cast<unsigned long long>(GetTickCount64() - startedAt));
        request->diagnostics.Event("CutsceneReplayFinished", detail);
        delete request;
        return 0;
    }
}

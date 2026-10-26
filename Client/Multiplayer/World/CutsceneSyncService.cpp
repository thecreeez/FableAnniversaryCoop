#include "CutsceneSyncService.h"

#include "Game/Cutscene/CutsceneObserver.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Game/World/Travel/Native/RegionExitFunctions.h"
#include "Multiplayer/Entities/LiveEntityRegistry.h"
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
#include <cwchar>

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
        const entities::LiveEntityRegistry& liveEntities,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        entities_ = &entities;
        liveEntities_ = &liveEntities;
        prompt_.Bind(GetModuleHandleW(nullptr));
        heroControl_.Bind(GetModuleHandleW(nullptr));
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
        EndWaitPrompt();
        entities_ = nullptr;
        liveEntities_ = nullptr;
        history_.clear();
        nextHistoryAt_ = 0;
        currentMapId_ = 0;
        previousMapId_ = 0;
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
                    suspendModeRequested_.store(false, std::memory_order_release);
                    if (modeSuspended_.exchange(false, std::memory_order_acq_rel) && observer_ != nullptr)
                    {
                        // Re-enter the scripted sequence and give the script its
                        // Hero back before the cutscene runs.
                        const bool begun = observer_->BeginScriptedSequence();
                        const bool attached = heroControl_.IsSuspended() && heroControl_.Resume(HeroThing());
                        observer_->SetCutsceneMode(true);
                        char state[96] = {};
                        std::snprintf(state, sizeof(state), "sequence=%s hero-control=%s",
                            begun ? "begun" : "failed", attached ? "reattached" : "none");
                        diagnostics_.Event("CutsceneModeRestored", state);
                    }
                    return true;
                }
            }
            if (suspendModeRequested_.exchange(false, std::memory_order_acq_rel) && observer_ != nullptr &&
                !modeSuspended_.load(std::memory_order_acquire))
            {
                const bool left = observer_->SetCutsceneMode(false);
                const bool detached = heroControl_.Suspend(HeroThing());
                // Retail end of a scripted sequence: HUD and player input back.
                const bool ended = observer_->EndScriptedSequence();
                modeSuspended_.store(left || detached || ended, std::memory_order_release);
                char state[96] = {};
                std::snprintf(state, sizeof(state), "sequence=%s hero-control=%s",
                    ended ? "ended" : "failed", detached ? "detached" : "not-scripted");
                diagnostics_.Event(left ? "CutsceneModeSuspended" : "CutsceneModeSuspendFailed", state);
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

    bool CutsceneSyncService::AllPlayersGathered(
        std::string& missing, std::uint32_t& present, std::uint32_t& total) const
    {
        missing.clear();
        present = 0;
        total = 1;
        const PlayerState* const local = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        if (local == nullptr || remotePlayers_ == nullptr || !anchored_) return false;
        if (local->mapId == 0 || local->mapName.empty()) return false;
        const auto inCircle = [this](const game::Vector3& position)
        {
            const float dx = position.x - anchor_.position.x;
            const float dy = position.y - anchor_.position.y;
            return std::sqrt(dx * dx + dy * dy) <= GatherRadius;
        };
        bool gathered = local->mapId == anchor_.mapId && inCircle(local->position);
        if (gathered) ++present;
        else missing = "self";
        const std::uint64_t now = GetTickCount64();
        for (const replication::RemotePlayerSnapshot& remote : remotePlayers_->Snapshots())
        {
            ++total;
            const PlayerState& state = remote.state;
            // Different regions can share coordinates, so the map must match
            // by id and name, and the remote report must be current (a peer
            // mid-transition still reports its old map for a moment).
            const bool fresh = remote.receivedAt != 0 && now - remote.receivedAt <= RemoteStateFreshMilliseconds;
            const bool nearby = fresh && state.mapId != 0 &&
                state.mapId == local->mapId && state.mapName == local->mapName &&
                local->mapId == anchor_.mapId && inCircle(state.position);
            if (nearby) ++present;
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
        bool waiting = false;
        std::uint32_t present = 0;
        std::uint32_t total = 1;
        {
            std::lock_guard<std::mutex> lock(holdMutex_);
            waiting = holdState_ == HoldState::Waiting;
            TrackMap(worldSettled ? local : nullptr);
            if (!waiting) RecordHistory(worldSettled ? local : nullptr, now);
            if (waiting && !anchored_ && worldSettled) AnchorHold(*local);
            if (holdState_ == HoldState::Waiting)
            {
                std::string missing;
                const bool gathered = worldSettled && AllPlayersGathered(missing, present, total);
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
        if (waiting && releaseName.empty() && !timedOut)
        {
            UpdateWaitPrompt(worldSettled ? local : nullptr, present, total);
        }
        else
        {
            EndWaitPrompt();
        }
        ProcessRetreat(worldSettled ? local : nullptr, now);
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

    void CutsceneSyncService::TrackMap(const PlayerState* const local) noexcept
    {
        if (local == nullptr || local->mapId == 0 || local->mapId == currentMapId_) return;
        previousMapId_ = currentMapId_;
        currentMapId_ = local->mapId;
    }

    void CutsceneSyncService::RecordHistory(const PlayerState* const local, const std::uint64_t now)
    {
        if (local == nullptr || local->mapId == 0) return;
        if (now < nextHistoryAt_) return;
        nextHistoryAt_ = now + HistorySampleMilliseconds;
        try
        {
            history_.push_back({now, {local->position, local->facing, local->mapId}});
        }
        catch (...)
        {
            return;
        }
        while (!history_.empty() && now - history_.front().first > HistoryMilliseconds) history_.pop_front();
    }

    void CutsceneSyncService::AnchorHold(const PlayerState& local)
    {
        anchored_ = true;
        anchor_ = {local.position, local.facing, local.mapId};
        promptSuppressed_ = false;
        retreatKnown_ = false;
        retreatCrossesMap_ = false;
        // The newest place on this map outside the circle.
        for (auto sample = history_.rbegin(); sample != history_.rend(); ++sample)
        {
            const HeroSample& at = sample->second;
            if (at.mapId != local.mapId) break;
            const float dx = at.position.x - anchor_.position.x;
            const float dy = at.position.y - anchor_.position.y;
            if (std::sqrt(dx * dx + dy * dy) > GatherRadius)
            {
                retreat_ = at;
                retreatKnown_ = true;
                break;
            }
        }
        // Arrived straight into the cutscene: go back where we came from.
        if (!retreatKnown_ && previousMapId_ != 0 && previousMapId_ != local.mapId &&
            FindPreviousMapRoute(previousMapId_, retreat_.position, retreat_.facing))
        {
            retreat_.mapId = previousMapId_;
            retreatKnown_ = true;
            retreatCrossesMap_ = true;
        }
        char detail[224] = {};
        std::snprintf(detail, sizeof(detail),
            "name=%s anchor=(%.1f,%.1f,%.1f) map_id=%u retreat=%s retreat_map_id=%u",
            heldName_.c_str(), anchor_.position.x, anchor_.position.y, anchor_.position.z,
            static_cast<unsigned>(anchor_.mapId),
            !retreatKnown_ ? "none" : retreatCrossesMap_ ? "previous-map" : "outside-circle",
            static_cast<unsigned>(retreat_.mapId));
        diagnostics_.Event("CutsceneHoldAnchored", detail);
    }

    bool CutsceneSyncService::FindPreviousMapRoute(
        const std::uint16_t mapId, game::Vector3& position, float& facing) const
    {
        if (liveEntities_ == nullptr || entities_ == nullptr || currentMapId_ == 0) return false;
        // The exit on this map nearest the anchor that leads to the map we
        // came from; travel goes to that exit's destination entrance.
        bool found = false;
        float best = 0.0f;
        for (const auto& record : liveEntities_->Snapshot())
        {
            if (record.mapId != currentMapId_ || record.thing == nullptr) continue;
            game::world::travel::native::RegionExitDescriptor exit;
            if (!game::world::travel::native::RegionExitFunctions::Describe(
                    record.thing, record.thingUid, entities_->GameModule(), exit) ||
                exit.destinationMapId != mapId)
            {
                continue;
            }
            const float dx = record.position.x - anchor_.position.x;
            const float dy = record.position.y - anchor_.position.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (found && distance >= best) continue;
            game::Entity* const destination = entities_->FindByUid(exit.destinationEntranceUid);
            if (destination == nullptr) continue;
            if (destination->IsValid())
            {
                const game::Vector3 target = destination->GetPosition();
                const float targetFacing = destination->GetFacing();
                if (std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z))
                {
                    position = target;
                    facing = targetFacing;
                    best = distance;
                    found = true;
                }
            }
            destination->Release();
        }
        return found;
    }

    void CutsceneSyncService::UpdateWaitPrompt(
        const PlayerState* const local, const std::uint32_t present, const std::uint32_t total)
    {
        if (local == nullptr || !anchored_) return;
        const float dx = local->position.x - anchor_.position.x;
        const float dy = local->position.y - anchor_.position.y;
        const bool inside = local->mapId == anchor_.mapId && std::sqrt(dx * dx + dy * dy) <= GatherRadius;
        if (!inside)
        {
            // Walked (or was moved) out: coming back shows the box again.
            prompt_.Close();
            promptSuppressed_ = false;
            return;
        }
        if (promptSuppressed_ || retreatPending_) return;
        if (prompt_.Poll() == game::native::NativePromptBox::State::Dismissed)
        {
            prompt_.Close();
            promptSuppressed_ = true;
            suspendModeRequested_.store(true, std::memory_order_release);
            if (retreatKnown_)
            {
                retreatPending_ = true;
                retreatAttempts_ = 0;
                nextRetreatAt_ = 0;
            }
            char detail[192] = {};
            std::snprintf(detail, sizeof(detail), "name=%s present=%u total=%u retreat=%s",
                heldName_.c_str(), present, total,
                !retreatKnown_ ? "none" : retreatCrossesMap_ ? "previous-map" : "outside-circle");
            diagnostics_.Event("CutsceneWaitCancelled", detail);
            return;
        }
        wchar_t text[160] = {};
        std::swprintf(text, sizeof(text) / sizeof(text[0]),
            L"\u041E\u0436\u0438\u0434\u0430\u043D\u0438\u0435 \u0438\u0433\u0440\u043E\u043A\u043E\u0432 (%u/%u), \u0435\u0441\u043B\u0438 \u0445\u043E\u0442\u0438\u0442\u0435 \u043F\u0440\u0435\u043A\u0440\u0430\u0442\u0438\u0442\u044C \u043E\u0436\u0438\u0434\u0430\u043D\u0438\u0435 \u043D\u0430\u0436\u043C\u0438\u0442\u0435 \u041B\u041A\u041C", present, total);
        prompt_.Show(text);
    }

    void CutsceneSyncService::EndWaitPrompt() noexcept
    {
        prompt_.Close();
        anchored_ = false;
        promptSuppressed_ = false;
    }

    void CutsceneSyncService::ProcessRetreat(const PlayerState* const local, const std::uint64_t now)
    {
        if (!retreatPending_ || now < nextRetreatAt_) return;
        // The frozen Hero ignores teleports until the script left cutscene mode.
        if (local == nullptr || entities_ == nullptr ||
            (retreatAttempts_ == 0 && !modeSuspended_.load(std::memory_order_acquire)))
        {
            nextRetreatAt_ = now + RetreatRetryMilliseconds;
            return;
        }
        // Done once the Hero is on the target map (and, on the same map, out
        // of the circle the teleport targeted).
        if (retreatAttempts_ > 0 && local->mapId == retreat_.mapId)
        {
            const float dx = local->position.x - retreat_.position.x;
            const float dy = local->position.y - retreat_.position.y;
            if (retreatCrossesMap_ || std::sqrt(dx * dx + dy * dy) < GatherRadius * 0.5f)
            {
                retreatPending_ = false;
                diagnostics_.Event("CutsceneWaitRetreated", retreatCrossesMap_ ? "previous-map" : "outside-circle");
                return;
            }
        }
        if (retreatAttempts_ >= RetreatAttemptLimit)
        {
            retreatPending_ = false;
            diagnostics_.Event("CutsceneWaitRetreatFailed", "hero teleport was not accepted");
            return;
        }
        ++retreatAttempts_;
        // A cross-map move unsettles the world; never request it twice.
        nextRetreatAt_ = now + (retreatCrossesMap_ ? RetreatRetryMilliseconds * 8 : RetreatRetryMilliseconds);
        game::Entity* const hero = entities_->GetHero();
        if (hero == nullptr) return;
        if (hero->IsValid()) hero->Teleport(retreat_.position, retreat_.facing, false);
        hero->Release();
    }

    void* CutsceneSyncService::HeroThing() const
    {
        if (entities_ == nullptr) return nullptr;
        game::Entity* const hero = entities_->GetHero();
        if (hero == nullptr) return nullptr;
        void* const thing = hero->IsValid() ? entities_->ResolveNative(hero->NativeHandle()) : nullptr;
        hero->Release();
        return thing;
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

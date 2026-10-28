#include "AuthorityReplication.h"
#include "Multiplayer/Entities/WorldEntityDirectory.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"

#include "Multiplayer/Protocol/AuthorityMessage.h"
#include "Multiplayer/Protocol/PacketEnvelope.h"
#include "Multiplayer/Transport/TransportMessage.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <Windows.h>

#include <array>
#include <iterator>
#include <cstdio>
#include <algorithm>
#include <utility>

namespace fable::multiplayer::authority
{
    void AuthorityReplication::Initialize(
        PeerRole role,
        std::uint64_t localActorId,
        UdpPeer& transport,
        const core::Diagnostics& diagnostics)
    {
        Shutdown();
        role_ = role;
        localActorId_ = localActorId;
        transport_ = &transport;
        diagnostics_ = diagnostics;
        maps_.Initialize(role, localActorId, diagnostics);
        actions_.Initialize(role, localActorId, diagnostics);
        mapIdentities_.Initialize(role, localActorId, diagnostics);
        initialized_ = true;
    }

    bool AuthorityReplication::Reconcile(
        const PlayerState* localPlayer,
        const std::vector<replication::RemotePlayerSnapshot>& remotePlayers)
    {
        if (!initialized_ || transport_ == nullptr)
        {
            return false;
        }
        const auto preparationNow = GetTickCount64();
        for (auto it = simulationPreparations_.begin(); it != simulationPreparations_.end();)
        {
            const auto* lease = maps_.Find(it->second.message.mapName);
            it = lease == nullptr || lease->epoch != it->second.message.mapEpoch ||
                preparationNow - it->second.startedAt > 5000
                ? simulationPreparations_.erase(it) : std::next(it);
        }
        mapIdentities_.Reconcile(localPlayer, remotePlayers);
        PlayerState canonicalLocalState;
        const PlayerState* canonicalLocal = localPlayer;
        if (localPlayer != nullptr)
        {
            canonicalLocalState = *localPlayer;
            const std::string* const canonicalName =
                mapIdentities_.FindName(localPlayer->mapId);
            if (canonicalName != nullptr)
            {
                canonicalLocalState.mapName = *canonicalName;
            }
            canonicalLocal = &canonicalLocalState;
        }
        std::vector<replication::RemotePlayerSnapshot> canonicalRemotes =
            remotePlayers;
        for (auto& remote : canonicalRemotes)
        {
            const std::string* const canonicalName =
                mapIdentities_.FindName(remote.state.mapId);
            if (canonicalName != nullptr)
            {
                remote.state.mapName = *canonicalName;
            }
        }
        if (!RequestLocalMap(canonicalLocal))
        {
            return false;
        }
        if (role_ != PeerRole::Host)
        {
            return PublishSimulationAssignments();
        }

        const std::uint64_t peerRevision = transport_->PeerSetRevision();
        const MapBaselinePreparationResult collection =
            PrepareHostCollectionForPeers();
        if (collection == MapBaselinePreparationResult::Failed)
        {
            diagnostics_.Event(
                "MultiplayerAuthorityCollectionFailed",
                "host world collection could not be ordered ahead of remote map grants");
            return false;
        }
        if (collection == MapBaselinePreparationResult::Deferred)
        {
            return true;
        }
        if (peerRevision != knownPeerRevision_)
        {
            maps_.QueueBaseline();
            actions_.QueueBaseline();
            // Late joiners need per-NPC overrides as well as map ownership.
            // Otherwise their AI gate falls back to the map owner indefinitely.
            for (const auto& entry : simulationAssignments_)
            {
                const auto* lease = maps_.Find(entry.second.mapName);
                if (lease == nullptr || lease->epoch != entry.second.mapEpoch) continue;
                protocol::AuthorityMessage baseline;
                baseline.operation = protocol::AuthorityOperation::Grant;
                baseline.scope = protocol::AuthorityScope::EntitySimulation;
                baseline.entityUid = entry.first.thingUid;
                baseline.entityGeneration = entry.first.generation;
                baseline.ownerActorId = entry.second.actorId;
                baseline.mapEpoch = entry.second.mapEpoch;
                baseline.actionEpoch = entry.second.epoch;
                baseline.mapName = entry.second.mapName;
                pendingSimulationAssignments_.push_back(std::move(baseline));
            }
        }
        knownPeerRevision_ = peerRevision;

        actorMaps_.clear();
        if (canonicalLocal != nullptr && canonicalLocal->actorId != 0 &&
            !canonicalLocal->mapName.empty())
        {
            actorMaps_[canonicalLocal->actorId] = canonicalLocal->mapName;
        }
        for (const auto& remote : canonicalRemotes)
        {
            if (remote.state.actorId != 0 && !remote.state.mapName.empty())
            {
                actorMaps_[remote.state.actorId] = remote.state.mapName;
            }
        }
        maps_.HostReconcile(canonicalLocal, canonicalRemotes);
        actions_.HostFenceAgainstMaps(maps_, actorMaps_);
        return PublishHostMessages();
    }

    void AuthorityReplication::SetMapBaselineGate(
        MapAuthorityBaselineGate* gate) noexcept
    {
        mapBaselineGate_ = gate;
    }

    bool AuthorityReplication::RequestMapPreparation(
        const std::string& mapName,
        std::uint16_t mapId)
    {
        if (!initialized_ || mapName.empty() || mapId == 0)
        {
            return false;
        }
        (void)mapIdentities_.ObserveAuthoritative(mapName, mapId);
        const std::string* const resolvedName =
            mapIdentities_.FindName(mapId);
        const std::string& effectiveMapName = resolvedName != nullptr
            ? *resolvedName
            : mapName;
        const bool sameMap = preparedLocalMapId_ == mapId;
        if (sameMap && role_ == PeerRole::Host)
        {
            return true;
        }
        if (sameMap && preparedLocalBaselineRevision_ != 0)
        {
            // A request being queued is not an acknowledgement.  Only the
            // host's Prepared message may establish the revision that lets a
            // guest pass the construction gate.
            return true;
        }

        const MapAuthorityLease* const observed = maps_.Find(mapId);
        if (role_ == PeerRole::Host)
        {
            if (!SubmitMapRequest(
                    protocol::AuthorityOperation::Prepare,
                    effectiveMapName,
                    mapId,
                    observed != nullptr ? observed->epoch : 0))
            {
                return false;
            }
            preparedLocalMapId_ = mapId;
            preparedLocalBaselineRevision_ = 0;
            preparedLocalMapName_ = effectiveMapName;
            preparationRetry_.Acknowledge();
            return true;
        }

        const std::uint64_t now = static_cast<std::uint64_t>(
            GetTickCount64());
        if (!sameMap)
        {
            preparedLocalMapId_ = mapId;
            preparedLocalBaselineRevision_ = 0;
            preparedLocalMapName_ = effectiveMapName;
            preparationRetry_.Acknowledge();
        }
        if (!preparationRetry_.IsDue(now))
        {
            return true;
        }

        const bool submitted = SubmitMapRequest(
            protocol::AuthorityOperation::Prepare,
            effectiveMapName,
            mapId,
            observed != nullptr ? observed->epoch : 0);
        preparationRetry_.RecordAttempt(now);
        if (!submitted)
        {
            return false;
        }
        if (preparationRetry_.AttemptCount() > 1)
        {
            diagnostics_.Event(
                "MultiplayerMapPreparationRetry",
                "guest did not receive Prepared; retrying the identical map preparation request");
        }
        return true;
    }

    bool AuthorityReplication::IsMapPreparationReady(
        const std::string& mapName,
        std::uint16_t mapId) const noexcept
    {
        if (!initialized_ || mapName.empty() || mapId == 0 ||
            preparedLocalMapId_ != mapId)
        {
            return false;
        }
        if (role_ == PeerRole::Host)
        {
            return true;
        }
        return preparedLocalBaselineRevision_ != 0 &&
            mapBaselineGate_ != nullptr &&
            mapBaselineGate_->IsGuestGrantReady(
                mapId,
                preparedLocalBaselineRevision_);
    }

    bool AuthorityReplication::ProcessControl()
    {
        if (!initialized_ || transport_ == nullptr)
        {
            return false;
        }
        if (role_ != PeerRole::Host)
        {
            return true;
        }
        const MapBaselinePreparationResult collection =
            PrepareHostCollectionForPeers();
        if (collection == MapBaselinePreparationResult::Failed)
        {
            diagnostics_.Event(
                "MultiplayerAuthorityCollectionFailed",
                "host world collection could not be ordered ahead of remote map grants");
            return false;
        }
        if (collection == MapBaselinePreparationResult::Deferred)
        {
            return true;
        }
        return PublishHostBaselinePreparations() && PublishHostMessages();
    }

    bool AuthorityReplication::HandleReliableMessage(
        const TransportMessage& transportMessage)
    {
        if (transportMessage.type != protocol::PacketType::Authority)
        {
            return false;
        }
        protocol::AuthorityMessage message;
        if (!protocol::DecodeAuthorityMessage(
                transportMessage.payload.data(),
                transportMessage.payloadSize,
                message))
        {
            diagnostics_.Event(
                "MultiplayerAuthorityMessageRejected",
                "invalid authority payload");
            return true;
        }
        if (message.scope == protocol::AuthorityScope::EntitySimulation &&
            message.operation == protocol::AuthorityOperation::Prepared)
        {
            if (role_ != PeerRole::Host || message.ownerActorId != transportMessage.sourceActorId) return true;
            const EntityAuthorityKey key{message.entityUid, message.entityGeneration};
            auto prepared = simulationPreparations_.find(key);
            if (prepared == simulationPreparations_.end()) return true;
            const auto& wanted = prepared->second.message;
            if (wanted.actionEpoch != message.actionEpoch || wanted.ownerActorId != message.ownerActorId ||
                wanted.mapEpoch != message.mapEpoch || wanted.mapName != message.mapName) return true;
            const auto* lease = maps_.Find(message.mapName);
            if (lease == nullptr || lease->epoch != message.mapEpoch) { simulationPreparations_.erase(prepared); return true; }
            message.operation = protocol::AuthorityOperation::Grant;
            if (ApplySimulationAssignment(message)) pendingSimulationAssignments_.push_back(message);
            simulationPreparations_.erase(key);
            return true;
        }
        if (message.scope == protocol::AuthorityScope::EntitySimulation &&
            message.operation == protocol::AuthorityOperation::Prepare)
        {
            if (role_ != PeerRole::Host && message.ownerActorId == localActorId_)
                simulationPreparations_[{message.entityUid, message.entityGeneration}] = {message, GetTickCount64()};
            return true;
        }
        if (role_ == PeerRole::Host)
        {
            if ((message.operation == protocol::AuthorityOperation::Request ||
                    message.operation == protocol::AuthorityOperation::Prepare) &&
                message.scope == protocol::AuthorityScope::MapSimulation &&
                message.ownerActorId == transportMessage.sourceActorId)
            {
                const std::string* const canonical =
                    mapIdentities_.FindName(message.mapId);
                const std::string& effectiveMapName = canonical != nullptr
                    ? *canonical
                    : message.mapName;
                const bool accepted = message.operation ==
                        protocol::AuthorityOperation::Prepare
                    ? maps_.HostPrepare(
                        effectiveMapName,
                        message.mapId,
                        transportMessage.sourceActorId,
                        message.mapEpoch)
                    : maps_.HostRequest(
                        effectiveMapName,
                        message.mapId,
                        transportMessage.sourceActorId,
                        message.mapEpoch);
                if (!accepted)
                {
                    diagnostics_.Event(
                        "MultiplayerMapAuthorityRequestRejected",
                        "requester, map, or observed epoch was invalid");
                }
                else if (!QueueHostBaselinePreparation(
                        message.mapId,
                        effectiveMapName,
                        transportMessage.sourceActorId,
                        message.operation ==
                            protocol::AuthorityOperation::Prepare))
                {
                    diagnostics_.Event(
                        "MultiplayerMapAuthorityBaselineDeferred",
                        "bounded host baseline preparation queue was full");
                }
                return true;
            }
            // Guests cannot grant or release authority themselves.
            diagnostics_.Event(
                "MultiplayerAuthorityMessageRejected",
                "non-host authority mutation was ignored");
            return true;
        }
        if (message.operation == protocol::AuthorityOperation::Prepared)
        {
            if (message.scope != protocol::AuthorityScope::MapSimulation ||
                message.ownerActorId != localActorId_ ||
                message.mapId != preparedLocalMapId_)
            {
                return true;
            }
            preparedLocalBaselineRevision_ =
                message.mapBaselineRevision;
            (void)mapIdentities_.ObserveAuthoritative(
                message.mapName, message.mapId);
            preparedLocalMapName_ = message.mapName;
            preparationRetry_.Acknowledge();
            diagnostics_.Event(
                "MultiplayerMapPreparationAcknowledged",
                "host saved-map baseline is staged ahead of retail map construction");
            return true;
        }
        if (message.operation == protocol::AuthorityOperation::Request ||
            message.operation == protocol::AuthorityOperation::Prepare)
        {
            diagnostics_.Event(
                "MultiplayerAuthorityMessageRejected",
                "host sent an invalid map authority request");
            return true;
        }
        bool applied = false;
        if (message.scope == protocol::AuthorityScope::MapSimulation)
        {
            (void)mapIdentities_.ObserveAuthoritative(
                message.mapName, message.mapId);
            if (message.operation == protocol::AuthorityOperation::Grant &&
                (mapBaselineGate_ == nullptr ||
                    !mapBaselineGate_->IsGuestGrantReady(
                        message.mapId,
                        message.mapBaselineRevision)))
            {
                diagnostics_.Event(
                    "MultiplayerMapAuthorityBaselineMissing",
                    "map grant was fenced because its host baseline revision was not ready");
                return true;
            }
            applied = maps_.Apply(message);
        }
        else if (message.scope == protocol::AuthorityScope::EntitySimulation)
        {
            applied = ApplySimulationAssignment(message);
        }
        else if (message.scope == protocol::AuthorityScope::EntityAction)
        {
            // Action grants carry no map id on the wire (the codec requires
            // 0 for this scope); the map name identifies the lease.
            const MapAuthorityLease* const map = message.mapId != 0
                ? maps_.Find(message.mapId)
                : maps_.Find(message.mapName);
            if (message.operation == protocol::AuthorityOperation::Release ||
                (map != nullptr && map->epoch == message.mapEpoch))
            {
                applied = actions_.Apply(message);
            }
        }
        if (!applied)
        {
            diagnostics_.Event(
                "MultiplayerAuthorityMessageStale",
                "authority grant was invalid or fenced by a newer epoch");
        }
        return true;
    }

    bool AuthorityReplication::RequestActionLease(
        const protocol::EntityActionMessage& intent,
        std::uint64_t sourceActorId,
        ActionAuthorityLease& grantedLease)
    {
        grantedLease = {};
        if (!initialized_ || role_ != PeerRole::Host ||
            sourceActorId == 0)
        {
            return false;
        }
        const auto actor = actorMaps_.find(sourceActorId);
        const MapAuthorityLease* const map = maps_.Find(intent.mapName);
        if (actor == actorMaps_.end() || actor->second != intent.mapName ||
            map == nullptr || map->epoch != intent.mapEpoch ||
            !actions_.HostAcquire(
                intent,
                sourceActorId,
                *map,
                grantedLease))
        {
            diagnostics_.Event(
                "MultiplayerActionAuthorityRejected",
                "intent actor, occupancy, entity, or map fence was invalid");
            return false;
        }
        if (!PublishHostMessages())
        {
            diagnostics_.Event(
                "MultiplayerActionAuthorityPublishDeferred",
                "granted action lease remains queued ahead of its action begin");
        }
        return true;
    }

    bool AuthorityReplication::RequestLocalMap(
        const PlayerState* localPlayer)
    {
        if (localPlayer == nullptr || localPlayer->actorId != localActorId_ ||
            localPlayer->mapName.empty() || localPlayer->mapId == 0 ||
            requestedLocalMapId_ == localPlayer->mapId)
        {
            return true;
        }

        const MapAuthorityLease* const observed = maps_.Find(
            localPlayer->mapId);
        const std::uint32_t observedEpoch = observed != nullptr
            ? observed->epoch
            : 0;
        if (!SubmitMapRequest(
                protocol::AuthorityOperation::Request,
                localPlayer->mapName,
                localPlayer->mapId,
                observedEpoch))
        {
            return false;
        }
        requestedLocalMapId_ = localPlayer->mapId;
        if (preparedLocalMapId_ == localPlayer->mapId)
        {
            preparedLocalMapId_ = 0;
            preparedLocalBaselineRevision_ = 0;
            preparedLocalMapName_.clear();
            preparationRetry_.Acknowledge();
        }
        return true;
    }

    bool AuthorityReplication::SubmitMapRequest(
        protocol::AuthorityOperation operation,
        const std::string& mapName,
        std::uint16_t mapId,
        std::uint32_t observedEpoch)
    {
        if ((operation != protocol::AuthorityOperation::Request &&
                operation != protocol::AuthorityOperation::Prepare) ||
            mapName.empty() || mapId == 0 || transport_ == nullptr)
        {
            return false;
        }
        if (role_ == PeerRole::Host)
        {
            const bool accepted = operation ==
                    protocol::AuthorityOperation::Prepare
                ? maps_.HostPrepare(
                    mapName,
                    mapId,
                    localActorId_,
                    observedEpoch)
                : maps_.HostRequest(
                    mapName,
                    mapId,
                    localActorId_,
                    observedEpoch);
            return accepted && QueueHostBaselinePreparation(
                mapId,
                mapName,
                localActorId_,
                false);
        }

        protocol::AuthorityMessage request;
        request.operation = operation;
        request.scope = protocol::AuthorityScope::MapSimulation;
        request.ownerActorId = localActorId_;
        request.mapId = mapId;
        request.mapEpoch = observedEpoch;
        request.mapName = mapName;
        std::array<std::uint8_t, protocol::MaximumDatagramBytes> payload = {};
        std::size_t payloadSize = 0;
        if (!protocol::EncodeAuthorityMessage(
                request,
                payload.data(),
                protocol::MaximumPayloadBytes(),
                payloadSize) ||
            !transport_->SubmitReliable(
                reliable_stream::Control,
                protocol::PacketType::Authority,
                payload.data(),
                payloadSize))
        {
            diagnostics_.Event(
                "MultiplayerMapAuthorityRequestDeferred",
                operation == protocol::AuthorityOperation::Prepare
                    ? "ordered transport could not accept the pre-load map preparation"
                    : "ordered transport could not accept the active map request");
            return false;
        }
        return true;
    }

    bool AuthorityReplication::QueueHostBaselinePreparation(
        std::uint16_t mapId,
        const std::string& mapName,
        std::uint64_t requesterActorId,
        bool acknowledge)
    {
        if (role_ != PeerRole::Host || mapId == 0 || mapName.empty() ||
            requesterActorId == 0)
        {
            return false;
        }
        const auto existing = std::find_if(
            pendingBaselinePreparations_.begin(),
            pendingBaselinePreparations_.end(),
            [mapId, requesterActorId, acknowledge](
                const BaselinePreparation& preparation)
            {
                return preparation.mapId == mapId &&
                    preparation.acknowledge == acknowledge &&
                    (!acknowledge || preparation.requesterActorId ==
                        requesterActorId);
            });
        if (existing != pendingBaselinePreparations_.end())
        {
            return true;
        }
        constexpr std::size_t MaximumPendingMapBaselines = 64;
        if (pendingBaselinePreparations_.size() >=
            MaximumPendingMapBaselines)
        {
            return false;
        }
        BaselinePreparation preparation;
        preparation.mapId = mapId;
        preparation.requesterActorId = requesterActorId;
        preparation.mapName = mapName;
        preparation.acknowledge = acknowledge;
        pendingBaselinePreparations_.push_back(preparation);
        return true;
    }

    MapBaselinePreparationResult
    AuthorityReplication::PrepareHostCollectionForPeers()
    {
        if (role_ != PeerRole::Host || transport_ == nullptr)
        {
            return MapBaselinePreparationResult::Failed;
        }
        if (transport_->ConnectedPeerCount() == 0)
        {
            // The local host lease has no network consumer. Capturing and
            // queueing the whole save here can stall startup before a guest
            // exists, and provides no ordering guarantee to a future peer.
            return MapBaselinePreparationResult::Ready;
        }
        if (mapBaselineGate_ == nullptr)
        {
            return MapBaselinePreparationResult::Failed;
        }
        return mapBaselineGate_->PrepareHostCollection(
            transport_->PeerSetRevision());
    }

    bool AuthorityReplication::PublishHostBaselinePreparations()
    {
        if (role_ != PeerRole::Host)
        {
            return true;
        }
        if (mapBaselineGate_ == nullptr)
        {
            return false;
        }
        while (!pendingBaselinePreparations_.empty())
        {
            BaselinePreparation& preparation =
                pendingBaselinePreparations_.front();
            const MapBaselinePreparationResult result =
                mapBaselineGate_->PrepareHostGrant(
                    preparation.mapId,
                    preparation.revision);
            if (result == MapBaselinePreparationResult::Failed)
            {
                return false;
            }
            if (result == MapBaselinePreparationResult::Deferred)
            {
                return true;
            }
            if (preparation.acknowledge)
            {
                protocol::AuthorityMessage prepared;
                prepared.operation = protocol::AuthorityOperation::Prepared;
                prepared.scope = protocol::AuthorityScope::MapSimulation;
                prepared.ownerActorId = preparation.requesterActorId;
                prepared.mapId = preparation.mapId;
                prepared.mapBaselineRevision = preparation.revision;
                prepared.mapName = preparation.mapName;
                std::array<
                    std::uint8_t,
                    protocol::MaximumDatagramBytes> payload = {};
                std::size_t payloadSize = 0;
                if (!protocol::EncodeAuthorityMessage(
                        prepared,
                        payload.data(),
                        protocol::MaximumPayloadBytes(),
                        payloadSize))
                {
                    return false;
                }
                if (!transport_->SubmitReliable(
                        reliable_stream::Control,
                        protocol::PacketType::Authority,
                        payload.data(),
                        payloadSize))
                {
                    if (transport_->HasFailed())
                    {
                        return false;
                    }
                    if (!transportBackpressureReported_)
                    {
                        transportBackpressureReported_ = true;
                        diagnostics_.Event(
                            "MultiplayerAuthorityReplicationDeferred",
                            "ordered baseline traffic is draining before the map preparation acknowledgement");
                    }
                    return true;
                }
                transportBackpressureReported_ = false;
            }
            pendingBaselinePreparations_.pop_front();
        }
        return true;
    }

    bool AuthorityReplication::ReleaseActionLease(
        const EntityAuthorityKey& entity,
        std::uint64_t requestingActorId,
        std::uint32_t actionEpoch)
    {
        if (!initialized_ || role_ != PeerRole::Host ||
            !actions_.HostRelease(
                entity,
                requestingActorId,
                actionEpoch))
        {
            return false;
        }
        if (!PublishHostMessages())
        {
            diagnostics_.Event(
                "MultiplayerActionAuthorityPublishDeferred",
                "released action lease remains queued after its action end");
        }
        return true;
    }

    bool AuthorityReplication::TouchActionLease(
        const EntityAuthorityKey& entity,
        std::uint64_t actorId,
        std::uint32_t actionEpoch) noexcept
    {
        return initialized_ && role_ == PeerRole::Host &&
            actions_.HostTouch(entity, actorId, actionEpoch);
    }

    bool AuthorityReplication::PublishHostMessages()
    {
        protocol::AuthorityMessage message;
        while (maps_.TakePending(message))
        {
            if (message.operation == protocol::AuthorityOperation::Grant)
            {
                if (mapBaselineGate_ == nullptr)
                {
                    maps_.RestorePending(std::move(message));
                    return false;
                }
                const MapBaselinePreparationResult result =
                    mapBaselineGate_->PrepareHostGrant(
                        message.mapId,
                        message.mapBaselineRevision);
                if (result != MapBaselinePreparationResult::Ready)
                {
                    maps_.RestorePending(std::move(message));
                    if (result == MapBaselinePreparationResult::Deferred &&
                        !baselinePreparationDeferredReported_)
                    {
                        baselinePreparationDeferredReported_ = true;
                        diagnostics_.Event(
                            "MultiplayerMapAuthorityBaselineDeferred",
                            "host map grant waits behind its exact saved-map baseline revision");
                    }
                    return result == MapBaselinePreparationResult::Deferred;
                }
                baselinePreparationDeferredReported_ = false;
            }
            std::array<std::uint8_t, protocol::MaximumDatagramBytes> payload = {};
            std::size_t payloadSize = 0;
            if (!protocol::EncodeAuthorityMessage(
                    message,
                    payload.data(),
                    protocol::MaximumPayloadBytes(),
                    payloadSize))
            {
                maps_.RestorePending(std::move(message));
                diagnostics_.Event(
                    "MultiplayerAuthorityReplicationFailed",
                    "host produced an invalid map authority message");
                return false;
            }
            if (!transport_->SubmitReliable(
                    reliable_stream::Control,
                    protocol::PacketType::Authority,
                    payload.data(),
                    payloadSize))
            {
                maps_.RestorePending(std::move(message));
                if (transport_->HasFailed())
                {
                    diagnostics_.Event(
                        "MultiplayerAuthorityReplicationFailed",
                        "ordered transport failed while publishing map authority");
                    return false;
                }
                if (!transportBackpressureReported_)
                {
                    transportBackpressureReported_ = true;
                    diagnostics_.Event(
                        "MultiplayerAuthorityReplicationDeferred",
                        "ordered baseline traffic is draining before the map grant");
                }
                return true;
            }
            transportBackpressureReported_ = false;
        }
        while (actions_.TakePending(message))
        {
            std::array<std::uint8_t, protocol::MaximumDatagramBytes> payload = {};
            std::size_t payloadSize = 0;
            if (!protocol::EncodeAuthorityMessage(
                    message,
                    payload.data(),
                    protocol::MaximumPayloadBytes(),
                    payloadSize))
            {
                actions_.RestorePending(std::move(message));
                diagnostics_.Event(
                    "MultiplayerAuthorityReplicationFailed",
                    "host produced an invalid action authority message");
                return false;
            }
            if (!transport_->SubmitReliable(
                    reliable_stream::Control,
                    protocol::PacketType::Authority,
                    payload.data(),
                    payloadSize))
            {
                actions_.RestorePending(std::move(message));
                if (transport_->HasFailed())
                {
                    diagnostics_.Event(
                        "MultiplayerAuthorityReplicationFailed",
                        "ordered transport failed while publishing action authority");
                    return false;
                }
                if (!transportBackpressureReported_)
                {
                    transportBackpressureReported_ = true;
                    diagnostics_.Event(
                        "MultiplayerAuthorityReplicationDeferred",
                        "ordered baseline traffic is draining before action authority");
                }
                return true;
            }
            transportBackpressureReported_ = false;
        }
        return PublishSimulationAssignments();
    }

    const ActionAuthorityLease* AuthorityReplication::FindActionLease(
        const EntityAuthorityKey& entity) const noexcept
    {
        return actions_.Find(entity);
    }

    const MapAuthorityLease* AuthorityReplication::FindMapLease(
        const std::string& mapName) const noexcept
    {
        return maps_.Find(mapName);
    }

    const MapAuthorityLease* AuthorityReplication::FindMapLease(
        const std::uint16_t mapId) const noexcept
    {
        return maps_.Find(mapId);
    }

    std::vector<MapAuthorityLease> AuthorityReplication::MapLeases() const
    {
        return maps_.Snapshot();
    }

    bool AuthorityReplication::IsMapPublisher(
        const std::string& mapName,
        std::uint64_t actorId,
        std::uint32_t mapEpoch) const noexcept
    {
        const MapAuthorityLease* const lease = maps_.Find(mapName);
        return lease != nullptr && actorId != 0 && mapEpoch != 0 &&
            lease->actorId == actorId && lease->epoch == mapEpoch;
    }

    bool AuthorityReplication::IsMapPublisher(
        const std::uint16_t mapId,
        const std::uint64_t actorId,
        const std::uint32_t mapEpoch) const noexcept
    {
        const MapAuthorityLease* const lease = maps_.Find(mapId);
        return lease != nullptr && actorId != 0 && mapEpoch != 0 &&
            lease->actorId == actorId && lease->epoch == mapEpoch;
    }

    bool AuthorityReplication::IsEntityPublisher(
        const EntityAuthorityKey& entity,
        const std::string& mapName,
        std::uint64_t actorId,
        std::uint32_t mapEpoch) const noexcept
    {
        const MapAuthorityLease* const lease = maps_.Find(mapName);
        if (lease == nullptr || actorId == 0 || mapEpoch == 0 ||
            lease->epoch != mapEpoch)
        {
            return false;
        }
        const SimulationAssignment* const assigned =
            FindSimulationAssignment(entity, mapName, mapEpoch);
        return assigned != nullptr
            ? assigned->actorId == actorId
            : lease->actorId == actorId;
    }

    bool AuthorityReplication::IsEntityPublisher(
        const EntityAuthorityKey& entity,
        const std::uint16_t mapId,
        const std::uint64_t actorId,
        const std::uint32_t mapEpoch) const noexcept
    {
        const MapAuthorityLease* const lease = maps_.Find(mapId);
        if (lease == nullptr)
        {
            return false;
        }
        return IsEntityPublisher(entity, lease->mapName, actorId, mapEpoch);
    }

    const AuthorityReplication::SimulationAssignment*
        AuthorityReplication::FindSimulationAssignment(
            const EntityAuthorityKey& entity,
            const std::string& mapName,
            const std::uint32_t mapEpoch) const noexcept
    {
        const auto found = simulationAssignments_.find(entity);
        if (found == simulationAssignments_.end() ||
            found->second.mapEpoch != mapEpoch ||
            found->second.mapName != mapName)
        {
            return nullptr;
        }
        return &found->second;
    }

    std::uint64_t AuthorityReplication::EntitySimulationOwner(
        const EntityAuthorityKey& entity,
        const std::string& mapName,
        const std::uint32_t mapEpoch) const noexcept
    {
        const MapAuthorityLease* const lease = maps_.Find(mapName);
        if (lease == nullptr || lease->epoch != mapEpoch)
        {
            return 0;
        }
        const SimulationAssignment* const assigned =
            FindSimulationAssignment(entity, mapName, mapEpoch);
        return assigned != nullptr ? assigned->actorId : lease->actorId;
    }

    std::uint64_t AuthorityReplication::ResolveSimulationOwner(
        void* const context, const entities::WorldEntityRecord& record)
    {
        const auto* const self = static_cast<const AuthorityReplication*>(context);
        if (self == nullptr || record.thingUid == 0 || record.generation == 0)
        {
            return 0;
        }
        const SimulationAssignment* const assigned = self->FindSimulationAssignment(
            {record.thingUid, record.generation}, record.mapName, record.mapEpoch);
        return assigned != nullptr ? assigned->actorId : 0;
    }

    bool AuthorityReplication::AssignEntitySimulation(
        const EntityAuthorityKey& entity,
        const std::string& mapName,
        const std::uint32_t mapEpoch,
        const std::uint64_t actorId)
    {
        if (role_ != PeerRole::Host || entity.thingUid == 0 ||
            entity.generation == 0 || actorId == 0 || mapName.empty())
        {
            return false;
        }
        const MapAuthorityLease* const lease = maps_.Find(mapName);
        if (lease == nullptr || lease->epoch != mapEpoch)
        {
            return false;
        }
        if (EntitySimulationOwner(entity, mapName, mapEpoch) == actorId)
        {
            return true;
        }
        protocol::AuthorityMessage message;
        message.operation = protocol::AuthorityOperation::Grant;
        message.scope = protocol::AuthorityScope::EntitySimulation;
        message.ownerActorId = actorId;
        message.entityUid = entity.thingUid;
        message.entityGeneration = entity.generation;
        message.mapEpoch = mapEpoch;
        message.actionEpoch = ++nextSimulationEpoch_;
        message.mapName = mapName;
        const auto pending = simulationPreparations_.find(entity);
        if (pending != simulationPreparations_.end() &&
            pending->second.message.ownerActorId == actorId &&
            pending->second.message.mapEpoch == mapEpoch &&
            GetTickCount64() - pending->second.startedAt < 3000) return true;
        message.operation = protocol::AuthorityOperation::Prepare;
        diagnostics_.Event("MultiplayerNpcHandoffPreparing", "old lease retained until destination binding acknowledges");
        simulationPreparations_[entity] = {message, GetTickCount64()};
        if (actorId != localActorId_) pendingSimulationAssignments_.push_back(std::move(message));
        return true;
    }

    bool AuthorityReplication::ApplySimulationAssignment(
        const protocol::AuthorityMessage& message)
    {
        const EntityAuthorityKey key{message.entityUid, message.entityGeneration};
        const auto existing = simulationAssignments_.find(key);
        if (existing != simulationAssignments_.end() &&
            existing->second.mapEpoch == message.mapEpoch &&
            existing->second.mapName == message.mapName &&
            existing->second.epoch >= message.actionEpoch)
        {
            return false;
        }
        if (message.operation == protocol::AuthorityOperation::Release)
        {
            if (existing != simulationAssignments_.end())
            {
                simulationAssignments_.erase(existing);
                ++simulationRevision_;
            }
            return true;
        }
        if (message.operation != protocol::AuthorityOperation::Grant ||
            message.ownerActorId == 0)
        {
            return false;
        }
        // Bounded: assignments only matter for the current lease of a map.
        if (simulationAssignments_.size() >= 4096)
        {
            for (auto iterator = simulationAssignments_.begin();
                iterator != simulationAssignments_.end();)
            {
                const MapAuthorityLease* const lease = maps_.Find(iterator->second.mapName);
                iterator = lease == nullptr || lease->epoch != iterator->second.mapEpoch
                    ? simulationAssignments_.erase(iterator)
                    : std::next(iterator);
            }
        }
        simulationAssignments_[key] = {
            message.ownerActorId, message.mapEpoch, message.actionEpoch, message.mapName};
        if (message.actionEpoch > nextSimulationEpoch_)
        {
            nextSimulationEpoch_ = message.actionEpoch;
        }
        ++simulationRevision_;
        char detail[256] = {};
        std::snprintf(detail, sizeof(detail),
            "thing_uid=%016llX generation=%u owner=%llu map=%s map_epoch=%u epoch=%u local=%d",
            static_cast<unsigned long long>(message.entityUid), message.entityGeneration,
            static_cast<unsigned long long>(message.ownerActorId), message.mapName.c_str(),
            message.mapEpoch, message.actionEpoch, message.ownerActorId == localActorId_ ? 1 : 0);
        diagnostics_.Event("MultiplayerEntitySimulationAssigned", detail);
        return true;
    }

    void AuthorityReplication::ObserveReadyEntity(const EntityAuthorityKey& entity,
        const std::string& mapName, std::uint32_t mapEpoch)
    {
        auto pending = simulationPreparations_.find(entity);
        if (pending == simulationPreparations_.end()) return;
        auto message = pending->second.message;
        if (message.ownerActorId != localActorId_ || message.mapName != mapName || message.mapEpoch != mapEpoch) return;
        const auto* lease = maps_.Find(mapName);
        if (lease == nullptr || lease->epoch != mapEpoch) { simulationPreparations_.erase(pending); return; }
        if (role_ == PeerRole::Host)
        {
            message.operation = protocol::AuthorityOperation::Grant;
            if (ApplySimulationAssignment(message)) pendingSimulationAssignments_.push_back(message);
        }
        else
        {
            message.operation = protocol::AuthorityOperation::Prepared;
            pendingSimulationAssignments_.push_back(message);
        }
        diagnostics_.Event("MultiplayerNpcHandoffBindingReady", "exact canonical incarnation is bound locally");
        simulationPreparations_.erase(entity);
    }

    bool AuthorityReplication::PublishSimulationAssignments()
    {
        while (!pendingSimulationAssignments_.empty())
        {
            const protocol::AuthorityMessage& message = pendingSimulationAssignments_.front();
            std::array<std::uint8_t, protocol::MaximumDatagramBytes> payload = {};
            std::size_t payloadSize = 0;
            if (!protocol::EncodeAuthorityMessage(
                    message, payload.data(), protocol::MaximumPayloadBytes(), payloadSize))
            {
                pendingSimulationAssignments_.pop_front();
                diagnostics_.Event(
                    "MultiplayerAuthorityReplicationFailed",
                    "host produced an invalid NPC simulation assignment");
                continue;
            }
            if (!transport_->SubmitReliable(
                    reliable_stream::Control,
                    protocol::PacketType::Authority,
                    payload.data(),
                    payloadSize))
            {
                return !transport_->HasFailed();
            }
            pendingSimulationAssignments_.pop_front();
        }
        return true;
    }

    bool AuthorityReplication::IsEntityActionPublisher(
        const EntityAuthorityKey& entity,
        const std::string& mapName,
        std::uint64_t actorId,
        std::uint32_t mapEpoch) const noexcept
    {
        const ActionAuthorityLease* const action = actions_.Find(entity);
        if (action != nullptr &&
            action->kind != protocol::ActionLeaseKind::Ambient &&
            action->mapName == mapName && action->mapEpoch == mapEpoch)
        {
            return action->actorId == actorId;
        }
        return IsEntityPublisher(entity, mapName, actorId, mapEpoch);
    }

    bool AuthorityReplication::HasEntityActionPublisher(
        const std::string& mapName,
        std::uint64_t actorId,
        std::uint32_t mapEpoch) const noexcept
    {
        return actions_.HasOwnedOverride(mapName, actorId, mapEpoch);
    }

    const std::string* AuthorityReplication::ResolveMapName(
        std::uint16_t mapId) const noexcept
    {
        return mapIdentities_.FindName(mapId);
    }

    std::uint16_t AuthorityReplication::ResolveMapId(
        const std::string& mapName) const noexcept
    {
        return mapIdentities_.FindId(mapName);
    }

    bool AuthorityReplication::IsHost() const noexcept
    {
        return initialized_ && role_ == PeerRole::Host;
    }

    void AuthorityReplication::Shutdown() noexcept
    {
        maps_.Clear();
        actions_.Clear();
        simulationAssignments_.clear();
        pendingSimulationAssignments_.clear();
        simulationPreparations_.clear();
        nextSimulationEpoch_ = 0;
        ++simulationRevision_;
        mapIdentities_.Clear();
        mapBaselineGate_ = nullptr;
        transport_ = nullptr;
        diagnostics_ = {};
        role_ = PeerRole::Guest;
        localActorId_ = 0;
        knownPeerRevision_ = 0;
        requestedLocalMapId_ = 0;
        preparedLocalMapId_ = 0;
        preparedLocalBaselineRevision_ = 0;
        preparedLocalMapName_.clear();
        preparationRetry_.Acknowledge();
        pendingBaselinePreparations_.clear();
        actorMaps_.clear();
        baselinePreparationDeferredReported_ = false;
        transportBackpressureReported_ = false;
        initialized_ = false;
    }
}

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveAuthoritySink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.authority;
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkAuthority,
    0x1001u,
    "authority",
    100u,
    "multiplayer-authority-dispatch",
    ResolveAuthoritySink);

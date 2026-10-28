#include "Multiplayer/Authority/AuthorityReplication.h"
#include "QuestEventReplicationService.h"

#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    using fable::multiplayer::protocol::QuestEventMessage;
    using fable::multiplayer::protocol::QuestEventOperation;

    fable::multiplayer::ReliableMessageSink* ResolveQuestEventSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.questEvents;
    }

    const char* OperationName(const QuestEventOperation operation) noexcept
    {
        switch (operation)
        {
        case QuestEventOperation::Activate: return "activate";
        case QuestEventOperation::Deactivate: return "deactivate";
        case QuestEventOperation::Complete: return "complete";
        case QuestEventOperation::Fail: return "fail";
        case QuestEventOperation::ShowQuestScreen: return "show-quest-screen";
        case QuestEventOperation::CloseQuestScreen: return "close-quest-screen";
        }
        return "unknown";
    }

    // Arguments after the quest name are flags in every observed retail call.
    // Anything wider might be a pointer into the sender's address space and
    // must never be replayed.
    bool HasReplayableArguments(const QuestEventMessage& message) noexcept
    {
        for (const std::uint32_t argument : message.arguments)
        {
            if (argument > 1) return false;
        }
        return true;
    }

    // Mutation-test seam: the acceptance suite proves its quest gate can fail
    // by launching both peers with sync disabled.
    bool SyncDisabledForTest() noexcept
    {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA(
            "ALBIONTOGETHER_QUEST_SYNC_DISABLED", value, sizeof(value));
        return length != 0 && length < sizeof(value) && value[0] == '1';
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkQuestEvent,
    0x100Bu,
    "quest-event",
    815u,
    "multiplayer-quest-event-dispatch",
    ResolveQuestEventSink);

namespace fable::multiplayer::persistence
{
    void QuestEventReplicationService::Initialize(
        const PeerRole role,
        const std::uint64_t localActorId,
        UdpPeer& transport,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        role_ = role;
        localActorId_ = localActorId;
        transport_ = &transport;
        diagnostics_ = diagnostics;
        if (SyncDisabledForTest())
        {
            diagnostics_.Event(
                "MultiplayerQuestEventSyncDisabled",
                "ALBIONTOGETHER_QUEST_SYNC_DISABLED=1; live quest transitions stay local");
            return;
        }
        enabled_ = hook_.Install(GetModuleHandleW(nullptr), diagnostics_);
        if (enabled_) hook_.SetDuplicateFilter(&IsRemoteDuplicate, this);
        if (enabled_) screenHook_.Install(GetModuleHandleW(nullptr), diagnostics_);
        if (!enabled_)
        {
            diagnostics_.Event(
                "MultiplayerQuestEventSyncBlocked",
                "quest lifecycle hook unavailable; live quest transitions stay local");
        }
    }

    bool QuestEventReplicationService::IsRemoteDuplicate(
        void* const context, const QuestEventOperation operation, const char* const name) noexcept
    {
        // Only a completion or failure pays out; a peer-driven one already did.
        if (operation != QuestEventOperation::Complete && operation != QuestEventOperation::Fail)
        {
            return false;
        }
        auto* const self = static_cast<QuestEventReplicationService*>(context);
        std::lock_guard<std::mutex> lock(self->lastOperationMutex_);
        try
        {
            const auto last = self->lastOperation_.find(name);
            return last != self->lastOperation_.end() && last->second == operation;
        }
        catch (...)
        {
            return false;
        }
    }

    void QuestEventReplicationService::Shutdown() noexcept
    {
        hook_.SetDuplicateFilter(nullptr, nullptr);
        screenHook_.Shutdown();
        pendingCloses_.clear();
        screenShownAt_.clear();
        screenAuthority_ = nullptr;
        screenHero_ = nullptr;
        screenMapId_ = 0;
        hook_.Shutdown();
        transport_ = nullptr;
        diagnostics_ = {};
        role_ = PeerRole::Guest;
        localActorId_ = 0;
        nextRevision_ = 0;
        outbound_.clear();
        pending_.clear();
        {
            std::lock_guard<std::mutex> lock(lastOperationMutex_);
            lastOperation_.clear();
        }
        enabled_ = false;
    }

    bool QuestEventReplicationService::Process()
    {
        if (!enabled_) return true;
        if (transport_ == nullptr || transport_->HasFailed()) return false;

        QuestLifecycleHook::Observation observation;
        while (hook_.PopObservation(observation))
        {
            if (!PublishLocal(observation)) return false;
        }
        screenHook_.Poll(screenHero_);
        const auto screenNow = GetTickCount64();
        for (auto it = pendingCloses_.begin(); it != pendingCloses_.end(); )
        {
            const auto* lease = screenAuthority_ != nullptr ? screenAuthority_->FindMapLease(it->second.mapId) : nullptr;
            if (screenNow - it->second.receivedAt > 15000 || screenMapId_ != it->second.mapId ||
                lease == nullptr || lease->epoch != it->second.epoch || screenHook_.Close(screenHero_, it->first.c_str()))
                it = pendingCloses_.erase(it);
            else ++it;
        }
        QuestScreenHook::Observation screen;
        while (screenHook_.PopObservation(screen))
        {
            if (!PublishScreen(screen)) return false;
        }

        // Replays run in arrival order; stop at the first one that must wait
        // for the manager so causality (activate before complete) holds.
        const std::uint64_t now = GetTickCount64();
        while (!pending_.empty())
        {
            if (DropTransientPair()) continue;
            const PendingReplay& front = pending_.front();
            if (front.message.operation == QuestEventOperation::Activate &&
                now - front.receivedAt < ActivateSettleMilliseconds &&
                !hook_.IsActive(front.message.name))
            {
                break;
            }
            if (!TryReplay(front)) break;
            pending_.pop_front();
        }

        for (std::size_t submitted = 0;
             submitted < SubmissionBudget && !outbound_.empty(); ++submitted)
        {
            if (!Submit(outbound_.front()))
            {
                return !transport_->HasFailed();
            }
            outbound_.pop_front();
        }
        return true;
    }

    bool QuestEventReplicationService::PublishLocal(
        const QuestLifecycleHook::Observation& observation)
    {
        QuestEventMessage message;
        message.operation = observation.operation;
        std::memcpy(message.arguments, observation.arguments, sizeof(message.arguments));
        if (observation.name.empty() ||
            observation.name.size() >= protocol::MaximumQuestEventNameBytes)
        {
            return true;
        }
        message.nameLength = static_cast<std::uint8_t>(observation.name.size());
        std::memcpy(message.name, observation.name.data(), observation.name.size());
        message.revision = ++nextRevision_;
        try
        {
            std::lock_guard<std::mutex> lock(lastOperationMutex_);
            lastOperation_[observation.name] = observation.operation;
        }
        catch (...)
        {
            return false;
        }
        if (outbound_.size() >= PendingLimit)
        {
            Report("MultiplayerQuestEventDropped", message, localActorId_, "outbound-full");
            return false;
        }
        outbound_.push_back(message);
        Report("MultiplayerQuestEventLocal", message, localActorId_,
            role_ == PeerRole::Host ? "broadcast" : "report-to-host");
        return true;
    }

    bool QuestEventReplicationService::PublishScreen(const QuestScreenHook::Observation& observation)
    {
        if (observation.closed)
        {
            const auto* lease = screenAuthority_ != nullptr ? screenAuthority_->FindMapLease(screenMapId_) : nullptr;
            if (lease == nullptr || observation.name.empty() ||
                observation.name.size() >= protocol::MaximumQuestEventNameBytes) return true;
            QuestEventMessage close;
            close.operation = QuestEventOperation::CloseQuestScreen;
            close.nameLength = static_cast<std::uint8_t>(observation.name.size());
            std::memcpy(close.name, observation.name.data(), observation.name.size());
            close.arguments[0] = screenMapId_;
            close.arguments[1] = lease->epoch;
            close.revision = ++nextRevision_;
            if (outbound_.size() >= PendingLimit) return false;
            outbound_.push_back(close);
            Report("MultiplayerQuestScreenOwnerConfirmed", close, localActorId_, "any-player-close-on-map-peers");
            return true;
        }
        if (observation.name.empty() || observation.name.size() >= protocol::MaximumQuestEventNameBytes)
        {
            return true;
        }
        try
        {
            screenShownAt_[observation.name] = GetTickCount64();
        }
        catch (...)
        {
        }
        QuestEventMessage message;
        message.operation = QuestEventOperation::ShowQuestScreen;
        message.arguments[0] = observation.mode ? 1u : 0u;
        message.nameLength = static_cast<std::uint8_t>(observation.name.size());
        std::memcpy(message.name, observation.name.data(), observation.name.size());
        message.revision = ++nextRevision_;
        if (outbound_.size() >= PendingLimit)
        {
            Report("MultiplayerQuestEventDropped", message, localActorId_, "outbound-full");
            return true;
        }
        outbound_.push_back(message);
        Report("MultiplayerQuestEventLocal", message, localActorId_,
            role_ == PeerRole::Host ? "broadcast" : "report-to-host");
        return true;
    }

    bool QuestEventReplicationService::Submit(const QuestEventMessage& message)
    {
        std::array<std::uint8_t, protocol::QuestEventFixedBytes +
            protocol::MaximumQuestEventNameBytes> bytes = {};
        std::size_t encodedSize = 0;
        if (!protocol::EncodeQuestEventMessage(message, bytes.data(), bytes.size(), encodedSize))
        {
            Report("MultiplayerQuestEventDropped", message, localActorId_, "encode");
            return true;
        }
        // Hosts broadcast to every connected guest; a guest's only reliable
        // peer is the host.
        if (!transport_->SubmitReliable(
                reliable_stream::Control,
                protocol::PacketType::QuestEvent,
                bytes.data(),
                encodedSize))
        {
            return false;
        }
        Report("MultiplayerQuestEventSent", message, localActorId_, "submitted");
        return true;
    }

    bool QuestEventReplicationService::HandleReliableMessage(
        const TransportMessage& transportMessage)
    {
        if (transportMessage.type != protocol::PacketType::QuestEvent)
        {
            return false;
        }
        PendingReplay pending;
        if (!protocol::DecodeQuestEventMessage(
                transportMessage.payload.data(), transportMessage.payloadSize,
                pending.message))
        {
            diagnostics_.Event("MultiplayerQuestEventRejected", "invalid quest event packet");
            return true;
        }
        if (!enabled_) return true;
        pending.sourceActorId = transportMessage.sourceActorId;
        pending.receivedAt = GetTickCount64();
        // Only the host fans out; a guest report becomes a host broadcast so
        // every other guest converges too.
        pending.rebroadcast = role_ == PeerRole::Host;
        Report("MultiplayerQuestEventReceived", pending.message,
            pending.sourceActorId, "queued");
        if (pending_.size() >= PendingLimit)
        {
            Report("MultiplayerQuestEventDropped", pending.message,
                pending.sourceActorId, "pending-full");
            return true;
        }
        pending_.push_back(pending);
        return true;
    }

    bool QuestEventReplicationService::DropTransientPair()
    {
        // Retail polls some ambient quests (village events) by activating and
        // immediately deactivating them. By the time such a pair arrives the
        // sender is already back where it started, so replaying the activate
        // would only rerun the quest script and its side effects here.
        const PendingReplay& front = pending_.front();
        if (front.message.operation != QuestEventOperation::Activate ||
            hook_.IsActive(front.message.name))
        {
            return false;
        }
        for (std::size_t index = 1; index < pending_.size(); ++index)
        {
            const QuestEventMessage& later = pending_[index].message;
            if (std::strcmp(later.name, front.message.name) != 0) continue;
            if (later.operation != QuestEventOperation::Deactivate) return false;
            Report("MultiplayerQuestEventCoalesced", front.message,
                front.sourceActorId, "activate-then-deactivate-dropped");
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            pending_.pop_front();
            return true;
        }
        return false;
    }

    bool QuestEventReplicationService::NeedsReplay(const QuestEventMessage& message) const
    {
        {
            std::lock_guard<std::mutex> lock(lastOperationMutex_);
            const auto last = lastOperation_.find(message.name);
            if (last != lastOperation_.end() && last->second == message.operation)
            {
                return false;
            }
        }
        const bool active = hook_.IsActive(message.name);
        const bool completed = hook_.IsCompleted(message.name);
        switch (message.operation)
        {
        case QuestEventOperation::Activate: return !active && !completed;
        case QuestEventOperation::Deactivate: return active;
        case QuestEventOperation::Complete: return !completed;
        // This build exposes no failed predicate (slot 302 is a constant
        // false), so a fail is replayed only while the quest is running here.
        case QuestEventOperation::Fail: return active && !completed;
        case QuestEventOperation::ShowQuestScreen:
        case QuestEventOperation::CloseQuestScreen: return true;
        }
        return false;
    }

    bool QuestEventReplicationService::TryReplay(const PendingReplay& pending)
    {
        if (!hook_.IsManagerReady()) return false;
        const QuestEventMessage& message = pending.message;
        if (message.operation == QuestEventOperation::CloseQuestScreen)
        {
            const auto* lease = screenAuthority_ != nullptr ? screenAuthority_->FindMapLease(
                static_cast<std::uint16_t>(message.arguments[0])) : nullptr;
            if (lease == nullptr || lease->epoch != message.arguments[1] ||
                message.arguments[0] > 0xFFFF)
            {
                Report("MultiplayerQuestEventRejected", message, pending.sourceActorId, "stale-map-screen");
                return true;
            }
            if (message.arguments[0] == screenMapId_ && pendingCloses_.size() < PendingLimit)
                pendingCloses_[message.name] = {GetTickCount64(), screenMapId_, lease->epoch};
            if (pending.rebroadcast && outbound_.size() < PendingLimit)
            {
                auto relay = message;
                relay.revision = ++nextRevision_;
                outbound_.push_back(relay);
            }
            return true;
        }
        if (message.operation == QuestEventOperation::ShowQuestScreen)
        {
            void* const scriptInterface = scriptInterface_ != nullptr ? *scriptInterface_ : nullptr;
            if (scriptInterface == nullptr) return false;
            const auto previous = screenShownAt_.find(message.name);
            const std::uint64_t now = GetTickCount64();
            if (previous != screenShownAt_.end() && now - previous->second < ScreenDedupMilliseconds)
            {
                Report("MultiplayerQuestEventSatisfied", message, pending.sourceActorId, "quest-screen-already-shown");
                return true;
            }
            const bool shown = screenHook_.Replay(scriptInterface, message.name, message.arguments[0] != 0);
            if (shown)
            {
                try
                {
                    screenShownAt_[message.name] = now;
                }
                catch (...)
                {
                }
            }
            Report(shown ? "MultiplayerQuestEventApplied" : "MultiplayerQuestEventApplyFailed",
                message, pending.sourceActorId, shown ? "quest-screen" : "quest-screen-rejected");
            if (pending.rebroadcast && outbound_.size() < PendingLimit)
            {
                QuestEventMessage relay = message;
                relay.revision = ++nextRevision_;
                outbound_.push_back(relay);
            }
            return true;
        }
        if (!HasReplayableArguments(message))
        {
            Report("MultiplayerQuestEventSkipped", message, pending.sourceActorId,
                "non-flag-arguments");
            return true;
        }
        if (NeedsReplay(message))
        {
            const bool accepted = hook_.Replay(message.operation, message.name, message.arguments);
            if (accepted)
            {
                try
                {
                    std::lock_guard<std::mutex> lock(lastOperationMutex_);
                    lastOperation_[message.name] = message.operation;
                }
                catch (...)
                {
                    return false;
                }
            }
            Report(accepted ? "MultiplayerQuestEventApplied" : "MultiplayerQuestEventApplyFailed",
                message, pending.sourceActorId, accepted ? "native" : "native-rejected");
        }
        else
        {
            Report("MultiplayerQuestEventSatisfied", message, pending.sourceActorId,
                "local-state-already-matches");
        }
        if (pending.rebroadcast)
        {
            QuestEventMessage relay = message;
            relay.revision = ++nextRevision_;
            if (outbound_.size() < PendingLimit)
            {
                outbound_.push_back(relay);
            }
            else
            {
                Report("MultiplayerQuestEventDropped", relay, localActorId_, "outbound-full");
            }
        }
        return true;
    }

    void QuestEventReplicationService::Report(
        const char* const event,
        const QuestEventMessage& message,
        const std::uint64_t sourceActorId,
        const char* const outcome) const noexcept
    {
        char detail[256] = {};
        std::snprintf(detail, sizeof(detail),
            "op=%s quest=%s revision=%llu source=%llu args=%u,%u,%u outcome=%s",
            OperationName(message.operation), message.name,
            static_cast<unsigned long long>(message.revision),
            static_cast<unsigned long long>(sourceActorId),
            message.arguments[0], message.arguments[1], message.arguments[2],
            outcome);
        diagnostics_.Event(event, detail);
    }
}

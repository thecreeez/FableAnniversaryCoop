#include "QuestFieldSyncService.h"

#include "Multiplayer/Persistence/QuestEventReplicationService.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <algorithm>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveQuestFieldSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.questFields;
    }

    // Per-player engine bookkeeping, not story state: GameflowAssistance is
    // the combat-assist script whose frame counters tick every frame on each
    // client. Syncing them made both peers echo updates ~10 times a second.
    bool IsLocalOnlyField(const std::string& quest, const std::string& field) noexcept
    {
        return quest == "GameflowAssistance" || field.find("FramesSince") != std::string::npos;
    }

    bool SyncDisabledForTest() noexcept
    {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA(
            "ALBIONTOGETHER_QUEST_FIELD_SYNC_DISABLED", value, sizeof(value));
        return length != 0 && length < sizeof(value) && value[0] == '1';
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkQuestField,
    0x100Cu,
    "quest-field",
    816u,
    "multiplayer-quest-field-dispatch",
    ResolveQuestFieldSink);

namespace fable::multiplayer::persistence
{
    void QuestFieldSyncService::Initialize(
        const PeerRole role,
        const std::uint64_t localActorId,
        UdpPeer& transport,
        QuestEventReplicationService& questEvents,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        questEvents_ = &questEvents;
        role_ = role;
        localActorId_ = localActorId;
        transport_ = &transport;
        diagnostics_ = diagnostics;
        if (SyncDisabledForTest())
        {
            diagnostics_.Event(
                "MultiplayerQuestFieldSyncDisabled",
                "ALBIONTOGETHER_QUEST_FIELD_SYNC_DISABLED=1; quest progress fields stay local");
            return;
        }
        enabled_ = recorder_.Install(GetModuleHandleW(nullptr), diagnostics_);
    }

    void QuestFieldSyncService::Shutdown() noexcept
    {
        recorder_.Shutdown();
        transport_ = nullptr;
        questEvents_ = nullptr;
        mappedGeneration_ = 0;
        nextRemapAt_ = 0;
        diagnostics_ = {};
        role_ = PeerRole::Guest;
        localActorId_ = 0;
        nextRevision_ = 0;
        nextScanAt_ = 0;
        fields_.clear();
        known_.clear();
        outbound_.clear();
        inbound_.clear();
        mappedOnce_ = false;
        enabled_ = false;
    }

    std::string QuestFieldSyncService::Key(const std::string& quest, const std::string& field)
    {
        return quest + "." + field;
    }

    bool QuestFieldSyncService::Remap()
    {
        const std::uint32_t generation = questEvents_ != nullptr
            ? questEvents_->Hook().Generation() : 0;
        const bool mapped = recorder_.Record(fields_);
        fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
            [](const QuestFieldRecorder::Field& field) { return IsLocalOnlyField(field.quest, field.name); }),
            fields_.end());
        if (mapped)
        {
            mappedGeneration_ = generation;
            nextRemapAt_ = GetTickCount64() + RemapIntervalMilliseconds;
        }
        if (mapped && !mappedOnce_)
        {
            mappedOnce_ = true;
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "fields=%zu", fields_.size());
            diagnostics_.Event("MultiplayerQuestFieldMapReady", detail);
        }
        return mapped;
    }

    const QuestFieldRecorder::Field* QuestFieldSyncService::Find(
        const char* const quest, const char* const field) const
    {
        for (const QuestFieldRecorder::Field& candidate : fields_)
        {
            if (candidate.quest == quest && candidate.name == field) return &candidate;
        }
        return nullptr;
    }

    void QuestFieldSyncService::Publish(const QuestFieldRecorder::Field& field, const std::uint32_t bits)
    {
        if (field.quest.size() >= protocol::MaximumQuestFieldNameBytes ||
            field.name.size() >= protocol::MaximumQuestFieldNameBytes)
        {
            return;
        }
        protocol::QuestFieldMessage message;
        message.revision = ++nextRevision_;
        message.kind = static_cast<std::uint8_t>(field.kind);
        message.bits = bits;
        message.questLength = static_cast<std::uint8_t>(field.quest.size());
        message.fieldLength = static_cast<std::uint8_t>(field.name.size());
        std::memcpy(message.quest, field.quest.data(), field.quest.size());
        std::memcpy(message.field, field.name.data(), field.name.size());
        if (outbound_.size() >= QueueLimit)
        {
            Report("MultiplayerQuestFieldDropped", message, localActorId_, "outbound-full");
            return;
        }
        outbound_.push_back(message);
        Report("MultiplayerQuestFieldLocal", message, localActorId_,
            role_ == PeerRole::Host ? "broadcast" : "report-to-host");
    }

    bool QuestFieldSyncService::Process()
    {
        if (!enabled_) return true;
        if (transport_ == nullptr || transport_->HasFailed()) return false;

        const std::uint64_t now = GetTickCount64();
        const bool scanDue = now >= nextScanAt_;
        const bool lifecycleChanged = questEvents_ != nullptr &&
            questEvents_->Hook().Generation() != mappedGeneration_;
        if (!mappedOnce_ || lifecycleChanged || !inbound_.empty() || now >= nextRemapAt_)
        {
            // Script objects come and go with quest activation; never compare
            // or write through a map older than the last transition.
            if (!Remap()) return true;
        }

        try
        {
            while (!inbound_.empty())
            {
                const Pending pending = inbound_.front();
                inbound_.pop_front();
                const protocol::QuestFieldMessage& message = pending.message;
                const QuestFieldRecorder::Field* const field = Find(message.quest, message.field);
                if (field == nullptr || static_cast<std::uint8_t>(field->kind) != message.kind)
                {
                    Report("MultiplayerQuestFieldUnmapped", message, pending.sourceActorId,
                        field == nullptr ? "quest-not-running-here" : "kind-mismatch");
                }
                else
                {
                    std::uint32_t current = 0;
                    const bool same = QuestFieldRecorder::Read(*field, current) && current == message.bits;
                    const bool written = same || QuestFieldRecorder::Write(*field, message.bits);
                    known_[Key(field->quest, field->name)] = message.bits;
                    Report(same ? "MultiplayerQuestFieldSatisfied"
                            : written ? "MultiplayerQuestFieldApplied" : "MultiplayerQuestFieldApplyFailed",
                        message, pending.sourceActorId, same ? "already-equal" : written ? "written" : "fault");
                }
                if (role_ == PeerRole::Host && outbound_.size() < QueueLimit)
                {
                    // Fan a guest's progress out to every other guest.
                    protocol::QuestFieldMessage relay = message;
                    relay.revision = ++nextRevision_;
                    outbound_.push_back(relay);
                }
            }

            if (scanDue)
            {
                nextScanAt_ = now + ScanIntervalMilliseconds;
                for (const QuestFieldRecorder::Field& field : fields_)
                {
                    std::uint32_t bits = 0;
                    if (!QuestFieldRecorder::Read(field, bits)) continue;
                    const std::string key = Key(field.quest, field.name);
                    const auto known = known_.find(key);
                    if (known == known_.end())
                    {
                        known_.emplace(key, bits);
                    }
                    else if (known->second != bits)
                    {
                        known->second = bits;
                        Publish(field, bits);
                    }
                }
            }
        }
        catch (...)
        {
            return false;
        }

        std::array<std::uint8_t, protocol::QuestFieldFixedBytes +
            2 * protocol::MaximumQuestFieldNameBytes> bytes = {};
        for (std::size_t submitted = 0;
             submitted < SubmissionBudget && !outbound_.empty(); ++submitted)
        {
            std::size_t encodedSize = 0;
            if (!protocol::EncodeQuestFieldMessage(outbound_.front(), bytes.data(), bytes.size(), encodedSize))
            {
                Report("MultiplayerQuestFieldDropped", outbound_.front(), localActorId_, "encode");
                outbound_.pop_front();
                continue;
            }
            if (!transport_->SubmitReliable(reliable_stream::Control,
                    protocol::PacketType::QuestField, bytes.data(), encodedSize))
            {
                return !transport_->HasFailed();
            }
            Report("MultiplayerQuestFieldSent", outbound_.front(), localActorId_, "submitted");
            outbound_.pop_front();
        }
        return true;
    }

    bool QuestFieldSyncService::HandleReliableMessage(const TransportMessage& transportMessage)
    {
        if (transportMessage.type != protocol::PacketType::QuestField) return false;
        Pending pending;
        if (!protocol::DecodeQuestFieldMessage(
                transportMessage.payload.data(), transportMessage.payloadSize, pending.message))
        {
            diagnostics_.Event("MultiplayerQuestFieldRejected", "invalid quest field packet");
            return true;
        }
        if (!enabled_) return true;
        pending.sourceActorId = transportMessage.sourceActorId;
        if (inbound_.size() >= QueueLimit)
        {
            Report("MultiplayerQuestFieldDropped", pending.message, pending.sourceActorId, "inbound-full");
            return true;
        }
        try
        {
            inbound_.push_back(pending);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    bool QuestFieldSyncService::ReadFieldForTest(const char* const quest, const char* const field, std::uint32_t& bits)
    {
        bits = 0;
        if (!enabled_ || !Remap()) return false;
        const QuestFieldRecorder::Field* const mapped = Find(quest, field);
        return mapped != nullptr && QuestFieldRecorder::Read(*mapped, bits);
    }

    bool QuestFieldSyncService::WriteFieldForTest(const char* const quest, const char* const field, const std::uint32_t bits)
    {
        if (!enabled_ || !Remap()) return false;
        const QuestFieldRecorder::Field* const mapped = Find(quest, field);
        return mapped != nullptr && QuestFieldRecorder::Write(*mapped, bits);
    }

    void QuestFieldSyncService::Report(
        const char* const event,
        const protocol::QuestFieldMessage& message,
        const std::uint64_t sourceActorId,
        const char* const outcome) const noexcept
    {
        char detail[256] = {};
        std::snprintf(detail, sizeof(detail),
            "quest=%s field=%s kind=%u value=%u revision=%llu source=%llu outcome=%s",
            message.quest, message.field, static_cast<unsigned>(message.kind), message.bits,
            static_cast<unsigned long long>(message.revision),
            static_cast<unsigned long long>(sourceActorId), outcome);
        diagnostics_.Event(event, detail);
    }
}

#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Quest/Sync/QuestFieldRecorder.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Protocol/QuestFieldMessage.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace fable::multiplayer
{
    class UdpPeer;
}

namespace fable::multiplayer::persistence
{
    class QuestEventReplicationService;

    // Shared intermediate quest progress. Every peer periodically maps its
    // quest-script progress fields, publishes values that changed locally,
    // and writes values received from others. Guests publish to the host;
    // the host applies and rebroadcasts to every guest. Applied values are
    // remembered as already-published so they never echo back.
    //
    // Concurrent edits of the same field resolve last-writer-wins at the host.
    class QuestFieldSyncService final : public ReliableMessageSink
    {
    public:
        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes()
            const noexcept override
        {
            static constexpr protocol::PacketType types[] = {
                protocol::PacketType::QuestField};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(
            PeerRole role,
            std::uint64_t localActorId,
            UdpPeer& transport,
            QuestEventReplicationService& questEvents,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;

        // Game thread only.
        bool Process();
        void ResetForReload() noexcept;
        bool HandleReliableMessage(const TransportMessage& message) override;

        [[nodiscard]] bool IsActive() const noexcept { return enabled_; }

        // Acceptance seams (game thread): read or change a live field exactly
        // as quest script code would, through the current mapping.
        bool ReadFieldForTest(const char* quest, const char* field, std::uint32_t& bits);
        bool WriteFieldForTest(const char* quest, const char* field, std::uint32_t bits);

    private:
        struct Pending final
        {
            protocol::QuestFieldMessage message;
            std::uint64_t sourceActorId = 0;
        };

        // Change detection is a plain memory compare of the mapped fields.
        static constexpr std::uint64_t ScanIntervalMilliseconds = 100;
        // Building the map re-persists every running script, so it happens only after a
        // lifecycle transition, before applying a remote value, and on this
        // slow safety cadence.
        static constexpr std::uint64_t RemapIntervalMilliseconds = 10'000;
        static constexpr std::size_t QueueLimit = 512;
        static constexpr std::size_t SubmissionBudget = 32;

        bool Remap();
        const QuestFieldRecorder::Field* Find(const char* quest, const char* field) const;
        void Publish(const QuestFieldRecorder::Field& field, std::uint32_t bits);
        void Report(const char* event, const protocol::QuestFieldMessage& message,
            std::uint64_t sourceActorId, const char* outcome) const noexcept;
        static std::string Key(const std::string& quest, const std::string& field);

        UdpPeer* transport_ = nullptr;
        QuestEventReplicationService* questEvents_ = nullptr;
        std::uint32_t mappedGeneration_ = 0;
        std::uint64_t nextRemapAt_ = 0;
        core::Diagnostics diagnostics_ = {};
        QuestFieldRecorder recorder_;
        PeerRole role_ = PeerRole::Guest;
        std::uint64_t localActorId_ = 0;
        std::uint64_t nextRevision_ = 0;
        std::uint64_t nextScanAt_ = 0;
        std::vector<QuestFieldRecorder::Field> fields_;
        // Last value this peer published or applied, per quest.field. A field
        // seen for the first time is a baseline, not a change.
        std::unordered_map<std::string, std::uint32_t> known_;
        std::deque<protocol::QuestFieldMessage> outbound_;
        std::deque<Pending> inbound_;
        bool mappedOnce_ = false;
        bool enabled_ = false;
    };
}

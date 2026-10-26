#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Quest/Sync/QuestLifecycleHook.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Protocol/QuestEventMessage.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fable::multiplayer
{
    class UdpPeer;
}

namespace fable::multiplayer::persistence
{
    // Live quest lifecycle sync with shared progress: whichever peer starts,
    // completes, fails or abandons a quest does so for everyone.
    //
    // Guests report local transitions to the host. The host applies a guest
    // report, then broadcasts every transition (its own and guests') to all
    // guests. Receivers replay through the original CQuestManager method only
    // when their native state differs, so echoes and duplicates are inert and
    // a peer in another region still converges immediately.
    class QuestEventReplicationService final : public ReliableMessageSink
    {
    public:
        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes()
            const noexcept override
        {
            static constexpr protocol::PacketType types[] = {
                protocol::PacketType::QuestEvent};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(
            PeerRole role,
            std::uint64_t localActorId,
            UdpPeer& transport,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;

        // Game thread only: publishes local transitions and replays pending
        // remote ones once the local quest manager exists.
        bool Process();

        bool HandleReliableMessage(const TransportMessage& message) override;

        [[nodiscard]] bool IsActive() const noexcept { return enabled_; }
        // Test seam used by acceptance drivers; lifecycle state queries go to
        // the same native predicates replay uses.
        [[nodiscard]] QuestLifecycleHook& Hook() noexcept { return hook_; }

    private:
        struct PendingReplay final
        {
            protocol::QuestEventMessage message;
            std::uint64_t sourceActorId = 0;
            std::uint64_t receivedAt = 0;
            bool rebroadcast = false;
        };

        static constexpr std::size_t PendingLimit = 128;
        // Retail polls ambient quests by activating and deactivating them a
        // few frames apart; a remote activate waits this long for its
        // deactivate so the pair can be dropped instead of replayed.
        static constexpr std::uint64_t ActivateSettleMilliseconds = 1'000;
        static constexpr std::size_t SubmissionBudget = 8;

        bool Submit(const protocol::QuestEventMessage& message);
        bool PublishLocal(const QuestLifecycleHook::Observation& observation);
        // Returns true when the event is done (applied, already satisfied or
        // rejected); false when the manager is not ready yet.
        bool TryReplay(const PendingReplay& pending);
        // Drops an activate/deactivate pair for a quest that is not running
        // here; returns true when the queue front was consumed.
        bool DropTransientPair();
        [[nodiscard]] bool NeedsReplay(const protocol::QuestEventMessage& message) const;
        void Report(const char* event, const protocol::QuestEventMessage& message,
            std::uint64_t sourceActorId, const char* outcome) const noexcept;

        static bool IsRemoteDuplicate(void* context,
            protocol::QuestEventOperation operation, const char* name) noexcept;

        UdpPeer* transport_ = nullptr;
        // Guards lastOperation_, read from quest-script threads.
        mutable std::mutex lastOperationMutex_;
        core::Diagnostics diagnostics_ = {};
        QuestLifecycleHook hook_;
        PeerRole role_ = PeerRole::Guest;
        std::uint64_t localActorId_ = 0;
        std::uint64_t nextRevision_ = 0;
        std::deque<protocol::QuestEventMessage> outbound_;
        std::deque<PendingReplay> pending_;
        // Last transition this peer performed per quest, local or replayed.
        // Retail commits completion on a later tick, so the native predicates
        // alone cannot stop an echo from completing (and rewarding) twice.
        std::unordered_map<std::string, protocol::QuestEventOperation> lastOperation_;
        bool enabled_ = false;
    };
}

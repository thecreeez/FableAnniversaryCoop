#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Creature/Actions/Hooks/CreatureActionLifecycleObserver.h"
#include "Game/Creature/Combat/CreatureAbilityEvent.h"
#include "Multiplayer/Protocol/EntityActionMessage.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <utility>
#include <vector>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fable::game::creature::animation { class CreatureAnimationService; }

namespace fable::multiplayer
{
    class UdpPeer;
}

namespace fable::multiplayer::authority
{
    class AuthorityReplication;
}

namespace fable::multiplayer::entities
{
    class EntityLifecycleReplication;
    class EntityNetworkIdentityRegistry;
    class EntityPresenceReplication;
}

namespace fable::multiplayer::combat
{
    class CombatActionLedger;
    class PlayerCombatantDirectory;
}

namespace fable::game::creature::combat
{
    class CreatureCombatService;
}

namespace fable::multiplayer::replication
{
    // Converts accepted native creature actions into fenced semantic actions.
    // Native callbacks only enqueue; authority and transport remain game-thread
    // work. Unknown action classes are named but never serialized by memory.
    class EntityActionReplication final : public ReliableMessageSink
    {
    public:
        // Observers play the owner's resolved NPC animations through it.
        void SetAnimationService(game::creature::animation::CreatureAnimationService* animation) noexcept
        {
            animation_ = animation;
        }

        // NPCs this process simulates that recently aimed an action at a
        // player (local or remote Hero). Game thread.
        struct Engagement final
        {
            std::uint64_t thingUid = 0;
            void* creature = nullptr;
        };
        [[nodiscard]] std::vector<Engagement> EngagedCreatures(
            std::uint64_t now, std::uint64_t window);
        // Any NPC this process simulates whose last action within window
        // targeted the player actor (acceptance checks).
        // An NPC's hit on a player counts as engagement: many AI attacks
        // (wasps' Fodder attacks) carry no target pointer.
        static void __cdecl ObserveHit(void* context, std::uint64_t sourceThingUid, std::uint64_t targetThingUid);
        [[nodiscard]] bool IsAnyEngagedWith(
            std::uint64_t actorId, std::uint64_t now, std::uint64_t window) const;

        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes()
            const noexcept override
        {
            static constexpr protocol::PacketType types[] = {
                protocol::PacketType::EntityAction};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(
            PeerRole role,
            std::uint64_t localActorId,
            UdpPeer& transport,
            authority::AuthorityReplication& authority,
            entities::EntityLifecycleReplication& lifecycle,
            entities::EntityNetworkIdentityRegistry& identities,
            entities::EntityPresenceReplication& presence,
            combat::PlayerCombatantDirectory& combatants,
            combat::CombatActionLedger& combatLedger,
            game::creature::combat::CreatureCombatService& combat,
            const core::Diagnostics& diagnostics);
        bool Attach(
            game::creature::actions::CreatureActionLifecycleObserver& observer);
        bool ProcessPending(
            const std::string& localMap,
            bool ownerRosterReady);
        bool HandleReliableMessage(
            const TransportMessage& message) override;
        void Shutdown() noexcept;

    private:
        static constexpr std::size_t PendingEventCapacity = 8192;
        static constexpr std::size_t PendingMessageCapacity = 8192;
        // A newly materialized Fable creature can wait several seconds for
        // its first retail AI-brain dispatch. Keep the primary-attacker lease
        // alive long enough for that decision and its combat actions to remain
        // on the attacking peer before declaring the engagement idle.
        static constexpr std::uint64_t CombatIdleMilliseconds = 10'000;

        struct ActiveAction final
        {
            std::uint64_t entityUid = 0;
            std::uint32_t entityGeneration = 0;
            std::uint64_t actionId = 0;
            std::uint64_t ownerActorId = 0;
            std::uint64_t targetEntityUid = 0;
            std::uint32_t targetEntityGeneration = 0;
            std::uint64_t targetPlayerActorId = 0;
            std::uint32_t mapEpoch = 0;
            std::uint32_t actionEpoch = 0;
            std::uint32_t abilityId = 0;
            float abilityCharge = 0.0f;
            protocol::EntityActionKind kind =
                protocol::EntityActionKind::Native;
            std::uint8_t flags = 0;
            std::string mapName;
            std::string semanticName;
            void* nativeAction = nullptr;
            std::uint64_t lastActivityAt = 0;
            bool localOrigin = false;
            bool combatEngagement = false;
            bool finished = false;
            bool endQueued = false;
            bool pendingRelease = false;
            // A combat sub-action may borrow its actor's already ordered
            // PrimaryAttacker lease. Ending that sub-action must not release
            // the engagement that owns the lease.
            bool ownsLease = true;
            bool nativeAbility = false;
            bool abilityReplayed = false;
            bool abilityReplayFailed = false;
            bool nativeReplayed = false;
            bool nativeReplayFailed = false;
            std::uint32_t nativeReplayAttempts = 0;
            std::uint64_t nextNativeReplayAt = 0;
            // Retail animation the owner's action resolved (PlayAnimation,
            // Fodder attacks); observers play the same clip on their copy.
            std::uint32_t animationId = 0;
            bool animationReplayed = false;
        };

        static void CaptureEvent(
            void* context,
            const game::creature::actions::CreatureActionLifecycleEvent& event);
        static void CaptureAbility(
            void* context,
            const game::creature::combat::CreatureAbilityEvent& event);
        void Enqueue(
            const game::creature::actions::CreatureActionLifecycleEvent& event)
            noexcept;
        void EnqueueAbility(
            const game::creature::combat::CreatureAbilityEvent& event) noexcept;
        bool ProcessEvent(
            const game::creature::actions::CreatureActionLifecycleEvent& event,
            const std::string& localMap,
            std::uint32_t mapEpoch);
        bool BeginLocalAction(
            const game::creature::actions::CreatureActionLifecycleEvent& event,
            const std::string& localMap,
            std::uint32_t mapEpoch);
        bool BeginLocalAbility(
            const game::creature::combat::CreatureAbilityEvent& event,
            const std::string& localMap,
            std::uint32_t mapEpoch);
        bool BindLocalAbilityAction(
            const game::creature::actions::CreatureActionLifecycleEvent& event);
        bool FinishLocalAction(void* nativeAction);
        bool BeginOrRefreshCombatEngagement(
            const game::creature::combat::CreatureAbilityEvent& event,
            const std::string& localMap,
            std::uint32_t mapEpoch);
        bool QueueUpdate(ActiveAction& action);
        bool ExpireCombatEngagements(std::uint64_t now);
        void ForgetCombatEngagement(const ActiveAction& action) noexcept;
        bool HostAcceptIntent(
            protocol::EntityActionMessage intent,
            std::uint64_t sourceActorId);
        bool AcceptAuthoritative(
            const protocol::EntityActionMessage& message);
        bool ReplayAuthoritativeAbility(ActiveAction& action);
        bool ReplayAuthoritativeUseObject(ActiveAction& action);
        void RecordEngagement(
            const game::creature::actions::CreatureActionLifecycleEvent& event);
        // creature pointer + local native uid -> last time it acted on a player
        struct EngagementRecord final
        {
            void* creature = nullptr;
            std::uint64_t at = 0;
            std::uint64_t targetActorId = 0;
        };
        std::unordered_map<std::uint64_t, EngagementRecord> engagements_;
        bool ReplayAuthoritativeNativeAction(ActiveAction& action);
        bool ReplayPendingAbilities();
        bool ReplayPendingNativeActions();
        bool PublishPeerBaseline();
        bool QueueEnd(ActiveAction& action);
        bool Queue(protocol::EntityActionMessage message);
        void TrackCombatAction(
            const protocol::EntityActionMessage& message) noexcept;
        bool PublishPending();
        void PruneFencedActions();
        bool FinalizeCompletedHostActions();
        [[nodiscard]] protocol::EntityActionMessage ToMessage(
            const ActiveAction& action,
            protocol::EntityActionPhase phase) const;
        [[nodiscard]] std::uint64_t NextActionId() noexcept;
        [[nodiscard]] static protocol::EntityActionKind Classify(
            const std::string& actionType) noexcept;
        [[nodiscard]] static std::uint8_t FlagsFor(
            protocol::EntityActionKind kind) noexcept;

        game::creature::actions::CreatureActionLifecycleObserver* observer_ =
            nullptr;
        game::creature::combat::CreatureCombatService* combat_ = nullptr;
        game::creature::animation::CreatureAnimationService* animation_ = nullptr;
        // Owner presentation (PlayAnimation) currently playing per fenced NPC
        // native creature. The NPC's own AI submission runs with this clip.
        // Read on engine threads at submission, written on the game thread.
        struct OwnerPresentation final
        {
            std::uint32_t animationId = 0;
            std::uint64_t receivedAt = 0;
            bool started = false;
        };
        static bool PresentationGate(void* context, void* creature, const char* actionType, bool begin);
        mutable SRWLOCK presentationLock_ = SRWLOCK_INIT;
        std::unordered_map<void*, OwnerPresentation> ownerPresentations_;
        std::atomic<std::uint64_t> presentationsStarted_{0};
        UdpPeer* transport_ = nullptr;
        authority::AuthorityReplication* authority_ = nullptr;
        entities::EntityLifecycleReplication* lifecycle_ = nullptr;
        entities::EntityNetworkIdentityRegistry* identities_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        combat::PlayerCombatantDirectory* combatants_ = nullptr;
        combat::CombatActionLedger* combatLedger_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        PeerRole role_ = PeerRole::Guest;
        std::uint64_t localActorId_ = 0;
        std::uint64_t nextActionId_ = 0;
        std::uint64_t knownPeerRevision_ = 0;
        std::mutex pendingEventMutex_;
        std::deque<game::creature::actions::CreatureActionLifecycleEvent>
            pendingEvents_;
        std::deque<game::creature::combat::CreatureAbilityEvent>
            pendingAbilities_;
        std::deque<protocol::EntityActionMessage> pendingMessages_;
        std::unordered_map<std::uint64_t, ActiveAction> activeActions_;
        std::unordered_map<void*, std::uint64_t> localActionIds_;
        std::unordered_map<std::uint64_t, std::uint64_t> combatActionIds_;
        std::unordered_map<std::uint64_t, std::uint64_t> recentAbilityActions_;
        std::atomic_bool acceptingEvents_{false};
        std::atomic_uint droppedEvents_{0};
        unsigned int reportedDroppedEvents_ = 0;
        bool nonOwnerActionReported_ = false;
        bool publishBackpressured_ = false;
        bool initialized_ = false;
    };
}

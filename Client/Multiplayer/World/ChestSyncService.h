#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace fable::multiplayer
{
    class UdpPeer;
    namespace entities
    {
        class EntityNetworkIdentityRegistry;
        class EntityPresenceReplication;
    }
    namespace replication
    {
        class LocalHeroReplication;
    }
}

namespace fable::multiplayer::world
{
    // Shared chests. When a player opens a chest (or any reward container),
    // retail CTCContainerRewardHero::Reward gives its contents to that
    // player's Hero; this service then tells every peer. A peer opens its own
    // copy of the chest (lid animation, opened state) and, when retail calls
    // Reward there, receives only the consumables: weapons, clothing, gold
    // and other items stay with the player who opened it (owner rule).
    class ChestSyncService final : public ReliableMessageSink
    {
    public:
        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        {
            static constexpr protocol::PacketType types[] = {protocol::PacketType::ChestOpen};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(
            PeerRole role,
            UdpPeer& transport,
            entities::EntityPresenceReplication& presence,
            entities::EntityNetworkIdentityRegistry& identities,
            replication::LocalHeroReplication& localHero,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread.
        bool Process();
        bool HandleReliableMessage(const TransportMessage& message) override;

    private:
        static constexpr std::size_t MessageBytes = 8 + 8 + 2;
        static constexpr std::size_t QueueLimit = 64;

        struct ChestOpen final
        {
            std::uint64_t revision = 0;
            std::uint64_t thingUid = 0;
            std::uint16_t mapId = 0;
        };

        using RewardFn = bool(__thiscall*)(void* container);
        static bool __fastcall RewardObserved(void* container, void* edx);
        static bool IsSharedConsumable(const char* definitionName) noexcept;
        bool RewardConsumables(void* container) noexcept;
        bool OpenLocally(const ChestOpen& message);
        bool Send(const ChestOpen& message);

        static ChestSyncService* active_;
        core::hooking::InlineHook reward_;
        RewardFn originalReward_ = nullptr;
        std::uintptr_t base_ = 0;
        PeerRole role_ = PeerRole::Guest;
        UdpPeer* transport_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        entities::EntityNetworkIdentityRegistry* identities_ = nullptr;
        replication::LocalHeroReplication* localHero_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::uint64_t nextRevision_ = 0;

        std::mutex mutex_;
        // Containers opened by the local player, reported from the hook.
        std::deque<void*> localOpened_;
        // Containers this peer is opening because another player did; their
        // Reward gives consumables only.
        std::unordered_set<void*> remoteOpening_;
        std::deque<ChestOpen> inbound_;
        std::deque<ChestOpen> outbound_;
    };
}

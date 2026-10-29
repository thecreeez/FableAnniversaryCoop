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

namespace fable::game
{
    class EntityService;
}

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
    // Demon doors use the same channel: each player solves the riddle alone,
    // and once any player's door opens (CTCDoor::SetOpen) it opens for all.
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
            game::EntityService& entities,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread.
        bool Process();
        void ResetForReload() noexcept;
        bool HandleReliableMessage(const TransportMessage& message) override;

        // Demon door packets arrive through their own sink (one per packet
        // type) and are handled here.
        class DoorSink final : public ReliableMessageSink
        {
        public:
            explicit DoorSink(ChestSyncService& owner) noexcept : owner_(owner) {}
            [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes() const noexcept override
            {
                static constexpr protocol::PacketType types[] = {protocol::PacketType::DemonDoorOpen};
                return {types, sizeof(types) / sizeof(types[0])};
            }
            bool HandleReliableMessage(const TransportMessage& message) override
            {
                return owner_.HandleReliableMessage(message);
            }

        private:
            ChestSyncService& owner_;
        };
        [[nodiscard]] ReliableMessageSink& Doors() noexcept { return doorSink_; }

    private:
        static constexpr std::size_t MessageBytes = 8 + 8 + 2;
        static constexpr std::size_t QueueLimit = 64;

        struct ChestOpen final
        {
            std::uint64_t revision = 0;
            std::uint64_t thingUid = 0;
            std::uint16_t mapId = 0;
            bool door = false;
        };

        using RewardFn = bool(__thiscall*)(void* container);
        using SetDoorOpenFn = void(__thiscall*)(void* door, std::uint8_t open);
        static void __fastcall DoorSetOpenObserved(void* door, void* edx, std::uint8_t open);
        bool IsDemonDoor(const void* thing) const;
        bool OpenDoorLocally(const ChestOpen& message);
        static bool __fastcall RewardObserved(void* container, void* edx);
        static bool IsSharedConsumable(const char* definitionName) noexcept;
        bool RewardConsumables(void* container) noexcept;
        bool OpenLocally(const ChestOpen& message);
        bool Send(const ChestOpen& message);

        static ChestSyncService* active_;
        DoorSink doorSink_{*this};
        core::hooking::InlineHook reward_;
        RewardFn originalReward_ = nullptr;
        core::hooking::InlineHook doorSetOpen_;
        SetDoorOpenFn originalDoorSetOpen_ = nullptr;
        static thread_local bool replayingDoor_;
        game::EntityService* entities_ = nullptr;
        // Door Things the local game opened, reported from the hook.
        std::deque<void*> localDoors_;
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

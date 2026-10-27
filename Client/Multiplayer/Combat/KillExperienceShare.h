#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <mutex>

namespace fable::multiplayer
{
    class UdpPeer;
    namespace replication
    {
        class LocalHeroReplication;
    }
}

namespace fable::multiplayer::combat
{
    // Kill experience: the killer keeps the retail 1x, every other player
    // gains 0.3x (owner rule). Retail grants it through
    // CTCHeroExperience::AddGeneralExperience from the kill handler
    // (return address 0x01AC4606); that call is observed and the base amount
    // sent to peers, which add their share through the same function.
    // Strength/skill/will experience (CTCHeroExperience stat vector) is
    // earned in combat; any increase of the local Hero's values is shared
    // the same way (0.3x), spending (decreases) is not.
    class KillExperienceShare final : public ReliableMessageSink
    {
    public:
        [[nodiscard]] ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        {
            static constexpr protocol::PacketType types[] = {protocol::PacketType::KillExperience};
            return {types, sizeof(types) / sizeof(types[0])};
        }

        void Initialize(PeerRole role, UdpPeer& transport, replication::LocalHeroReplication& localHero,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread.
        bool Process();
        bool HandleReliableMessage(const TransportMessage& message) override;

    private:
        static constexpr float PeerShare = 0.3f;
        static constexpr int StatCount = 3;

        struct Grant final
        {
            std::int32_t general = 0;
            std::int32_t stats[StatCount] = {};
        };

        void TrackStatExperience(void* experience);
        static constexpr std::size_t QueueLimit = 64;
        using AddFn = void(__thiscall*)(void* experience, std::int32_t amount);
        static void __fastcall AddObserved(void* experience, void* edx, std::int32_t amount);

        static KillExperienceShare* active_;
        core::hooking::InlineHook add_;
        AddFn original_ = nullptr;
        std::uintptr_t killReturn_ = 0;
        PeerRole role_ = PeerRole::Guest;
        UdpPeer* transport_ = nullptr;
        replication::LocalHeroReplication* localHero_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::mutex mutex_;
        std::deque<std::int32_t> localKills_;
        std::deque<Grant> inbound_;
        std::deque<Grant> outbound_;
        void* trackedExperience_ = nullptr;
        std::int32_t statBaseline_[StatCount] = {};
    };
}

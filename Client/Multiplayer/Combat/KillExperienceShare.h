#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fable::game { class EntityService; }
namespace fable::multiplayer::entities {
    class EntityPresenceReplication; class EntityNetworkIdentityRegistry; class EntityLifecycleReplication;
}
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
            game::EntityService& entities, entities::EntityPresenceReplication& presence,
            entities::EntityNetworkIdentityRegistry& identities, entities::EntityLifecycleReplication& lifecycle,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread.
        bool Process();
        bool HandleReliableMessage(const TransportMessage& message) override;
        // Acceptance seam: treat amount as a local kill grant.
        void InjectKillForTest(std::int32_t amount);
        bool CollectOrbForAcceptance(std::uint64_t uid);

    private:
        static constexpr float PeerShare = 0.3f;
        static constexpr int StatCount = 3;

        #pragma pack(push, 1)
        struct Grant final
        {
            std::uint8_t operation = 0; // reward, claim request, grant, collected, cancel
            std::uint64_t originActorId = 0;
            std::uint64_t grantId = 0;
            std::uint64_t originNonce = 0;
            std::uint64_t itemUid = 0;
            std::uint64_t capturedAt = 0;
            std::uint16_t mapId = 0;
            std::int32_t gold = 0;
            std::int32_t general = 0;
            std::int32_t stats[StatCount] = {};
        };

        #pragma pack(pop)
        static_assert(sizeof(Grant) == 63);
        using PickupFn = void(__thiscall*)(void*, void*);
        static void __fastcall PickupObserved(void*, void*, void*);
        static thread_local Grant pickupGrant_;
        static thread_local bool inPickup_;
        core::hooking::InlineHook pickupHook_;
        PickupFn originalPickup_ = nullptr;
        game::EntityService* entities_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        entities::EntityNetworkIdentityRegistry* identities_ = nullptr;
        entities::EntityLifecycleReplication* lifecycle_ = nullptr;
        std::unordered_set<std::uint64_t> consumedLoot_;
        struct Claim { std::uint64_t actor = 0; std::uint64_t nonce = 0; std::uint64_t until = 0; };
        std::unordered_map<std::uint64_t, Claim> lootClaims_;
        std::unordered_map<std::uint64_t, std::uint64_t> localPickupRequests_;
        using OrbRecognizeFn = bool(__thiscall*)(void*, void**);
        using OrbCollectFn = void(__thiscall*)(void*, void*);
        using OrbUpdateFn = void(__thiscall*)(void*);
        static bool __fastcall OrbRecognized(void* item, void*, void** component);
        static void __fastcall OrbMerge(void* component, void*, void* other);
        static void __fastcall OrbUpdated(void* component, void*);
        core::hooking::InlineHook orbRecognizeHook_, orbMergeHook_, orbUpdateHook_;
        OrbRecognizeFn originalOrbRecognize_ = nullptr;
        OrbCollectFn originalOrbMerge_ = nullptr;
        OrbUpdateFn originalOrbUpdate_ = nullptr;
        void TrackStatExperience(void* experience);
        double experienceRemainder_[4] = {};
        static constexpr std::size_t QueueLimit = 64;
        using AddFn = void(__thiscall*)(void* experience, std::int32_t amount);
        static void __fastcall AddObserved(void* experience, void* edx, std::int32_t amount);

        static KillExperienceShare* active_;
        core::hooking::InlineHook add_;
        AddFn original_ = nullptr;
        using GoldFn = void(__thiscall*)(void*, std::int32_t, std::uint8_t);
        static void __fastcall GoldObserved(void*, void*, std::int32_t, std::uint8_t);
        core::hooking::InlineHook goldHook_;
        GoldFn originalGold_ = nullptr;
        std::deque<Grant> localGold_;
        std::uint64_t nextGrantId_ = 0;
        std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> lastGrant_;
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

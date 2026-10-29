#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace fable::multiplayer
{
    class UdpPeer;
    namespace entities { class EntityPresenceReplication; class EntityNetworkIdentityRegistry; }
    namespace replication { class LocalHeroReplication; class RemotePlayerChannels; }
    namespace persistence { class SessionCheckpointService; }
}
namespace fable::multiplayer::world
{
    class DestructibleSyncService final : public ReliableMessageSink
    {
    public:
        ReliableMessageTypeSet HandledPacketTypes() const noexcept override
        { static constexpr protocol::PacketType t[] = {protocol::PacketType::Destructible}; return {t, 1}; }
        bool Initialize(PeerRole, UdpPeer&, entities::EntityPresenceReplication&, entities::EntityNetworkIdentityRegistry&,
            replication::LocalHeroReplication&, replication::RemotePlayerChannels&, const core::Diagnostics&) noexcept;
        void Shutdown() noexcept;
        bool Process();
        bool HandleReliableMessage(const TransportMessage&) override;
        void ResetForReload() noexcept;
        void SetCheckpointScope(persistence::SessionCheckpointService* scope) noexcept { checkpoints_ = scope; }
    private:
        struct Broken { std::uint64_t uid = 0, fence = 0; std::uint16_t map = 0, definition = 0; std::uint8_t operation = 1; };
        using SmashFn = void(__thiscall*)(void*);
        static void __fastcall Smash(void*, void*);
        static void __fastcall Wobble(void*, void*);
        static DestructibleSyncService* active_;
        static thread_local bool replaying_;
        bool Apply(const Broken&);
        core::hooking::InlineHook hook_;
        core::hooking::CodePatch wobblePatch_;
        SmashFn originalWobble_ = nullptr;
        SmashFn original_ = nullptr;
        PeerRole role_ = PeerRole::Guest;
        UdpPeer* transport_ = nullptr;
        entities::EntityPresenceReplication* presence_ = nullptr;
        entities::EntityNetworkIdentityRegistry* identities_ = nullptr;
        replication::LocalHeroReplication* hero_ = nullptr;
        replication::RemotePlayerChannels* peers_ = nullptr;
        core::Diagnostics diagnostics_;
        std::mutex mutex_;
        std::deque<Broken> local_, inbound_, outbound_;
        std::unordered_map<std::uint64_t, Broken> committed_;
        std::unordered_set<std::uint64_t> locallyBroken_;
        std::uint64_t peerRevision_ = 0, nextRepairAt_ = 0;
        persistence::SessionCheckpointService* checkpoints_ = nullptr;
    };
}

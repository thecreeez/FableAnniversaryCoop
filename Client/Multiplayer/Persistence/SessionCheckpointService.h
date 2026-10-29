#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Math/Vector3.h"
#include "Game/Native/NativePromptBox.h"
#include "Game/Native/ScriptTypes.h"
#include "Game/Persistence/Hooks/GameStateSectionLoadHook.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

namespace fable::game { class EntityService; }
namespace fable::multiplayer
{
    struct MultiplayerSessionContexts;
    namespace persistence
    {
        // A checkpoint is a committed set of native saves, one per player. No host
        // PLAYER section is copied over a guest's Hero. Reload is a separate barrier.
        class SessionCheckpointService final : public ReliableMessageSink
        {
        public:
            ReliableMessageTypeSet HandledPacketTypes() const noexcept override
            { static constexpr protocol::PacketType t[] = {protocol::PacketType::Checkpoint}; return {t, 1}; }
            bool Initialize(PeerRole, MultiplayerSessionContexts&, game::EntityService&,
                std::uint64_t epoch, const core::Diagnostics&) noexcept;
            void Shutdown() noexcept;
            bool Process();
            bool HandleReliableMessage(const TransportMessage&) override;
            bool IsReloading() const noexcept { return loading_; }
            bool IsBarrierActive() const noexcept { return reloadRequested_ || loading_; }
            std::uint64_t WorldFence() const noexcept { return worldFence_.load(std::memory_order_acquire); }
            bool HasCheckpointForTest() const noexcept { return committed_[0].ticket.revision != 0; }
            std::uint64_t LoadedAttemptForTest() const noexcept { return reloading_.ticket.rollback; }
            float SavedHealthForTest() const noexcept { return committed_[0].health; }
            bool RequestReloadForTest() noexcept;
        private:
            enum class Operation : std::uint8_t { Prepare = 1, Saved, Commit, ReloadPrepare, ReloadReady, ReloadGo, ReloadComplete, HeroDied };
#pragma pack(push, 1)
            struct Ticket final
            {
                std::uint64_t epoch = 0, revision = 0;
                std::uint64_t rollback = 0;
                std::uint64_t quests = 0, regions = 0, factions = 0;
                Operation operation = Operation::Prepare;
                std::uint8_t kind = 0, slot = 0, reserved = 0;
                std::uint16_t map = 0;
                game::Vector3 position;
                float facing = 0;
                char region[96] = {};
            };
#pragma pack(pop)
            static_assert(sizeof(Ticket) == 166);
            struct Saved final
            {
                Ticket ticket;
                std::wstring name, path;
                std::uint64_t size = 0, stamp = 0;
                float health = 0;
            };
            enum class Phase { Idle, SaveFile, AwaitSaved, AwaitReloadReady, AwaitHostBaseline, AwaitGuestLoaded };
            using SaveFn = void(__thiscall*)(void*, const game::native::WideString*, unsigned char, std::uint32_t, std::uint32_t);
            using SelectFn = void(__thiscall*)(void*, int);
            using LoadFn = bool(__thiscall*)(void*, const game::native::WideString*, const game::native::CharString*, unsigned char);
            static SessionCheckpointService* active_;
            static thread_local bool projecting_;
            static void __fastcall SaveObserved(void*, void*, const game::native::WideString*, unsigned char, std::uint32_t, std::uint32_t);
            static void __fastcall LoadSelected(void*, void*, int);
            static bool __fastcall WorldLoadRequested(void*, void*, const game::native::WideString*, const game::native::CharString*, unsigned char);
            bool QueueNativeLoad(const Saved&, bool observe = false) noexcept;
            static void __fastcall TransferRegion(void*, void*, const char*, game::native::CharString*, game::native::CharString*);
            static void BundleLoaded(void*) noexcept;
            static void __fastcall SetLoadButtonEnabled(void*, void*, unsigned int, int);
            bool SaveNative(Saved&) noexcept;
            bool ResolvePath(Saved&) noexcept;
            bool StableFile(Saved&) noexcept;
            bool VerifyFile(const Saved&) const noexcept;
            bool Send(Ticket, Operation);
            bool ShowPartyDeathCard() noexcept;
            bool AllAcknowledged() const;
            void CommitLocal(const Saved&);
            bool BeginReload(const Saved&);
            void FinishReload();
            void ResetTransientReplication();
            void QueueSourceSave(const wchar_t*) noexcept;
            std::uintptr_t base_ = 0;
            PeerRole role_ = PeerRole::Guest;
            MultiplayerSessionContexts* contexts_ = nullptr;
            game::EntityService* entities_ = nullptr;
            core::Diagnostics diagnostics_;
            core::hooking::InlineHook save_, select_, load_;
            core::hooking::InlineHook loadButton_;
            using ButtonFn = void(__thiscall*)(void*, unsigned int, int);
            ButtonFn originalButton_ = nullptr;
            std::atomic_bool worldEntered_{false};
            core::hooking::CodePatch regionCall_;
            game::persistence::GameStateSectionLoadHook bundleHook_;
            SaveFn originalSave_ = nullptr;
            SelectFn originalSelect_ = nullptr;
            LoadFn originalLoad_ = nullptr;
            game::native::NativePromptBox deathPrompt_, waitPrompt_;
            std::atomic_uint sourceSave_{0};
            std::uint64_t epoch_ = 0, revision_ = 0, peerRevision_ = 0, stableWorldSince_ = 0;
            std::uint64_t fileStableSince_ = 0, fileOriginalStamp_ = 0, nextRetryAt_ = 0, phaseSince_ = 0;
            Phase phase_ = Phase::Idle;
            Saved pending_, reloading_;
            std::array<Saved, 2> committed_;
            std::unordered_set<std::uint64_t> acknowledged_;
            std::mutex mutex_;
            std::deque<std::pair<Ticket, std::uint64_t>> inbound_;
            std::atomic_int requestedLoad_{-1};
            bool reloadRequested_ = false, loading_ = false, sawWorldDeparture_ = false;
            void* oldHero_ = nullptr;
            std::uint64_t loadStartedAt_ = 0;
            std::uint64_t remoteHost_ = 0, remoteNonce_ = 0;
            std::atomic_bool hostSessionChanged_{false};
            Ticket deferredGo_;
            Ticket lastLoaded_;
            std::atomic_bool bundleLoaded_{false};
            bool completeAckPending_ = false;
            std::atomic<std::uint64_t> worldFence_{0};
            std::uint64_t reloadCounter_ = 0, nextDeathNoticeAt_ = 0;
            bool partyDeathRequired_ = false, partyDeathCardShown_ = false;
        };
    }
}

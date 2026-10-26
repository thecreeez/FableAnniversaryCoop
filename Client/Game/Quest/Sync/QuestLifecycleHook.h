#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Native/ScriptTypes.h"
#include "Multiplayer/Protocol/QuestEventMessage.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fable::multiplayer::persistence
{
    // Observes the four retail CQuestManager lifecycle transitions and can
    // replay them. Every path that changes quest state (quest scripts, Guild
    // quest cards, the script interface) funnels through these manager
    // methods, so this is the narrowest seam that sees all of them.
    //
    // Detours may run on whichever thread the quest system uses; they only
    // append to a bounded locked queue. Replays happen on the game thread and
    // are tagged so they are not observed again.
    class QuestLifecycleHook final
    {
    public:
        struct Observation final
        {
            protocol::QuestEventOperation operation =
                protocol::QuestEventOperation::Activate;
            std::uint32_t arguments[3] = {};
            std::string name;
        };

        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;

        // Asked before a local (non-replayed) transition runs. Returning true
        // skips the retail call: the same transition already arrived from a
        // peer, and running it again would, for a completion, pay the reward
        // twice. Called on whichever thread runs the quest script.
        using DuplicateFilter = bool (*)(void* context,
            protocol::QuestEventOperation operation, const char* name) noexcept;
        void SetDuplicateFilter(DuplicateFilter filter, void* context) noexcept
        {
            duplicateContext_ = context;
            duplicateFilter_ = filter;
        }
        void Shutdown() noexcept;

        // Moves at most one queued local observation into `observation`.
        bool PopObservation(Observation& observation) noexcept;

        // Replays one transition through the original manager method. Returns
        // false when the manager is not ready or the call faulted.
        bool Replay(
            protocol::QuestEventOperation operation,
            const char* name,
            const std::uint32_t (&arguments)[3]) noexcept;

        // Automation only: performs a transition through the hooked retail
        // entry point, exactly as gameplay would, so it is observed and
        // published like any local quest change.
        bool InvokeAsGameplay(
            protocol::QuestEventOperation operation,
            const char* name,
            const std::uint32_t (&arguments)[3]) noexcept;

        [[nodiscard]] bool IsManagerReady() const noexcept;
        // Bumped after every completed lifecycle transition, local or
        // replayed; quest-script objects may have been created or retired.
        [[nodiscard]] std::uint32_t Generation() const noexcept
        {
            return generation_.load(std::memory_order_acquire);
        }
        [[nodiscard]] bool IsActive(const char* name) const noexcept;
        [[nodiscard]] bool IsCompleted(const char* name) const noexcept;

        [[nodiscard]] bool IsInstalled() const noexcept
        {
            return activate_.IsInstalled() && deactivate_.IsInstalled() &&
                complete_.IsInstalled() && fail_.IsInstalled();
        }

    private:
        using ActivateFn = bool(__thiscall*)(
            void*, game::native::CharString*, std::uint32_t, std::uint32_t);
        using DeactivateFn = void(__thiscall*)(
            void*, game::native::CharString*, std::uint32_t);
        using ResolveFn = void(__thiscall*)(
            void*, game::native::CharString*, std::uint32_t, std::uint32_t, std::uint32_t);
        using PredicateFn = bool(__thiscall*)(void*, const game::native::CharString*);

        static bool __fastcall ActivateObserved(
            void* manager, void*, game::native::CharString* name,
            std::uint32_t a2, std::uint32_t a3);
        static void __fastcall DeactivateObserved(
            void* manager, void*, game::native::CharString* name, std::uint32_t a2);
        static void __fastcall CompleteObserved(
            void* manager, void*, game::native::CharString* name,
            std::uint32_t a2, std::uint32_t a3, std::uint32_t a4);
        static void __fastcall FailObserved(
            void* manager, void*, game::native::CharString* name,
            std::uint32_t a2, std::uint32_t a3, std::uint32_t a4);

        bool IsDuplicate(protocol::QuestEventOperation operation,
            const game::native::CharString* name) noexcept;
        void Record(
            protocol::QuestEventOperation operation,
            const game::native::CharString* name,
            std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) noexcept;
        [[nodiscard]] void* Manager() const noexcept;
        bool Query(PredicateFn predicate, const char* name) const noexcept;
        bool Call(
            protocol::QuestEventOperation operation,
            const char* name,
            const std::uint32_t (&arguments)[3],
            bool replay) noexcept;

        static constexpr std::size_t QueueCapacity = 64;
        static QuestLifecycleHook* active_;
        static thread_local bool replaying_;

        HMODULE gameModule_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        core::hooking::InlineHook activate_;
        core::hooking::InlineHook deactivate_;
        core::hooking::InlineHook complete_;
        core::hooking::InlineHook fail_;
        ActivateFn originalActivate_ = nullptr;
        DeactivateFn originalDeactivate_ = nullptr;
        ResolveFn originalComplete_ = nullptr;
        ResolveFn originalFail_ = nullptr;

        SRWLOCK queueLock_ = SRWLOCK_INIT;
        std::array<Observation, QueueCapacity> queue_ = {};
        std::size_t queueHead_ = 0;
        std::size_t queueCount_ = 0;
        std::size_t dropped_ = 0;
        std::atomic_uint32_t generation_{0};
        DuplicateFilter duplicateFilter_ = nullptr;
        void* duplicateContext_ = nullptr;
    };
}

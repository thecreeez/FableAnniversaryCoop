#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Creature/Animation/Native/AnimationPlaybackFunctions.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fable::game::creature::actions
{
    class CreatureActionLifecycleObserver;
    struct CreatureActionLifecycleEvent;
}

namespace fable::game::creature::animation
{
    // Overrides only the animation choice made while a replicated native
    // creature action starts. The action itself, its timing, movement, hit
    // windows, and effects remain owned by Fable's normal action stack.
    class CreatureActionAnimationSelectionHook final
    {
    public:
        static constexpr std::size_t ActionTypeCapacity = 128;
        struct OwnerAttackFrame final
        {
            void* creature = nullptr;
            void* action = nullptr;
            std::uint64_t observedAt = 0;
            native::AnimationPlaybackRequest request;
            int blendFrames = 0, options = 0;
            char actionType[128] = {};
        };
        static void WatchLocalHero(void* hero) noexcept;
        static bool PopOwnerAttack(OwnerAttackFrame&) noexcept;

        bool Install(
            HMODULE gameModule,
            const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;

        bool BeginSelection(
            void* creature,
            const char* actionType,
            std::uint32_t animationId) noexcept;
        void EndSelection() noexcept;
        static void SetOwnerAttackRequest(const native::AnimationPlaybackRequest* request, int blendFrames, int options) noexcept;
        bool AttachActionLifecycleObserver(
            actions::CreatureActionLifecycleObserver& observer) noexcept;
        void DetachActionLifecycleObserver() noexcept;

        [[nodiscard]] bool IsInstalled() const noexcept;

    private:
        static constexpr std::size_t SelectionCapacity = 32;
        static constexpr std::uint64_t SelectionLifetimeMilliseconds = 2'000;

        using SubmitRequestPointer = native::AnimationPlaybackFunctions::
            SubmitRequestPointer;
        using ValidateAnimationPointer = native::AnimationPlaybackFunctions::
            ValidateAnimationPointer;

        struct Selection final
        {
            std::uint64_t token = 0;
            std::uint64_t expiresAt = 0;
            void* creature = nullptr;
            void* action = nullptr;
            void* animationState = nullptr;
            std::uint32_t animationId = 0;
            std::uint32_t observedAnimationId = 0;
            bool applied = false;
            bool hasOwnerRequest = false;
            native::AnimationPlaybackRequest ownerRequest;
            int blendFrames = 0, options = 0;
            std::array<char, ActionTypeCapacity> actionType = {};
        };

        static void __fastcall Intercept(
            void* animationState,
            void*,
            const native::AnimationPlaybackRequest* request,
            std::int32_t blendFrames,
            std::int32_t options);
        static void ObserveActionLifecycle(
            void* context,
            const actions::CreatureActionLifecycleEvent& event) noexcept;
        static bool ActiveActionMatches(const Selection& selection) noexcept;
        static bool ScopedActiveActionMatches(
            const Selection& selection) noexcept;
        static bool AnimationStateMatches(
            const Selection& selection,
            void* animationState) noexcept;
        static void* ResolveAnimationState(void* creature) noexcept;
        void HandleActionLifecycle(
            const actions::CreatureActionLifecycleEvent& event) noexcept;
        bool TryApplySelection(
            void* animationState,
            const native::AnimationPlaybackRequest& request,
            native::AnimationPlaybackRequest& replacement, int& blendFrames, int& options) noexcept;
        void ReportExpiredSelections(std::uint64_t now) noexcept;
        void ReportSelection(const Selection& selection) const noexcept;

        void CaptureOwnerAttack(void* state, const native::AnimationPlaybackRequest&, int blendFrames, int options) noexcept;
        SRWLOCK ownerCaptureLock_ = SRWLOCK_INIT;
        void* watchedHero_ = nullptr;
        void* watchedState_ = nullptr;
        std::array<OwnerAttackFrame, 64> ownerFrames_{};
        std::size_t ownerFirst_ = 0, ownerCount_ = 0;
        void* lastOwnerAction_ = nullptr;
        int lastOwnerAnimation_ = 0;
        std::uint64_t lastOwnerAt_ = 0;
        static CreatureActionAnimationSelectionHook* active_;
        static thread_local std::uint64_t scopedSelectionToken_;
        static thread_local const native::AnimationPlaybackRequest* ownerRequest_;
        static thread_local int ownerBlendFrames_, ownerOptions_;

        HMODULE gameModule_ = nullptr;
        core::hooking::InlineHook submitHook_;
        SubmitRequestPointer original_ = nullptr;
        ValidateAnimationPointer validateAnimation_ = nullptr;
        mutable SRWLOCK selectionLock_ = SRWLOCK_INIT;
        std::array<Selection, SelectionCapacity> selections_ = {};
        std::atomic_uint64_t nextSelectionToken_{0};
        std::atomic_size_t pendingSelectionCount_{0};
        actions::CreatureActionLifecycleObserver* actionObserver_ = nullptr;
        core::Diagnostics diagnostics_ = {};
    };
}

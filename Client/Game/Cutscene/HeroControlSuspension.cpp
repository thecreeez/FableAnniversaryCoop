#include "HeroControlSuspension.h"

#include "Game/Entity/Native/ThingComponentAccess.h"

namespace
{
    // CTCScriptedControl fields: +0x0C safe pointer to the active control,
    // +0x20 control attached, +0x24 its priority.
    constexpr std::uintptr_t ControlPointerOffset = 0x0C;
    constexpr std::uintptr_t AttachedOffset = 0x20;
    constexpr std::uintptr_t PriorityOffset = 0x24;
    // Safe pointer get (thiscall, returns the raw object).
    constexpr std::uintptr_t SafePointerGetRva = 0x012E6EA0;
    // CTCScriptedControl::Attach(control, priority), ret 8.
    constexpr std::uintptr_t AttachRva = 0x01AAE530;
    // The retail control destructor's release: queue a release action, clear
    // the control, drop the queued scripted action.
    constexpr std::uintptr_t QueueActionRva = 0x01AAF4E0;
    constexpr std::uintptr_t ClearControlRva = 0x01AAE5D0;
    constexpr std::uintptr_t DropActionRva = 0x01AB0CB0;
    constexpr std::uintptr_t OperatorNewRva = 0x00001050;
    constexpr std::uintptr_t ActionBaseConstructorRva = 0x012B4FF0;
    constexpr std::uintptr_t ReleaseActionVtableRva = 0x02AE9E04;
    constexpr std::size_t ReleaseActionBytes = 0x0C;
    // Player object (HUD singleton): +0x2D8 control mode, 1 = player.
    constexpr std::uintptr_t PlayerSingletonRva = 0x0322B3CC;
    constexpr std::uintptr_t PlayerModeOffset = 0x2D8;
    constexpr std::int32_t PlayerInControl = 1;
    // SetPlayerInControl(bool): true sets mode 1, as at a sequence's end.
    constexpr std::uintptr_t SetPlayerInControlRva = 0x018E8720;

    void* FindScriptedControl(void* const heroThing) noexcept
    {
        using fable::game::entity::native::ThingComponentAccess;
        using fable::game::entity::native::ThingComponentType;
        return heroThing == nullptr ? nullptr
            : ThingComponentAccess::Find(heroThing, ThingComponentType::ScriptedControl);
    }
}

namespace fable::game::cutscene
{
    bool HeroControlSuspension::SuspendPlayerMode() noexcept
    {
        __try
        {
            auto* const player = *reinterpret_cast<std::uint8_t* const*>(base_ + PlayerSingletonRva);
            if (player == nullptr) return false;
            const std::int32_t mode = *reinterpret_cast<std::int32_t*>(player + PlayerModeOffset);
            // Observed in play: 0 while the player moves freely; only a scripted
            // lock (2, 3) is lifted.
            if (mode == 0 || mode == PlayerInControl) return false;
            savedMode_ = mode;
            modeSaved_ = true;
            using SetFn = void(__thiscall*)(void*, std::uint8_t);
            reinterpret_cast<SetFn>(base_ + SetPlayerInControlRva)(player, 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void HeroControlSuspension::RestorePlayerMode() noexcept
    {
        if (!modeSaved_) return;
        modeSaved_ = false;
        __try
        {
            auto* const player = *reinterpret_cast<std::uint8_t* const*>(base_ + PlayerSingletonRva);
            if (player != nullptr) *reinterpret_cast<std::int32_t*>(player + PlayerModeOffset) = savedMode_;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool HeroControlSuspension::Suspend(void* const heroThing) noexcept
    {
        if (base_ == 0 || IsSuspended()) return false;
        const bool modeChanged = SuspendPlayerMode();
        auto* const component = static_cast<std::uint8_t*>(FindScriptedControl(heroThing));
        if (component == nullptr) return modeChanged;
        using GetFn = void*(__thiscall*)(void*);
        using QueueFn = void(__thiscall*)(void*, void*);
        using VoidFn = void(__thiscall*)(void*);
        using NewFn = void*(__cdecl*)(std::size_t);
        __try
        {
            if (component[AttachedOffset] == 0) return modeChanged;
            void* const control = reinterpret_cast<GetFn>(base_ + SafePointerGetRva)(component + ControlPointerOffset);
            if (control == nullptr) return modeChanged;
            const std::int32_t priority = *reinterpret_cast<std::int32_t*>(component + PriorityOffset);
            auto* const action = static_cast<std::uint8_t*>(
                reinterpret_cast<NewFn>(base_ + OperatorNewRva)(ReleaseActionBytes));
            if (action == nullptr) return modeChanged;
            reinterpret_cast<VoidFn>(base_ + ActionBaseConstructorRva)(action);
            *reinterpret_cast<std::uint32_t*>(action + 4) = 0;
            *reinterpret_cast<std::uint16_t*>(action + 8) = 0;
            *reinterpret_cast<std::uintptr_t*>(action) = base_ + ReleaseActionVtableRva;
            reinterpret_cast<QueueFn>(base_ + QueueActionRva)(component, action);
            reinterpret_cast<VoidFn>(base_ + ClearControlRva)(component);
            reinterpret_cast<VoidFn>(base_ + DropActionRva)(component);
            control_ = control;
            priority_ = priority;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return modeChanged;
        }
    }

    bool HeroControlSuspension::Resume(void* const heroThing) noexcept
    {
        RestorePlayerMode();
        void* const control = control_;
        control_ = nullptr;
        if (control == nullptr || base_ == 0) return false;
        void* const component = FindScriptedControl(heroThing);
        if (component == nullptr) return false;
        using AttachFn = void(__thiscall*)(void*, void*, std::int32_t);
        __try
        {
            reinterpret_cast<AttachFn>(base_ + AttachRva)(component, control, priority_);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

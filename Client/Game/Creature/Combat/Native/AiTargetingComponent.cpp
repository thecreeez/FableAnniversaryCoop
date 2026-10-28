#include "Core/Hooking/CodePatch.h"
#include "Game/Creature/Control/ScriptControl.h"
#include <unordered_map>
#include "AiTargetingComponent.h"

#include <array>
#include <cstring>

namespace
{
    struct CombatBinding { void* target; std::uint64_t ownerUid, targetUid; unsigned reads = 0; };
    SRWLOCK combatTargetLock = SRWLOCK_INIT;
    std::unordered_map<void*, CombatBinding> combatTargets;
    fable::core::hooking::CodePatch selectedGetterPatch;
    using WeakGet = void*(__thiscall*)(void*);
    WeakGet weakGet = nullptr;

    std::uint64_t NativeUid(void* thing) noexcept
    {
        __try { return thing != nullptr ? *reinterpret_cast<std::uint64_t*>(static_cast<unsigned char*>(thing) + 0x14) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    void* ComponentOwner(void* component) noexcept
    {
        __try { return component != nullptr ? *reinterpret_cast<void**>(static_cast<unsigned char*>(component) + 4) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    void* AssignedTarget(void* component) noexcept
    {
        void* owner = ComponentOwner(component);
        if (owner == nullptr || fable::game::ScriptControl::HasNativeControl(owner)) return nullptr;
        void* result = nullptr;
        AcquireSRWLockExclusive(&combatTargetLock);
        const auto found = combatTargets.find(owner);
        if (found != combatTargets.end() && NativeUid(owner) == found->second.ownerUid &&
            NativeUid(found->second.target) == found->second.targetUid)
        {
            result = found->second.target;
            ++found->second.reads;
        }
        ReleaseSRWLockExclusive(&combatTargetLock);
        return result;
    }
    void* __fastcall ReadSelectedEnemy(void* component, void*)
    {
        if (void* selected = AssignedTarget(component)) return selected;
        return weakGet(static_cast<unsigned char*>(component) + 0x38);
    }
}

namespace fable::game::creature::combat::native
{
    bool AiTargetingComponent::BindCombatTarget(HMODULE module, void* creature, void* target) noexcept
    {
        if (module == nullptr || creature == nullptr || target == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        weakGet = reinterpret_cast<WeakGet>(base + 0x012E6EA0);
        // Leave CTCAIEnemy::CurrentEnemy (01AC61F0) untouched: SeekEnemy
        // compares it with vf31 and invokes the retail combat transition
        // 016DF890 when they differ. Overriding both hides that transition.
        if (!selectedGetterPatch.IsInstalled())
        {
            // CTCTargetingAI::vf31 is what SeekEnemy compares to current enemy.
            void* expected = reinterpret_cast<void*>(base + 0x01A9454F);
            void* replacement = reinterpret_cast<void*>(&ReadSelectedEnemy);
            if (!selectedGetterPatch.Install(reinterpret_cast<void*>(base + VtableRva + 31 * sizeof(void*)),
                &expected, sizeof(expected), &replacement, sizeof(replacement))) return false;
        }
        const auto ownerUid = NativeUid(creature), targetUid = NativeUid(target);
        if (ownerUid == 0 || targetUid == 0) return false;
        AcquireSRWLockExclusive(&combatTargetLock);
        bool bound = true;
        try
        {
            auto found = combatTargets.find(creature);
            if (found == combatTargets.end() || found->second.target != target || found->second.ownerUid != ownerUid)
                combatTargets[creature] = {target, ownerUid, targetUid, 0};
        }
        catch (...) { bound = false; }
        ReleaseSRWLockExclusive(&combatTargetLock);
        return bound;
    }

    void AiTargetingComponent::ClearCombatTarget(void* creature) noexcept
    {
        AcquireSRWLockExclusive(&combatTargetLock);
        combatTargets.erase(creature);
        ReleaseSRWLockExclusive(&combatTargetLock);
    }

    bool AiTargetingComponent::ConsumeCombatTargetRead(void* creature) noexcept
    {
        AcquireSRWLockExclusive(&combatTargetLock);
        const auto found = combatTargets.find(creature);
        const bool read = found != combatTargets.end() && found->second.reads != 0;
        if (found != combatTargets.end()) found->second.reads = 0;
        ReleaseSRWLockExclusive(&combatTargetLock);
        return read;
    }

    bool AiTargetingComponent::Validate(
        HMODULE gameModule,
        void* targetingComponent) noexcept
    {
        if (gameModule == nullptr || targetingComponent == nullptr)
        {
            return false;
        }

        constexpr std::array<std::uint8_t, 3> getterPrefix = {
            0x83, 0xC1, 0x25,
        };
        constexpr std::array<std::uint8_t, 3> setterPrefix = {
            0x6A, 0x08, 0xB8,
        };
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        bool valid = false;
        __try
        {
            auto** const vtable = *reinterpret_cast<void***>(
                targetingComponent);
            valid = vtable == reinterpret_cast<void**>(base + VtableRva) &&
                vtable[GetScriptTargetOverrideSlot] ==
                    reinterpret_cast<void*>(
                        base + GetScriptTargetOverrideRva) &&
                vtable[SetScriptTargetOverrideSlot] ==
                    reinterpret_cast<void*>(
                        base + SetScriptTargetOverrideRva) &&
                std::memcmp(
                    vtable[GetScriptTargetOverrideSlot],
                    getterPrefix.data(),
                    getterPrefix.size()) == 0 &&
                std::memcmp(
                    vtable[SetScriptTargetOverrideSlot],
                    setterPrefix.data(),
                    setterPrefix.size()) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            valid = false;
        }
        return valid;
    }

    void* AiTargetingComponent::GetScriptTargetOverride(
        HMODULE gameModule,
        void* targetingComponent) noexcept
    {
        if (!Validate(gameModule, targetingComponent))
        {
            return nullptr;
        }

        void* target = nullptr;
        __try
        {
            auto** const vtable = *reinterpret_cast<void***>(
                targetingComponent);
            target = reinterpret_cast<GetTargetPointer>(
                vtable[GetScriptTargetOverrideSlot])(targetingComponent);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            target = nullptr;
        }
        return target;
    }

    bool AiTargetingComponent::SetScriptTargetOverride(
        HMODULE gameModule,
        void* targetingComponent,
        void* target) noexcept
    {
        if (!Validate(gameModule, targetingComponent))
        {
            return false;
        }

        bool invoked = false;
        __try
        {
            auto** const vtable = *reinterpret_cast<void***>(
                targetingComponent);
            reinterpret_cast<SetTargetPointer>(
                vtable[SetScriptTargetOverrideSlot])(
                    targetingComponent, target);
            invoked = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            invoked = false;
        }
        return invoked &&
            GetScriptTargetOverride(gameModule, targetingComponent) == target;
    }
}

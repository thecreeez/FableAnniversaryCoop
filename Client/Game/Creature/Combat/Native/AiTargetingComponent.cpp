#include "Core/Hooking/CodePatch.h"
#include "Game/Creature/Control/ScriptControl.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
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

    bool ValidateEnemyMutator(HMODULE module, std::uintptr_t rva,
        std::uint32_t handler) noexcept
    {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(base + rva);
        __try
        {
            // push -1; push SEH handler; mov eax, fs:[0].
            return bytes[0] == 0x6A && bytes[1] == 0xFF && bytes[2] == 0x68 &&
                *reinterpret_cast<const std::uint32_t*>(bytes + 3) == base + handler &&
                bytes[7] == 0x64 && bytes[8] == 0xA1 &&
                *reinterpret_cast<const std::uint32_t*>(bytes + 9) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool FindExplicitEnemy(HMODULE module, void* component,
        std::uint64_t uid, void*& target) noexcept
    {
        target = nullptr;
        const auto get = reinterpret_cast<WeakGet>(
            reinterpret_cast<std::uintptr_t>(module) + 0x012E6EA0);
        __try
        {
            // CTCEnemy+10 owns a sentinel list of intelligent pointers.
            // Retail01987690 compares WeakGet(node+8) for each list node.
            auto* head = *reinterpret_cast<unsigned char**>(
                static_cast<unsigned char*>(component) + 0x10);
            if (head == nullptr) return false;
            auto* node = *reinterpret_cast<unsigned char**>(head);
            for (std::size_t count = 0; node != head; ++count)
            {
                if (node == nullptr || count >= 8192) return false;
                void* value = get(node + 8);
                if (value != nullptr && *reinterpret_cast<const std::uint64_t*>(
                        static_cast<unsigned char*>(value) + 0x14) == uid)
                {
                    target = value;
                    return true;
                }
                node = *reinterpret_cast<unsigned char**>(node);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

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
    bool AiTargetingComponent::EnsureAttackEnemy(
        HMODULE module, void* creature, void* attacker, bool& added,
        std::uint64_t& attackerUid) noexcept
    {
        added = false;
        attackerUid = 0;
        if (module == nullptr || creature == nullptr || attacker == nullptr ||
            ScriptControl::HasNativeControl(creature) ||
            !ValidateEnemyMutator(module, AddEnemyRva, 0x0252DA21) ||
            !ValidateEnemyMutator(module, RemoveEnemyRva, 0x0252DA50))
            return false;
        bool enemy = false;
        if (!ReadEnemyRelation(module, creature, attacker, enemy)) return false;
        if (enemy) return true;
        void* component = entity::native::ThingComponentAccess::Find(
            creature, entity::native::ThingComponentType::Enemy);
        if (component == nullptr) return false;
        void* existing = nullptr;
        const auto uid = NativeUid(attacker);
        if (uid == 0 || !FindExplicitEnemy(module, component, uid, existing))
            return false;
        // Preserve pre-existing explicit entries, including native vetoes
        // such as FriendsWithEverything or a scripted ally relationship.
        if (existing != nullptr) return false;
        attackerUid = uid;
        using Mutator = void(__thiscall*)(void*, void*);
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        added = true; // Retain cleanup ownership even if a native call faults.
        __try
        {
            reinterpret_cast<Mutator>(base + AddEnemyRva)(component, attacker);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!FindExplicitEnemy(module, component, uid, existing) || existing != attacker)
            return false;
        if (ReadEnemyRelation(module, creature, attacker, enemy) && enemy)
            return true;
        // Explicit allies/global friendliness still win. Undo only our entry.
        if (ReleaseAttackEnemy(module, creature, uid)) added = false;
        return false;
    }

    bool AiTargetingComponent::ReleaseAttackEnemy(
        HMODULE module, void* creature, std::uint64_t attackerUid) noexcept
    {
        if (module == nullptr || creature == nullptr || attackerUid == 0 ||
            !ValidateEnemyMutator(module, RemoveEnemyRva, 0x0252DA50)) return false;
        void* component = entity::native::ThingComponentAccess::Find(
            creature, entity::native::ThingComponentType::Enemy);
        if (component == nullptr) return true;
        void* attacker = nullptr;
        if (!FindExplicitEnemy(module, component, attackerUid, attacker)) return false;
        if (attacker == nullptr) return true; // Expired native weak reference.
        using Mutator = void(__thiscall*)(void*, void*);
        __try
        {
            reinterpret_cast<Mutator>(reinterpret_cast<std::uintptr_t>(module) +
                RemoveEnemyRva)(component, attacker);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool AiTargetingComponent::ReadEnemyRelation(
        HMODULE module, void* creature, void* target, bool& enemy) noexcept
    {
        enemy = false;
        if (module == nullptr || creature == nullptr || target == nullptr)
            return false;
        // RVA01988480: cdecl(CThing*, CThing*), verified stack arguments
        // and plain RET. Refuse a different retail executable signature.
        constexpr std::array<std::uint8_t, 10> prefix = {
            0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x1C};
        const auto address = reinterpret_cast<std::uintptr_t>(module) +
            EnemyRelationRva;
        __try
        {
            if (std::memcmp(reinterpret_cast<const void*>(address),
                    prefix.data(), prefix.size()) != 0)
                return false;
            using Predicate = bool(__cdecl*)(void*, void*);
            enemy = reinterpret_cast<Predicate>(address)(creature, target);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

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

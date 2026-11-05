#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace fable::game::creature::combat::native
{
    // Native CTCTargetingAI access for AI-controlled remote Hero pawns. Its
    // script-target override is deliberately separate from CTCTargetingHero's
    // selected/candidate target state.
    struct AiTargetingComponent final
    {
        using GetTargetPointer = void* (__thiscall*)(void*);
        using SetTargetPointer = void (__thiscall*)(void*, void*);

        static constexpr std::uintptr_t VtableRva = 0x02B10904;
        static constexpr std::size_t GetScriptTargetOverrideSlot = 35;
        static constexpr std::size_t SetScriptTargetOverrideSlot = 36;
        static constexpr std::uintptr_t GetScriptTargetOverrideRva =
            0x01A94505;
        static constexpr std::uintptr_t SetScriptTargetOverrideRva =
            0x01A9450D;

        // Host-selected combat targets, consumed at the native AI getters,
        // independently from CTCTargetingAI's script override.
        static bool BindCombatTarget(HMODULE module, void* creature, void* target) noexcept;
        static void ClearCombatTarget(void* creature) noexcept;
        static bool ConsumeCombatTargetRead(void* creature) noexcept;
        // Native pairwise enemy predicate, used by targeting and OnHit.
        // This observes the retail decision without adding enemy relations.
        static constexpr std::uintptr_t EnemyRelationRva = 0x01988480;
        [[nodiscard]] static bool ReadEnemyRelation(
            HMODULE module, void* creature, void* target, bool& enemy) noexcept;
        // Retail CTCEnemy add/remove pair, the same pair used by script
        // slots616/617. Only remove an entry when Ensure reports ownership.
        static constexpr std::uintptr_t AddEnemyRva = 0x01988130;
        static constexpr std::uintptr_t RemoveEnemyRva = 0x019882D0;
        [[nodiscard]] static bool EnsureAttackEnemy(
            HMODULE module, void* creature, void* attacker, bool& added,
            std::uint64_t& attackerUid) noexcept;
        [[nodiscard]] static bool ReleaseAttackEnemy(
            HMODULE module, void* creature, std::uint64_t attackerUid) noexcept;
        [[nodiscard]] static bool Validate(
            HMODULE gameModule,
            void* targetingComponent) noexcept;
        [[nodiscard]] static void* GetScriptTargetOverride(
            HMODULE gameModule,
            void* targetingComponent) noexcept;
        [[nodiscard]] static bool SetScriptTargetOverride(
            HMODULE gameModule,
            void* targetingComponent,
            void* target) noexcept;
    };
}

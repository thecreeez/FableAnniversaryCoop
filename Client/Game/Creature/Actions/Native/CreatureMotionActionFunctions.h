#pragma once

#include <Windows.h>

#include <cstdint>

namespace fable::game::creature::actions::native
{
    // Retail Hero defensive moves, rebuilt on a remote Hero presentation from
    // what the owner's game submitted. Both are ordinary creature actions the
    // player-controlled brain constructs on the stack and hands to
    // CThingCreature::AddAction (0x01B42F70).
    struct CreatureMotionActionFunctions final
    {
        // CCreatureAction_StartBlocking(creature): enters the native block.
        static constexpr std::uintptr_t StartBlockingConstructorRva = 0x017AA9D0;
        static constexpr std::uintptr_t StartBlockingVtableRva = 0x02AC5F3C;
        // CCombatAction_ControlledStrafeJump(creature, target, dir3*, stick2*):
        // the roll/dodge. The constructor keeps the planar direction at
        // +0x12C and the stick input at +0x138.
        static constexpr std::uintptr_t StrafeJumpConstructorRva = 0x017C0160;
        static constexpr std::uintptr_t StrafeJumpVtableRva = 0x02ACAAA4;
        static constexpr std::uintptr_t StrafeJumpDirectionOffset = 0x12C;
        static constexpr std::uintptr_t StrafeJumpStickOffset = 0x138;

        // CCreatureAction_UseObject(creature, target, 1): an NPC walking up to
        // and using a bench, chair or similar object (sitting included).
        static constexpr std::uintptr_t UseObjectConstructorRva = 0x017F83D0;

        static bool SubmitStartBlocking(HMODULE gameModule, void* creature) noexcept;
        static bool SubmitUseObject(HMODULE gameModule, void* creature, void* target) noexcept;
        static bool SubmitStrafeJump(
            HMODULE gameModule,
            void* creature,
            const float (&direction)[3],
            const float (&stick)[2]) noexcept;
        // Reads the roll parameters from an accepted local action.
        static bool ReadStrafeJump(
            HMODULE gameModule,
            void* action,
            float (&direction)[3],
            float (&stick)[2]) noexcept;
    };
}

#include "CreatureMotionActionFunctions.h"
#include "Game/Entity/Native/ThingComponentAccess.h"

#include "Game/Creature/Actions/Hooks/CreatureActionLifecycleObserver.h"
#include "Game/Creature/Actions/Native/CreatureActionFunctions.h"

#include <cmath>
#include <cstring>

namespace
{
    // Both constructors open with push -1; push <SEH handler>.
    constexpr std::uint8_t SehPrologue[] = {0x6A, 0xFF, 0x68};
    // Room for either action object; StrafeJump writes up to +0x148.
    constexpr std::size_t ActionStorageSize = 0x400;

    using StartBlockingConstructor = void*(__thiscall*)(void* action, void* creature);
    using StrafeJumpConstructor = void*(__thiscall*)(
        void* action, void* creature, void* target, const float* direction, const float* stick);
    using UseObjectConstructor = void*(__thiscall*)(void* action, void* creature, void* target, std::uint8_t flag);
    using SubmitFn = bool(__thiscall*)(void* creature, void* action);
    using DestroyFn = void*(__thiscall*)(void* action);

    bool HasSehPrologue(const std::uintptr_t address) noexcept
    {
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(address), SehPrologue, sizeof(SehPrologue)) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ResolveSubmit(const HMODULE gameModule, SubmitFn& submit) noexcept
    {
        using fable::game::creature::actions::native::CreatureActionFunctions;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        auto* const address = reinterpret_cast<std::uint8_t*>(base + CreatureActionFunctions::SubmitAddressRva);
        bool hooked = false;
        __try
        {
            hooked = address[0] == 0xE9;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!hooked)
        {
            std::uint8_t* pristine = nullptr;
            if (!CreatureActionFunctions::ResolveSubmit(gameModule, pristine) || pristine != address) return false;
        }
        submit = reinterpret_cast<SubmitFn>(address);
        return true;
    }

    // Constructs with `construct`, submits as an authoritative replay, then
    // destroys the stack copy (the creature keeps its own).
    template <typename Construct>
    bool SubmitConstructed(const HMODULE gameModule, void* const creature, Construct construct,
        const bool authoritativeReplay = true) noexcept
    {
        using fable::game::creature::actions::CreatureActionLifecycleObserver;
        using fable::game::creature::actions::native::CreatureActionFunctions;
        SubmitFn submit = nullptr;
        if (!ResolveSubmit(gameModule, submit)) return false;
        const auto destroy = reinterpret_cast<DestroyFn>(
            reinterpret_cast<std::uintptr_t>(gameModule) + CreatureActionFunctions::ActionDestructorAddressRva);
        alignas(16) unsigned char action[ActionStorageSize] = {};
        bool constructed = false;
        bool submitted = false;
        bool receiptArmed = false;
        if (authoritativeReplay) CreatureActionLifecycleObserver::BeginAuthoritativeReplay();
        __try
        {
            construct(action);
            constructed = true;
            receiptArmed = CreatureActionLifecycleObserver::BeginSubmissionReceipt(creature);
            submitted = receiptArmed && submit(creature, action);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            submitted = false;
        }
        if (authoritativeReplay) CreatureActionLifecycleObserver::EndAuthoritativeReplay();
        bool accepted = false;
        const bool observed = receiptArmed && CreatureActionLifecycleObserver::EndSubmissionReceipt(creature, accepted);
        if (constructed)
        {
            __try
            {
                destroy(action);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return submitted && observed && accepted;
    }
}

namespace fable::game::creature::actions::native
{
    bool CreatureMotionActionFunctions::ReadBlocking(void* creature, bool& blocking) noexcept
    {
        void* combat = entity::native::ThingComponentAccess::Find(creature,
            static_cast<entity::native::ThingComponentType>(0x30));
        if (combat == nullptr) return false;
        __try { blocking = *(static_cast<const unsigned char*>(combat) + 0x6F) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    bool CreatureMotionActionFunctions::SetBlocking(HMODULE gameModule, void* creature, bool blocking) noexcept
    {
        void* combat = entity::native::ThingComponentAccess::Find(creature,
            static_cast<entity::native::ThingComponentType>(0x30));
        if (gameModule == nullptr || combat == nullptr) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(gameModule) +
            (blocking ? BeginBlockingRva : EndBlockingRva);
        const unsigned char expected[] = {0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0xF9};
        __try
        {
            if (std::memcmp(reinterpret_cast<void*>(address), expected, sizeof(expected)) != 0) return false;
            reinterpret_cast<void(__thiscall*)(void*)>(address)(combat);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    bool CreatureMotionActionFunctions::SubmitStartBlocking(
        const HMODULE gameModule, void* const creature, const bool authoritativeReplay) noexcept
    {
        if (gameModule == nullptr || creature == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        if (!HasSehPrologue(base + StartBlockingConstructorRva)) return false;
        const auto construct = reinterpret_cast<StartBlockingConstructor>(base + StartBlockingConstructorRva);
        return SubmitConstructed(gameModule, creature, [&](void* action) { construct(action, creature); },
            authoritativeReplay);
    }

    bool CreatureMotionActionFunctions::SubmitUseObject(
        const HMODULE gameModule, void* const creature, void* const target) noexcept
    {
        if (gameModule == nullptr || creature == nullptr || target == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        if (!HasSehPrologue(base + UseObjectConstructorRva)) return false;
        const auto construct = reinterpret_cast<UseObjectConstructor>(base + UseObjectConstructorRva);
        return SubmitConstructed(gameModule, creature, [&](void* action) { construct(action, creature, target, 1); });
    }

    bool CreatureMotionActionFunctions::SubmitStrafeJump(
        const HMODULE gameModule,
        void* const creature,
        const float (&direction)[3],
        const float (&stick)[2],
        const bool authoritativeReplay) noexcept
    {
        if (gameModule == nullptr || creature == nullptr) return false;
        for (const float value : direction)
        {
            if (!std::isfinite(value)) return false;
        }
        for (const float value : stick)
        {
            if (!std::isfinite(value)) return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        if (!HasSehPrologue(base + StrafeJumpConstructorRva)) return false;
        const auto construct = reinterpret_cast<StrafeJumpConstructor>(base + StrafeJumpConstructorRva);
        return SubmitConstructed(gameModule, creature,
            [&](void* action) { construct(action, creature, nullptr, direction, stick); },
            authoritativeReplay);
    }

    bool CreatureMotionActionFunctions::ReadStrafeJump(
        const HMODULE gameModule,
        void* const action,
        float (&direction)[3],
        float (&stick)[2]) noexcept
    {
        if (gameModule == nullptr || action == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        __try
        {
            if (*static_cast<std::uintptr_t*>(action) != base + StrafeJumpVtableRva) return false;
            const auto* const fields = static_cast<const std::uint8_t*>(action);
            std::memcpy(direction, fields + StrafeJumpDirectionOffset, sizeof(direction));
            std::memcpy(stick, fields + StrafeJumpStickOffset, sizeof(stick));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        for (const float value : direction)
        {
            if (!std::isfinite(value)) return false;
        }
        for (const float value : stick)
        {
            if (!std::isfinite(value)) return false;
        }
        return true;
    }
}

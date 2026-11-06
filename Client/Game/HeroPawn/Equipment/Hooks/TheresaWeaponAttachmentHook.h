#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Native/ScriptTypes.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace fable::game::hero_pawn::equipment::hooks
{
    // Visual-only prototype. The query hook is active only inside a verified
    // Theresa weapon attachment resolve, never general combat bone queries.
    class TheresaWeaponAttachmentHook final
    {
    public:
        struct Basis final { float up[3] = {}; float forward[3] = {}; };
        static_assert(sizeof(Basis) == 24);
        using ResolveTransform = bool(__thiscall*)(void*, float*, Basis*, bool, float, bool);
        bool Install(HMODULE module, const core::Diagnostics& diagnostics) noexcept;
        bool Shutdown() noexcept;
        bool ResolveAttachment(ResolveTransform original, void* attachment,
            float* position, Basis* basis, bool interpolate, float alpha,
            bool solid, bool& adapted, std::uintptr_t caller);

    private:
        using BoneQuery = bool(__thiscall*)(void*, const game::native::CharString*,
            int, std::uint32_t, float*, Basis*, float, bool);
        static bool __fastcall QueryWeaponBone(void* component, void*,
            const game::native::CharString* name, int occurrence, std::uint32_t flags,
            float* position, Basis* basis, float alpha, bool solid);
        static TheresaWeaponAttachmentHook* active_;
        HMODULE module_ = nullptr;
        BoneQuery original_ = nullptr;
        core::Diagnostics diagnostics_;
        core::hooking::InlineHook patch_;
        std::atomic_uint diagnosticCount_{0};
    };
}

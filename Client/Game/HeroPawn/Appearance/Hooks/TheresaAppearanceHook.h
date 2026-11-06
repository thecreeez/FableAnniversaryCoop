#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "TheresaHeroSkin.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace fable::game::hero_pawn::appearance::hooks
{
    // Guests retain Hero's driver with a render-only Sister follower mesh and
    // native equipped-clothing projection, without Hero body morph replay.
    // The native player, inventory, quest identity and collision stay Hero-owned.
    class TheresaAppearanceHook final
    {
    public:
        bool Install(HMODULE module, bool guest, const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        static void RegisterRemoteDefinition(void* definition) noexcept;
        [[nodiscard]] static bool UsesTheresaPresentation(void* thing) noexcept;

    private:
        using SetGraphic = void(__thiscall*)(void*, int, unsigned int);
        using RebuildModifiers = void(__thiscall*)(void*);
        static void __fastcall SetPlayerGraphic(void* thing, void*, int type, unsigned int graphic);
        static void __fastcall RebuildAppearance(void* component, void*);
        static void __fastcall RebuildBodyParts(void* component, void*);
        void ProjectSelectedClothing(void* component, RebuildModifiers original, const char* stage);

        static TheresaAppearanceHook* active_;
        static std::atomic<void*> remoteDefinition_;
        static std::atomic<void*> localThing_;
        HMODULE module_ = nullptr;
        core::Diagnostics diagnostics_;
        core::hooking::InlineHook graphicPatch_;
        core::hooking::InlineHook modifierPatch_;
        core::hooking::InlineHook bodyPartPatch_;
        TheresaHeroSkin skin_;
        SetGraphic originalGraphic_ = nullptr;
        RebuildModifiers originalModifiers_ = nullptr;
        RebuildModifiers originalBodyParts_ = nullptr;
        bool guest_ = false;
        unsigned int clothingDiagnosticCount_ = 0;
    };
}

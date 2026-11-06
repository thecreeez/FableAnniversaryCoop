#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace fable::game::hero_pawn::appearance::hooks
{
    // Render-only Sister mesh follows the unchanged Hero animation component.
    // The native pawn owns the additional mesh and its destruction/streaming.
    class TheresaHeroSkin final
    {
    public:
        bool Install(HMODULE module, const core::Diagnostics& diagnostics) noexcept;
        bool Shutdown() noexcept;
        static void Update(void* thing, bool childhood=false) noexcept;
        static void Forget(void* thing) noexcept;
        static void SetCutsceneHidden(bool hidden) noexcept;
        // Read-only acceptance probe; never repairs or submits a mesh.
        static bool InspectForTest(void* thing, bool hidden) noexcept;

    private:
        struct Entry final
        {
            std::atomic<void*> thing{nullptr};
            std::atomic<void*> graphic{nullptr};
            std::atomic<void*> master{nullptr};
            std::atomic<void*> skin{nullptr};
            std::atomic_bool queued{false};
            std::atomic_bool childhood{false};
            std::atomic_uint64_t visualKey{0};
        };
        using AddMesh = void(__cdecl*)(void*);
        using BuildMap = void(__thiscall*)(void*);
        static void __cdecl AddSkin(void* command);
        static void __fastcall BuildSkinMap(void* mesh, void*);
        static void* __cdecl LoadSkinObject(void* type, void* outer, const wchar_t* name,
            const wchar_t* file, unsigned int flags, void* sandbox, int reconcile);
        static bool __cdecl CheckSkinStreaming(void* graphic, void* command,
            std::size_t size, void* callback, char replace);
        static void* __cdecl ResolveSkinPawn(void* pawn);
        static void __cdecl RefreshVisual(void* command);
        bool FixMap(void* mesh) noexcept;
        Entry* FindGraphic(void* graphic) noexcept;

        static TheresaHeroSkin* active_;
        HMODULE module_ = nullptr;
        core::Diagnostics diagnostics_;
        core::hooking::InlineHook addPatch_;
        core::hooking::InlineHook mapPatch_;
        AddMesh originalAdd_ = nullptr;
        BuildMap originalMap_ = nullptr;
        std::array<Entry, 8> entries_;
        std::atomic_uint32_t reports_{0};
    };
}

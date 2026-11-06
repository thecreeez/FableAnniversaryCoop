#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/HeroPawn/Appearance/Native/HeroMorphComponent.h"
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
        static void InvalidateVisual(void* thing) noexcept;
        static void OnWorldReady() noexcept;
        // Read-only acceptance probe; never repairs or submits a mesh.
        static bool InspectForTest(void* thing, bool hidden) noexcept;

    private:
        static void UpdateImpl(void* thing,bool childhood,void* graphicOverride) noexcept;
        static void __fastcall TickInventory(void* inventory,void*,unsigned int tick);
        static void __fastcall CloseInventory(void* inventory,void*);
        static void __fastcall ReleaseInventory(void* inventory,void*);
        static void RefreshInventory(void* inventory) noexcept;
        static void ForgetInventory(void* inventory) noexcept;
        struct Entry final
        {
            std::atomic<void*> thing{nullptr};
            std::atomic<void*> graphic{nullptr};
            std::atomic<void*> master{nullptr};
            std::atomic<void*> skin{nullptr};
            std::atomic<void*> limbs{nullptr};
            std::atomic_bool limbsQueued{false};
            std::atomic<void*> arms{nullptr};
            std::atomic_bool armsQueued{false};
            std::atomic_bool queued{false};
            std::atomic_bool childhood{false};
            std::atomic_uint64_t visualKey{0};
            std::atomic_uint outfitMask{0};
            std::atomic_uint appliedOutfitMask{~0u};
            native::TheresaVisualShapeState shape;
            std::uint64_t nextShapeAt = 0;
            std::uint64_t nextVisualAt = 0;
            std::uint64_t outfitRefreshAt = 0;
            unsigned int outfitRefreshAttempts = 0;
            std::array<void*,128> visibleBeforeScene{};
            unsigned int visibleBeforeSceneCount=0;
            bool sceneWasHidden=false;
        };
        using AddMesh = void(__cdecl*)(void*);
        using BuildMap = void(__thiscall*)(void*);
        using SetHidden = void(__thiscall*)(void*,unsigned int);
        using ShowSection = void(__thiscall*)(void*,int,int,int);
        static void __cdecl AddSkin(void* command);
        static void __fastcall BuildSkinMap(void* mesh, void*);
        static void* __cdecl LoadSkinObject(void* type, void* outer, const wchar_t* name,
            const wchar_t* file, unsigned int flags, void* sandbox, int reconcile);
        static bool __cdecl CheckSkinStreaming(void* graphic, void* command,
            std::size_t size, void* callback, char replace);
        static void* __cdecl ResolveSkinPawn(void* pawn);
        static void __cdecl RefreshVisual(void* command);
        static void __fastcall SetPreviewMesh(void* actor,void*,void* meshName,int option);
        static void __fastcall DestroyPreview(void* actor,void*);
        static void __fastcall SetGenericPreviewMesh(void* actor,void*,void* meshName,int skeletal,int option);
        static void __fastcall DestroyGenericPreview(void* actor,void*);
        static void BeforePreviewMesh(void* actor);
        static void AfterPreviewMesh(void* actor);
        void RefreshPreviews(Entry& entry) noexcept;
        void RefreshPreview(Entry& entry,void* preview) noexcept;
        static void __fastcall GuardHidden(void* mesh,void*,unsigned int hidden);
        static void __fastcall GuardSection(void* mesh,void*,int section,int show,int lod);
        bool IsDriver(void* mesh) noexcept;
        bool FixMap(void* mesh, bool bareArms=false) noexcept;
        Entry* FindGraphic(void* graphic) noexcept;

        static TheresaHeroSkin* active_;
        HMODULE module_ = nullptr;
        core::Diagnostics diagnostics_;
        core::hooking::InlineHook addPatch_;
        core::hooking::InlineHook mapPatch_;
        core::hooking::InlineHook hiddenPatch_;
        core::hooking::InlineHook sectionPatch_;
        AddMesh originalAdd_ = nullptr;
        BuildMap originalMap_ = nullptr;
        SetHidden originalHidden_ = nullptr;
        ShowSection originalSection_ = nullptr;
        std::atomic_uint32_t hiddenReports_{0};
        std::array<Entry, 8> entries_;
        std::atomic_uint32_t reports_{0};
    };
}

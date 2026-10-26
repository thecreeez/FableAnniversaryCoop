#include "QuestMapCameraGuard.h"

#include "Core/Hooking/CodePatch.h"

#include <array>
#include <cstdint>

namespace
{
    // CTCInventoryQuests slot 32, right after cameraManager->Current():
    // mov edx,[eax]; mov ecx,eax; mov eax,[edx+8]; call eax
    constexpr std::uintptr_t GuardRva = 0x01A2B951;
    // mov [esi+1C8h], eax
    constexpr std::uintptr_t ResumeRva = 0x01A2B95A;
    constexpr std::array<std::uint8_t, 9> Signature = {
        0x8B, 0x10, 0x8B, 0xC8, 0x8B, 0x42, 0x08, 0xFF, 0xD0};

    std::uintptr_t gResume = 0;
    fable::core::hooking::CodePatch gPatch;

#if defined(_M_IX86)
    __declspec(naked) void QuestMapCameraGuardStub()
    {
        __asm
        {
            test eax, eax
            jz missing_camera
            mov edx, dword ptr [eax]
            mov ecx, eax
            mov eax, dword ptr [edx + 8]
            call eax
            jmp dword ptr [gResume]
        missing_camera:
            or eax, -1
            jmp dword ptr [gResume]
        }
    }
#endif
}

namespace fable::game::native
{
    bool InstallQuestMapCameraGuard(const HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        (void)diagnostics;
        return false;
#else
        if (gameModule == nullptr) return false;
        if (gPatch.IsInstalled()) return true;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        gResume = base + ResumeRva;
        const bool installed = gPatch.InstallRelativeJump(
            reinterpret_cast<void*>(base + GuardRva),
            Signature.data(),
            Signature.size(),
            reinterpret_cast<void*>(&QuestMapCameraGuardStub),
            Signature.size());
        diagnostics.Event(installed ? "QuestMapCameraGuardReady" : "QuestMapCameraGuardBlocked",
            installed ? "quest screen tolerates a missing current camera" : "quest screen bytes did not match");
        return installed;
#endif
    }

    void ShutdownQuestMapCameraGuard() noexcept
    {
        (void)gPatch.Shutdown();
    }
}

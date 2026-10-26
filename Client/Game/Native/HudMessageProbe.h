#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Native/NativePromptBox.h"

#include <Windows.h>

#include <cstdint>

namespace fable::game::native
{
    // Research aid, enabled by ALBIONTOGETHER_TRACE_SCRIPT_INTERFACE=1:
    // logs every message queued on the HUD message manager (G+0x18) with its
    // text, style and callers, to find which native dialog a game action uses.
    class HudMessageProbe final
    {
    public:
        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread, focused game window: Insert shows the native prompt box,
        // Home retitles it with the next count, End closes it.
        void Process() noexcept;

    private:
        using QueueFn = std::uintptr_t(__thiscall*)(void* self, void* text, void* a2, void* a3,
            void* a4, void* a5, std::uint32_t style, float duration, std::uint32_t a8, std::uint32_t a9);

        static std::uintptr_t __fastcall Queued(void* self, void* edx, void* text, void* a2, void* a3,
            void* a4, void* a5, std::uint32_t style, float duration, std::uint32_t a8, std::uint32_t a9);

        void QueueTest(int key) noexcept;
        void ReportInputStack() noexcept;

        static HudMessageProbe* active_;
        NativePromptBox prompt_;
        std::uint32_t count_ = 0;
        std::uint64_t nextInputReportAt_ = 0;
        char lastInputStack_[400] = {};
        bool keyDown_[3] = {};
        NativePromptBox::State reportedState_ = NativePromptBox::State::Hidden;
        core::hooking::InlineHook queue_;
        QueueFn original_ = nullptr;
        std::uintptr_t base_ = 0;
        core::Diagnostics diagnostics_ = {};
    };
}

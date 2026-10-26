#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fable::game::native
{
    // Research aid, enabled by ALBIONTOGETHER_TRACE_SCRIPT_INTERFACE=1:
    // patches every CGameScriptInterface vtable slot with a counting thunk
    // so a manual session shows which retail script functions a game action
    // (a dialog, a menu, a cutscene) used. Call counts are reported as
    // deltas from the game thread.
    class ScriptInterfaceTracer final
    {
    public:
        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        // Game thread: reports slots called since the last report.
        void Process() noexcept;

    private:
        static constexpr std::size_t MaximumSlots = 1024;
        static constexpr std::size_t ThunkBytes = 32;
        static constexpr std::uint64_t ReportIntervalMilliseconds = 2'000;

        static void __cdecl Count(std::uint32_t slot) noexcept;

        static std::array<std::atomic_uint32_t, MaximumSlots> counts_;
        std::array<std::uint32_t, MaximumSlots> reported_ = {};
        std::array<void*, MaximumSlots> originals_ = {};
        void** vtable_ = nullptr;
        std::size_t slotCount_ = 0;
        std::uint8_t* thunks_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::uint64_t nextReportAt_ = 0;
    };
}

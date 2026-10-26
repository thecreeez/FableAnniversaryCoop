#pragma once

#include <Windows.h>

#include <cstdint>

namespace fable::game::cutscene
{
    // Lends the Hero back to the player while a quest script that already
    // took scripted control of it (StartScriptingEntity) is held: the control
    // is detached from the Hero's CTCScriptedControl the way retail releases
    // it, and attached again, with its priority, before the script resumes.
    // The script keeps its own handle to the control object throughout.
    // The player object's control mode (+0x2D8; 1 = player in control) is
    // switched back to 1 the way retail ends a scripted sequence, and the
    // saved mode is written back on resume.
    class HeroControlSuspension final
    {
    public:
        void Bind(HMODULE gameModule) noexcept { base_ = reinterpret_cast<std::uintptr_t>(gameModule); }

        // heroThing is the Hero's native Thing. False when no script holds it.
        bool Suspend(void* heroThing) noexcept;
        bool Resume(void* heroThing) noexcept;
        [[nodiscard]] bool IsSuspended() const noexcept { return control_ != nullptr || modeSaved_; }

    private:
        std::uintptr_t base_ = 0;
        bool SuspendPlayerMode() noexcept;
        void RestorePlayerMode() noexcept;

        void* control_ = nullptr;
        std::int32_t priority_ = 0;
        bool modeSaved_ = false;
        std::int32_t savedMode_ = 1;
    };
}

#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <Windows.h>

namespace fable::game::native
{
    // Opening the quest screen (CTCInventoryQuests, vtable slot 32) asks the
    // player's camera manager for its current camera and calls a virtual on
    // it without a null check. On a multiplayer guest that camera list can
    // be empty, so the game crashed when the guest opened the quest map in
    // the Heroes' Guild. The guard skips the call and keeps the camera id at
    // -1, the value the function stores just before.
    bool InstallQuestMapCameraGuard(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
    void ShutdownQuestMapCameraGuard() noexcept;
    // How often the guard skipped a missing camera (acceptance evidence).
    [[nodiscard]] long QuestMapCameraGuardHits() noexcept;
}

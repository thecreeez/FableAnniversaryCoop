#pragma once

#include "Core/Diagnostics/Diagnostics.h"

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace fable::multiplayer::persistence
{
    // Observes and replays the retail quest card screen. Quest scripts call
    // CGameScriptInterface slot 325 (questName, mode, startQuest) when they
    // present a quest start/completion card; the card's own completion field
    // (QuestStartScreened) is then synchronized, so a peer's script never
    // shows it. Mode is replayed; startQuest (quest-manager side effects) is
    // left to quest event sync. Patches only that vtable entry.
    class QuestScreenHook final
    {
    public:
        struct Observation final
        {
            std::string name;
            bool mode = false;
            bool closed = false;
        };

        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        bool PopObservation(Observation& observation);
        void Poll(void* localHero);
        static bool ConfirmLocalCard(void* localHero) noexcept;
        bool Close(void* localHero, const char* questName) noexcept;
        // Game thread; shows the card for questName on this peer.
        bool Replay(void* scriptInterface, const char* questName, bool mode) noexcept;

    private:
        static constexpr std::size_t Slot = 325;
        static constexpr std::size_t ObservationLimit = 16;
        using ShowFn = void(__thiscall*)(void* self, void* name, std::uint8_t mode, std::uint8_t startQuest);

        static void __fastcall Shown(void* self, void* edx, void* name, std::uint8_t mode, std::uint8_t startQuest);

        static QuestScreenHook* active_;
        static thread_local bool replaying_;
        HMODULE gameModule_ = nullptr;
        void** entry_ = nullptr;
        ShowFn original_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::mutex mutex_;
        std::deque<Observation> observations_;
        void* observedCard_ = nullptr;
        std::string observedName_;
        bool observedMode_ = false;
        bool wasOpen_ = false;
        bool forcedClose_ = false;
    };
}

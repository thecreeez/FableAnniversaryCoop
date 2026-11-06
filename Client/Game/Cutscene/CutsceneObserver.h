#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace fable::game::cutscene
{
    // Observes retail cutscenes: every scripted cutscene is started by name
    // (CS_GUILD_ARRIVE, CS_WASPBOSS_INTRO, ...) through one cdecl entry used
    // by ~350 quest-script call sites, and the HUD keeps a per-player
    // "in cutscene" byte. This class only reports both; it never changes
    // what the game does.
    class CutsceneObserver final
    {
    public:
        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
        // The retail CGameScriptInterface used to replay cutscenes.
        void BindScriptInterface(void* scriptInterface) noexcept { scriptInterface_ = scriptInterface; }
        [[nodiscard]] void* const* ScriptInterfaceSlot() const noexcept { return &scriptInterface_; }
        void Shutdown() noexcept;

        // Game thread: samples the in-cutscene flag and reports transitions,
        // with where the local Hero stood (for driving bots to the trigger).
        void Process(const char* mapName, float x, float y, float z) noexcept;

        [[nodiscard]] bool IsInCutscene() const noexcept { return inCutscene_; }
        static bool ReadNativeState(HMODULE module, bool& value) noexcept;

        // Called on the quest-script thread before a retail cutscene starts;
        // it may block (holding only that script) and returns once the
        // cutscene may play. Not called for Play().
        using Gate = bool(__cdecl*)(void* context, const char* name);
        void SetGate(Gate gate, void* context) noexcept;

        // Plays a cutscene by name with the retail script sequence (movie
        // resource, cutscene mode on, start, mode off). Blocks for the whole
        // cutscene, so call it from a worker thread, never the game thread.
        bool Play(const char* name) noexcept;

        // Quest-script thread only: yields this script for one game frame,
        // the way retail scripts wait. Scripts run in lockstep with the game
        // thread, so a script must never block in a plain sleep.
        bool YieldScriptFrame() noexcept;
        // Script thread: retail cutscene mode (interface slot 397). Quest
        // scripts enter it just before the start call, which freezes the Hero.
        bool SetCutsceneMode(bool enabled) noexcept;
        // Script thread: the retail scripted sequence (interface slot 388
        // begins it: HUD hidden, player input off, interface +0x2C set; the
        // guard it leaves in the movie resource ends it, 0x0189E940, when the
        // script destroys that resource after the cutscene). Ending lends
        // control back; beginning again uses a mod-owned movie resource that
        // is deliberately never destroyed, so the script's own guard is the
        // only one that ends the sequence after the cutscene.
        bool EndScriptedSequence() noexcept;
        bool BeginScriptedSequence() noexcept;
        bool PopSkip(std::string& name);
        bool RequestSkip(const char* name, bool broadcast = false) noexcept;

    private:
        using StartFn = std::uintptr_t(__cdecl*)(
            void* name, void* a2, std::uint32_t a3, std::uint32_t a4,
            std::uint32_t a5, std::uint32_t a6);

        static std::uintptr_t __cdecl StartObserved(
            void* name, void* a2, std::uint32_t a3, std::uint32_t a4,
            std::uint32_t a5, std::uint32_t a6);
        bool ReadInCutscene(bool& value) const noexcept;
        using InputFn = void(__thiscall*)(void*, void*, void*);
        static void __fastcall InputObserved(void* self, void*, void* input, void* messages);
        void SetActiveName(const char* name) noexcept;
        core::hooking::InlineHook skipInput_;
        InputFn originalInput_ = nullptr;
        SRWLOCK skipLock_ = SRWLOCK_INIT;
        char activeName_[96] = {}, localSkip_[96] = {};
        bool skipRequested_ = false, broadcastSkip_ = false;

        static CutsceneObserver* active_;
        // Set on a thread while Play() drives the retail start, so the gate
        // and the start report are skipped for replayed cutscenes.
        static thread_local bool replaying_;
        std::atomic<Gate> gate_{nullptr};
        std::atomic<void*> gateContext_{nullptr};

        HMODULE gameModule_ = nullptr;
        void* scriptInterface_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        core::hooking::InlineHook start_;
        StartFn originalStart_ = nullptr;
        std::atomic_uint32_t startCount_{0};
        bool inCutscene_ = false;
        bool sampled_ = false;
        std::uint64_t enteredAt_ = 0;
    };
}

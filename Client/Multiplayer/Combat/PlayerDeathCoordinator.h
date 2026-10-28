#include "Core/Hooking/CodePatch.h"
#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Game/Creature/Combat/CombatHealthMutationEvent.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace fable::game
{
    class QuestService;
}

namespace fable::game::creature::combat
{
    class CreatureCombatService;
}

namespace fable::multiplayer::replication
{
    class LocalHeroReplication;
}

namespace fable::multiplayer::combat
{
    class PlayerCombatantDirectory;
    // Defers local Hero death handling out of the native health hook. Fable's
    // own mutation runs first, so resurrection phials retain their retail
    // consumption and revival behavior. A still-terminal Hero is restored
    // through the retail player-death entry, without speculative healing.
    class PlayerDeathCoordinator final
    {
    public:
        bool Initialize(
            game::creature::combat::CreatureCombatService& combat,
            game::QuestService& quests,
            PlayerCombatantDirectory& combatants,
            const core::Diagnostics& diagnostics) noexcept;
        bool Process(
            const replication::LocalHeroReplication& localHero) noexcept;
        void Shutdown() noexcept;
        bool IsReady() const noexcept { return initialized_ && originalDeath_ != nullptr; }
        bool DamageForAcceptance(void* hero, float damage) noexcept;

    private:
        using NativeDeathFn = int(__thiscall*)(void*);
        static int __fastcall NativeDeath(void* creature, void* edx);
        static void __stdcall ResurrectionTutorial(int index, int parameter);
        static PlayerDeathCoordinator* active_;
        core::hooking::InlineHook nativeDeathHook_;
        core::hooking::CodePatch resurrectionTutorialCall_;
        NativeDeathFn originalDeath_ = nullptr;
        void* localCreature_ = nullptr;
        struct PendingDeath final
        {
            void* creature = nullptr;
            std::uint64_t thingUid = 0;
            std::uint64_t observedAt = 0;
            std::uint64_t nextAttemptAt = 0;
            unsigned int attempts = 0;
            bool reportedDeferred = false;
        };

        static constexpr std::size_t EventCapacity = 64;
        static constexpr float TerminalHealth = 0.01f;

        static void CaptureMutation(
            void* context,
            const game::creature::combat::CombatHealthMutationEvent& event)
            noexcept;
        void Enqueue(
            const game::creature::combat::CombatHealthMutationEvent& event)
            noexcept;

        game::creature::combat::CreatureCombatService* combat_ = nullptr;
        game::QuestService* quests_ = nullptr;
        PlayerCombatantDirectory* combatants_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        std::mutex eventMutex_;
        std::deque<game::creature::combat::CombatHealthMutationEvent> events_;
        PendingDeath pending_;
        std::atomic_bool acceptingEvents_{false};
        std::atomic_uint droppedEvents_{0};
        bool initialized_ = false;
    };
}

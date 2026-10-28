#include <cstring>
#include "PlayerDeathCoordinator.h"
#include "PlayerDeathPolicy.h"
#include "PlayerCombatantDirectory.h"

#include "Game/Creature/Combat/CreatureCombatService.h"
#include "Game/Quest/QuestService.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"

#include <Windows.h>

#include <cstdio>

namespace fable::multiplayer::combat
{
    PlayerDeathCoordinator* PlayerDeathCoordinator::active_ = nullptr;

    void __stdcall PlayerDeathCoordinator::ResurrectionTutorial(int index, int parameter)
    {
        (void)index;
        (void)parameter;
        if (active_ != nullptr)
            active_->diagnostics_.Event("MultiplayerResurrectionTutorialSuppressed",
                "recovery finished; retail tutorial49 bypassed before modal UI creation");
    }

    int __fastcall PlayerDeathCoordinator::NativeDeath(void* creature, void*)
    {
        auto* self = active_;
        if (self == nullptr || self->originalDeath_ == nullptr) return 0;
        // PlayerCreature::vf16 opens a process-global death UI/mode40 when
        // no local inventory flask exists. A remote Hero is only a proxy;
        // its owning client handles inventory and recovery. Never run that
        // global UI path on an observer after a replicated terminal hit.
        if (creature != self->localCreature_ && self->combatants_ != nullptr &&
            self->combatants_->FindActor(creature) != 0)
        {
            self->diagnostics_.Event("MultiplayerRemotePlayerDeathUiSuppressed",
                "remote Hero death remains owner-authoritative; no observer death UI or inventory consumption");
            return 0;
        }
        float health = 0, maximum = 0;
        if (creature == self->localCreature_ && self->combat_ != nullptr &&
            self->combat_->ReadCombatHealth(creature, health, maximum) && health > TerminalHealth)
        {
            // A queued terminal action may finish after native recovery.
            // It must not consume a second flask or open a stale death card.
            return 0;
        }
        if (creature == self->localCreature_ && self->combat_ != nullptr &&
            self->combat_->ConsumeNativeResurrectionFlask(creature))
        {
            self->diagnostics_.Event("MultiplayerResurrectionFlaskConsumedNoPause",
                "one native flask consumed; native recovery action; no quest-exit/death card or world pause");
            return 0;
        }
        return self->originalDeath_(creature);
    }

    bool PlayerDeathCoordinator::Initialize(
        game::creature::combat::CreatureCombatService& combat,
        game::QuestService& quests,
        PlayerCombatantDirectory& combatants,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        combat_ = &combat;
        quests_ = &quests;
        combatants_ = &combatants;
        diagnostics_ = diagnostics;
        acceptingEvents_.store(true, std::memory_order_release);
        if (!combat_->AddHealthMutationSink(
                &PlayerDeathCoordinator::CaptureMutation, this))
        {
            acceptingEvents_.store(false, std::memory_order_release);
            combat_ = nullptr;
            quests_ = nullptr;
            diagnostics_ = {};
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        unsigned char expected[7] = {0x6A, 0xFF, 0x68};
        const auto handler = base + 0x02555E0B;
        std::memcpy(expected + 3, &handler, sizeof(handler));
        active_ = this;
        if (!nativeDeathHook_.Install(reinterpret_cast<void*>(base + 0x01B5EAA0), expected, sizeof(expected),
            reinterpret_cast<void*>(&NativeDeath), sizeof(expected)))
        {
            active_ = nullptr;
            diagnostics_.Event("MultiplayerNativeDeathHookBlocked", "player-death prologue did not match");
            Shutdown();
            return false;
        }
        originalDeath_ = reinterpret_cast<NativeDeathFn>(nativeDeathHook_.Original());
        // RecoverFromDeath::vf4 calls the tutorial/profile dispatcher(49,0),
        // separately from the death card. Keep the base vf4 cleanup (mode31,
        // orientation and weapon state); replace only this call, ret8 ABI.
        constexpr std::uintptr_t callRva = 0x017AF2C3;
        unsigned char expectedCall[5] = {0xE8}, replacementCall[5] = {0xE8};
        const auto originalDisplacement = static_cast<std::int32_t>(0x019AA450 - (callRva + 5));
        const auto replacementDisplacement = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(&ResurrectionTutorial) - (base + callRva + 5));
        std::memcpy(expectedCall + 1, &originalDisplacement, sizeof(originalDisplacement));
        std::memcpy(replacementCall + 1, &replacementDisplacement, sizeof(replacementDisplacement));
        if (!resurrectionTutorialCall_.Install(reinterpret_cast<void*>(base + callRva),
            expectedCall, sizeof(expectedCall), replacementCall, sizeof(replacementCall)))
        {
            diagnostics_.Event("MultiplayerResurrectionTutorialHookBlocked", "recovery tutorial call did not match");
            Shutdown();
            return false;
        }
        diagnostics_.Event("MultiplayerResurrectionTutorialHookReady", "checked recovery tutorial49 call replaced");
        initialized_ = true;
        diagnostics_.Event(
            "MultiplayerPlayerDeathReady",
            "native resurrection phials and death card remain authoritative; no speculative teleport or healing");
        return true;
    }

    bool PlayerDeathCoordinator::Process(
        const replication::LocalHeroReplication& localHero) noexcept
    {
        if (!initialized_ || combat_ == nullptr || quests_ == nullptr)
        {
            return false;
        }

        std::deque<game::creature::combat::CombatHealthMutationEvent> captured;
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            captured.swap(events_);
        }
        void* const localCreature = localHero.NativeHero();
        localCreature_ = localCreature;
        for (const auto& event : captured)
        {
            if (localCreature == nullptr || event.creature != localCreature)
            {
                continue;
            }
            const PlayerDeathOutcome outcome = ClassifyPlayerDeath(
                event.currentHealth, event.maximumHealth);
            if (outcome == PlayerDeathOutcome::Alive)
            {
                pending_ = {};
                continue;
            }
            if (outcome == PlayerDeathOutcome::GuildRespawnRequired &&
                pending_.creature != event.creature)
            {
                pending_.creature = event.creature;
                pending_.thingUid = event.thingUid;
                pending_.observedAt = event.observedAt;
                pending_.nextAttemptAt = event.observedAt + 1500;
            }
        }
        if (pending_.creature == nullptr)
        {
            return true;
        }
        if (pending_.creature != localCreature)
        {
            pending_ = {};
            return true;
        }

        float currentHealth = 0.0f;
        float maximumHealth = 0.0f;
        if (!combat_->ReadCombatHealth(
                localCreature, currentHealth, maximumHealth))
        {
            return true;
        }
        if (currentHealth > TerminalHealth)
        {
            diagnostics_.Event(
                "MultiplayerPlayerNativeResurrectionObserved",
                "local Hero recovered through native death/phial processing");
            pending_ = {};
            return true;
        }

        const std::uint64_t now = GetTickCount64();
        if (now < pending_.nextAttemptAt)
        {
            return true;
        }
        // Give the existing native death action time to consume its phial.
        // One fallback per death, through PlayerCreature::vf16, never a
        // cancellable quest teleport followed by speculative full healing.
        if (pending_.attempts != 0) return true;
        if (originalDeath_ != nullptr)
        {
            ++pending_.attempts;
            NativeDeath(localCreature, nullptr);
            diagnostics_.Event("MultiplayerPlayerNativeDeathSubmitted",
                "retail phial/death-card path submitted once; health left to native recovery");
        }
        else
        {
            pending_.nextAttemptAt = now + 1000;
            if (!pending_.reportedDeferred)
            {
                diagnostics_.Event("MultiplayerPlayerNativeDeathDeferred", "native PlayerCreature death entry unavailable; no healing or teleport applied");
                pending_.reportedDeferred = true;
            }
        }
        return true;
    }

    bool PlayerDeathCoordinator::DamageForAcceptance(void* hero, float damage) noexcept
    {
        return combat_ != nullptr && hero == localCreature_ && combat_->ApplyOwnedCombatDamage(hero, damage);
    }

    void PlayerDeathCoordinator::Shutdown() noexcept
    {
        if (!resurrectionTutorialCall_.Shutdown()) return;
        if (!nativeDeathHook_.Shutdown()) return;
        if (active_ == this) active_ = nullptr;
        originalDeath_ = nullptr;
        localCreature_ = nullptr;
        acceptingEvents_.store(false, std::memory_order_release);
        if (combat_ != nullptr)
        {
            combat_->RemoveHealthMutationSink(
                &PlayerDeathCoordinator::CaptureMutation, this);
        }
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            events_.clear();
        }
        pending_ = {};
        droppedEvents_.store(0, std::memory_order_release);
        combat_ = nullptr;
        quests_ = nullptr;
        combatants_ = nullptr;
        diagnostics_ = {};
        initialized_ = false;
    }

    void PlayerDeathCoordinator::CaptureMutation(
        void* context,
        const game::creature::combat::CombatHealthMutationEvent& event)
        noexcept
    {
        if (context != nullptr)
        {
            static_cast<PlayerDeathCoordinator*>(context)->Enqueue(event);
        }
    }

    void PlayerDeathCoordinator::Enqueue(
        const game::creature::combat::CombatHealthMutationEvent& event)
        noexcept
    {
        if (!acceptingEvents_.load(std::memory_order_acquire))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (events_.size() >= EventCapacity)
        {
            droppedEvents_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        events_.push_back(event);
    }
}

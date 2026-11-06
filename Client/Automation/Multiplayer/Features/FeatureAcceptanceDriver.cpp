#include "Game/HeroPawn/Equipment/Native/HeroWeaponComponent.h"
#include "FeatureAcceptanceDriver.h"

#include "Game/Creature/Actions/Native/CreatureMotionActionFunctions.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/Native/ScriptTypes.h"
#include "Game/Native/Addresses.h"
#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

#include <cstdio>
#include <cmath>
#include "Game/HeroPawn/Appearance/Native/HeroMorphComponent.h"

namespace
{
    constexpr std::int32_t HeroExperienceComponent = 0x68;
    constexpr std::uintptr_t GetStatRva = 0x019B8CC0;
    constexpr std::uintptr_t SetStatRva = 0x019B8FE0;
    constexpr std::size_t QuestCardSlot = 325;
    // Any quest the fixture knows; the card is only presented.
    constexpr const char* QuestCardName = "Q_Arena";

    bool WakeCombatStance(void* hero) noexcept
    {
        using namespace fable::game::entity::native;
        void* combat = ThingComponentAccess::Find(hero, static_cast<ThingComponentType>(0x30));
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto address = base + 0x01958B40;
        if (combat == nullptr) return false;
        __try
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(address);
            if (bytes[0] != 0xA1 || *reinterpret_cast<const std::uintptr_t*>(bytes + 1) != base + 0x0322C75C ||
                bytes[5] != 0x89 || bytes[6] != 0x41 || bytes[7] != 0x1C) return false;
            // Retail Combat::Wake timer, ret4; called by GameObject::vf71.
            reinterpret_cast<void(__thiscall*)(void*, void*)>(address)(combat, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return fable::game::hero_pawn::equipment::native::HeroWeaponComponent::ReadCombatStance(hero);
    }

    bool AddStrength(void* const experience, const std::int32_t amount) noexcept
    {
        using GetFn = std::int32_t(__thiscall*)(void*, std::int32_t);
        using SetFn = void(__thiscall*)(void*, std::int32_t, std::int32_t);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            const std::int32_t current = reinterpret_cast<GetFn>(base + GetStatRva)(experience, 0);
            reinterpret_cast<SetFn>(base + SetStatRva)(experience, 0, current + amount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Calls the (hooked) vtable entry, exactly as a quest script would.
    bool ShowQuestCard(void* const scriptInterface, const char* const quest) noexcept
    {
        using ConstructFn = void(__thiscall*)(fable::game::native::CharString*, const char*, int);
        using DestroyFn = void(__thiscall*)(fable::game::native::CharString*);
        using ShowFn = void(__thiscall*)(void*, void*, std::uint8_t, std::uint8_t);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        fable::game::native::CharString name = {};
        bool built = false;
        bool shown = false;
        __try
        {
            reinterpret_cast<ConstructFn>(base + fable::game::native::rva::CharStringConstructor)(&name, quest, -1);
            built = true;
            void* const* const vtable = *reinterpret_cast<void* const* const*>(scriptInterface);
            reinterpret_cast<ShowFn>(vtable[QuestCardSlot])(scriptInterface, &name, 0, 0);
            shown = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            shown = false;
        }
        if (built)
        {
            __try
            {
                reinterpret_cast<DestroyFn>(base + fable::game::native::rva::CharStringDestructor)(&name);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return shown;
    }
}

namespace fable::automation::multiplayer::features
{
    void FeatureAcceptanceDriver::Initialize(
        const bool enabled,
        const bool host,
        ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        enabled_ = enabled;
        if (!enabled_) return;
        host_ = host;
        sharedWorld_ = GetEnvironmentVariableW(L"ALBIONTOGETHER_SHARED_WORLD_TEST", nullptr, 0) != 0;
        multiplayer_ = &multiplayer;
        diagnostics_ = diagnostics;
        diagnostics_.Event("FeatureAcceptanceArmed", host_ ? "role=host" : "role=guest");
    }

    void FeatureAcceptanceDriver::Shutdown() noexcept
    {
        enabled_ = false;
        sharedWorld_ = false; sharedStep_ = 0; sharedStarted_ = sharedNext_ = 0;
        multiplayer_ = nullptr;
        phase_.store(Phase::WaitingForWorld, std::memory_order_release);
    }

    void FeatureAcceptanceDriver::Enter(const Phase phase, const std::uint64_t now) noexcept
    {
        phase_.store(phase, std::memory_order_release);
        phaseStartedAt_ = now;
    }

    void FeatureAcceptanceDriver::Fail(const char* const reason) noexcept
    {
        diagnostics_.Event("FeatureAcceptanceFailed", reason);
        phase_.store(Phase::Failed, std::memory_order_release);
    }

    void FeatureAcceptanceDriver::Tick(const bool remotePresentationReady) noexcept
    {
        if (!enabled_ || (!host_ && !sharedWorld_) || multiplayer_ == nullptr ||
            phase_.load(std::memory_order_acquire) != Phase::WaitingForWorld)
        {
            return;
        }
        const auto& contexts = multiplayer_->Contexts();
        if (remotePresentationReady && contexts.players.localHero.IsWorldReady() &&
            contexts.players.localHero.CurrentState() != nullptr &&
            contexts.players.remotePlayers.ActiveCount() != 0)
        {
            Enter(Phase::Settle, GetTickCount64());
        }
    }

    bool FeatureAcceptanceDriver::ProcessGameThreadIdle() noexcept
    {
        if (GetEnvironmentVariableW(L"ALBIONTOGETHER_THERESA_TEST", nullptr, 0)) return false;
        if (enabled_ && sharedWorld_) return ProcessSharedWorld();
        const Phase phase = phase_.load(std::memory_order_acquire);
        if (!enabled_ || !host_ || multiplayer_ == nullptr || phase == Phase::WaitingForWorld ||
            phase == Phase::Done || phase == Phase::Failed)
        {
            return false;
        }
        const std::uint64_t now = GetTickCount64();
        const std::uint64_t wait = phase == Phase::Settle ? SettleMilliseconds : StepMilliseconds;
        if (now - phaseStartedAt_ < wait) return false;

        auto& contexts = multiplayer_->Contexts();
        void* const hero = contexts.players.localHero.NativeHero();
        if (hero == nullptr)
        {
            Fail("local-hero-unavailable");
            return false;
        }
        const HMODULE module = GetModuleHandleW(nullptr);
        using game::creature::actions::native::CreatureMotionActionFunctions;
        char detail[128] = {};
        switch (phase)
        {
        case Phase::Settle:
            Enter(Phase::Roll, now);
            return false;
        case Phase::Roll:
        {
            const float direction[3] = {1.0f, 0.0f, 0.0f};
            const float stick[2] = {1.0f, 0.0f};
            const bool rolled = CreatureMotionActionFunctions::SubmitStrafeJump(module, hero, direction, stick, false);
            std::snprintf(detail, sizeof(detail), "accepted=%d", rolled ? 1 : 0);
            diagnostics_.Event("FeatureHostRolled", detail);
            Enter(Phase::Block, now);
            return true;
        }
        case Phase::Block:
        {
            const bool blocked = CreatureMotionActionFunctions::SubmitStartBlocking(module, hero, false);
            std::snprintf(detail, sizeof(detail), "accepted=%d", blocked ? 1 : 0);
            diagnostics_.Event("FeatureHostBlocked", detail);
            Enter(Phase::CombatStance, now);
            return true;
        }
        case Phase::CombatStance:
            if (!WakeCombatStance(hero)) { Fail("native-combat-stance-not-active"); return false; }
            diagnostics_.Event("FeatureHostCombatStance", "native combat idle active");
            Enter(Phase::StatExperience, now);
            return true;
        case Phase::StatExperience:
        {
            void* const experience = game::entity::native::ThingComponentAccess::Find(
                hero, static_cast<game::entity::native::ThingComponentType>(HeroExperienceComponent));
            const bool added = experience != nullptr && AddStrength(experience, ExperienceAmount);
            std::snprintf(detail, sizeof(detail), "strength=%d added=%d", ExperienceAmount, added ? 1 : 0);
            diagnostics_.Event("FeatureHostStatExperience", detail);
            Enter(Phase::SmallExperience, now);
            return true;
        }
        case Phase::SmallExperience:
        {
            void* experience = game::entity::native::ThingComponentAccess::Find(hero,
                static_cast<game::entity::native::ThingComponentType>(HeroExperienceComponent));
            if (experience == nullptr || !AddStrength(experience, 1)) { Fail("small-experience-award-failed"); return false; }
            ++smallAwards_;
            Enter(smallAwards_ >= 4 ? Phase::KillExperience : Phase::SmallExperience, now);
            return true;
        }
        case Phase::KillExperience:
            contexts.actions.killExperience.InjectKillForTest(ExperienceAmount);
            std::snprintf(detail, sizeof(detail), "general=%d", ExperienceAmount);
            diagnostics_.Event("FeatureHostKillExperience", detail);
            Enter(Phase::QuestCard, now);
            return true;
        case Phase::QuestCard:
        {
            void* const scriptInterface = *contexts.world.cutscenes.ScriptInterfaceSlot();
            const bool shown = scriptInterface != nullptr && ShowQuestCard(scriptInterface, QuestCardName);
            std::snprintf(detail, sizeof(detail), "quest=%s shown=%d", QuestCardName, shown ? 1 : 0);
            diagnostics_.Event("FeatureHostQuestCard", detail);
            Enter(Phase::Done, now);
            diagnostics_.Event("FeatureAcceptanceHostDone", "all host steps issued");
            return true;
        }
        default:
            return false;
        }
    }
    bool FeatureAcceptanceDriver::ProcessSharedWorld() noexcept
    {
        if (!multiplayer_ || phase_.load() == Phase::Failed || sharedStep_ == 9) return false;
        auto& c = multiplayer_->Contexts();
        const auto now = GetTickCount64();
        if (!sharedStarted_) sharedStarted_ = now;
        if (now - sharedStarted_ > 180000) { Fail("shared-world-timeout"); return false; }
        if (now < sharedNext_) return false;
        auto* hero = static_cast<unsigned char*>(c.players.localHero.NativeHero());
        if (!hero || !c.world.checkpoints.HasCheckpointForTest() || !c.players.localHero.IsWorldReady()) return false;
        if (sharedStep_ == 0)
        {
            if (c.players.remotePlayers.ActiveCount() == 0) return false;
            for (const auto& peer : c.transport.remotePlayerChannels.Snapshots())
            {
                void* remote = c.players.playerCombatants.FindCreature(peer.state.actorId);
                game::hero_pawn::appearance::native::HeroMorphResolutionState state;
                if (!remote || !game::hero_pawn::appearance::native::HeroMorphComponent::InspectResolution(remote, state) || !state.skeletalMeshComponent) return false;
                auto* mesh = static_cast<unsigned char*>(state.skeletalMeshComponent);
                if (*reinterpret_cast<float*>(mesh + 0x10C) != 0 || *reinterpret_cast<float*>(mesh + 0x110) != 0 || (mesh[0x128] & 2))
                { Fail("remote-hero-still-distance-culled"); return false; }
            }
            diagnostics_.Event("SharedWorldRangeVerified", "actual remote mesh has no primitive/volume distance culling");
            if (!host_ && !c.world.worldClock.RequestSleepForTest(0.625f)) return false;
            sharedStep_ = 1; sharedNext_ = now + 3000; return true;
        }
        if (sharedStep_ == 1)
        {
            ::fable::multiplayer::protocol::WorldClockMessage clock;
            if (!c.world.worldClock.ReadForTest(clock) || std::fabs(clock.fraction - 0.625f) > 0.02f) return false;
            diagnostics_.Event("SharedWorldGuestSleepVerified", "guest request changed actual native clock on this peer; bed UI itself not driven");
            if (!host_)
            {
                bool smashed = false;
                for (const auto& record : c.entities.entityPresence.LiveEntities().Snapshot())
                {
                    if (record.mapId != c.players.localHero.MapId() || record.creature) continue;
                    void* smash = game::entity::native::ThingComponentAccess::Find(record.thing,
                        static_cast<game::entity::native::ThingComponentType>(0x4E));
                    if (!smash) continue;
                    auto** table = *reinterpret_cast<void***>(smash);
                    reinterpret_cast<void(__thiscall*)(void*)>(table[30])(smash);
                    smashed = true; diagnostics_.Event("SharedWorldGuestSmashIssued", "retail smash on existing map object"); break;
                }
                if (!smashed) { Fail("fixture-has-no-native-smashable"); return false; }
            }
            sharedStep_ = 2; return true;
        }
        if (sharedStep_ == 2)
        {
            if (c.actions.playerDeath.ConsumeFlaskForAcceptance(hero)) { sharedNext_ = now + 3500; return true; }
            sharedStep_ = 3;
            sharedNext_ = now + (host_ ? 20000 : 3500); return true;
        }
        if (sharedStep_ == 3)
        {
            if (!c.actions.playerDeath.DamageForAcceptance(hero, *reinterpret_cast<float*>(hero + 0xCC) * 3))
            { Fail("terminal-native-damage-rejected"); return false; }
            sharedStep_ = 4; sharedNext_ = now + 3000; return true;
        }
        if (sharedStep_ == 4)
        {
            if (!c.actions.playerDeath.IsTerminalDead()) return false;
            auto* card = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(hero,
                static_cast<game::entity::native::ThingComponentType>(0xFC)));
            const bool deathCard = card && card[0xD] && *reinterpret_cast<int*>(card + 0x24) == 3;
            if (deathCard != host_) { Fail("wrong-peer-native-death-card"); return false; }
            if (!host_ && *reinterpret_cast<float*>(hero + 0xD0) > 0.01f) { Fail("dead-guest-was-healed"); return false; }
            diagnostics_.Event("SharedWorldTerminalDeathVerified", host_ ? "host retail death card" : "guest remains dead; no native loading/death card");
            if (host_ && !c.world.checkpoints.RequestReloadForTest()) { Fail("native-checkpoint-selector-rejected"); return false; }
            sharedStep_ = 5; return true;
        }
        if (sharedStep_ == 5 && c.world.checkpoints.LoadedAttemptForTest() && !c.world.checkpoints.IsBarrierActive())
        {
            // Native retirement continues after the world-ready callback.
            // The reported guest crash was ~1s after checkpoint completion.
            sharedStep_ = 6;
            sharedNext_ = now + 5000;
            return true;
        }
        if (sharedStep_ == 6 && !c.world.checkpoints.IsBarrierActive())
        {
            if (c.actions.playerDeath.IsTerminalDead()) { Fail("death-state-survived-checkpoint-load"); return false; }
            const float health = *reinterpret_cast<float*>(hero + 0xD0);
            if (health <= 0 || std::fabs(health - c.world.checkpoints.SavedHealthForTest()) > 1)
            { Fail("local-checkpoint-hero-health-not-restored"); return false; }
            if (!c.players.remotePlayers.ActiveCount()) return false;
            diagnostics_.Event("SharedWorldCheckpointVerified", "paired native checkpoint restored this player's saved HP and rebuilt remote Hero");
            sharedStep_ = 9; return true;
        }
        return false;
    }

}

#include "Game/HeroPawn/Equipment/Native/HeroWeaponComponent.h"
#include "FeatureAcceptanceDriver.h"

#include "Game/Creature/Actions/Native/CreatureMotionActionFunctions.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/Native/ScriptTypes.h"
#include "Game/Native/Addresses.h"
#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

#include <cstdio>

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
        multiplayer_ = &multiplayer;
        diagnostics_ = diagnostics;
        diagnostics_.Event("FeatureAcceptanceArmed", host_ ? "role=host" : "role=guest");
    }

    void FeatureAcceptanceDriver::Shutdown() noexcept
    {
        enabled_ = false;
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
        if (!enabled_ || !host_ || multiplayer_ == nullptr ||
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
}

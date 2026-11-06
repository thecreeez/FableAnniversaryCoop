#include "AutomationRunner.h"

#include "Automation/LocalInstance/MapTransitionAcceptanceDriver.h"
#include "Automation/LocalInstance/ChildhoodAcceptanceDriver.h"
#include "Automation/Multiplayer/Abilities/HeroWillAbilityAcceptanceDriver.h"
#include "Automation/Multiplayer/Combat/CombatTargetAcceptanceDriver.h"
#include "Automation/Multiplayer/Combat/CombatVisualExchangeDriver.h"
#include "Automation/Multiplayer/Persistence/SaveAcceptanceDriver.h"
#include "Automation/Multiplayer/Quest/QuestSyncAcceptanceDriver.h"
#include "Automation/Multiplayer/Features/FeatureAcceptanceDriver.h"
#include "Automation/Multiplayer/Features/TheresaAcceptanceDriver.h"
#include "Automation/Multiplayer/Features/CutsceneAcceptanceDriver.h"
#include "Automation/Multiplayer/Transition/NpcTransferAcceptanceDriver.h"
#include "Automation/Multiplayer/Transition/MapStressAcceptanceDriver.h"
#include "Automation/Multiplayer/Transition/OwnerScopedPresentationAcceptanceDriver.h"
#include "Automation/Runtime/RuntimeConfiguration.h"
#include "Game/Runtime/GameServiceRuntime.h"
#include "Multiplayer/Runtime/MultiplayerSession.h"

#include <memory>

namespace fable::automation::runtime
{
    class AutomationRunner::State final
    {
    public:
        ::fable::automation::local_instance::MapTransitionAcceptanceDriver transition;
        ::fable::automation::local_instance::ChildhoodAcceptanceDriver childhood;
        ::fable::automation::multiplayer::combat::CombatTargetAcceptanceDriver combatTarget;
        ::fable::automation::multiplayer::combat::CombatVisualExchangeDriver combatVisual;
        ::fable::automation::multiplayer::abilities::HeroWillAbilityAcceptanceDriver heroWill;
        ::fable::automation::multiplayer::transition::NpcTransferAcceptanceDriver npcTransfer;
        ::fable::automation::multiplayer::transition::MapStressAcceptanceDriver mapStress;
        ::fable::automation::multiplayer::transition::OwnerScopedPresentationAcceptanceDriver ownerPresentation;
        ::fable::automation::multiplayer::persistence::SaveAcceptanceDriver save;
        ::fable::automation::multiplayer::quest::QuestSyncAcceptanceDriver questSync;
        ::fable::automation::multiplayer::features::FeatureAcceptanceDriver features;
        ::fable::automation::multiplayer::features::TheresaAcceptanceDriver theresa;
        ::fable::automation::multiplayer::features::CutsceneAcceptanceDriver cutscene;
        ::fable::multiplayer::MultiplayerSession* multiplayer = nullptr;
        ::fable::core::Diagnostics diagnostics = {};
        bool heroWillFocused = false;
        bool transitionScenario = false;
        bool transitionConvergenceReady = false;
    };

    AutomationRunner::AutomationRunner()
        : state_(std::make_unique<State>())
    {
    }

    AutomationRunner::~AutomationRunner()
    {
        Shutdown();
    }

    bool AutomationRunner::Initialize(
        const RuntimeConfiguration& configuration,
        ::fable::game::GameServiceRuntime& services,
        ::fable::multiplayer::MultiplayerSession& multiplayer,
        const ::fable::core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        state_->multiplayer = &multiplayer;
        state_->diagnostics = diagnostics;
        state_->childhood.Initialize(
            configuration.ScenarioIs(L"bootstrap_fixture_probe") &&
                GetEnvironmentVariableW(L"ALBIONTOGETHER_CHILDHOOD_TEST", nullptr, 0)
                ? &services : nullptr, diagnostics);

        const bool transition =
            configuration.ScenarioIs(L"multiplayer_host_transition") ||
            configuration.ScenarioIs(L"multiplayer_guest_transition");
        state_->transitionScenario = transition;
        state_->transitionConvergenceReady = false;

        state_->transition.Initialize(
            configuration.ScenarioIs(L"multiplayer_host_transition") ||
                configuration.ScenarioIs(L"multiplayer_host_authority") ||
                configuration.ScenarioIs(L"multiplayer_guest_transition"),
            configuration.ScenarioIs(L"multiplayer_host_authority"),
            services.Entities(),
            multiplayer,
            diagnostics);

        const bool hostCombat = configuration.ScenarioIs(L"multiplayer_host_combat");
        const bool guestCombat = configuration.ScenarioIs(L"multiplayer_guest_combat");
        const bool hostWill = configuration.ScenarioIs(L"multiplayer_host_hero_will");
        const bool guestWill = configuration.ScenarioIs(L"multiplayer_guest_hero_will");
        const bool combat = hostCombat || guestCombat;
        const bool manualCombat = combat && configuration.ManualPlaytest();
        heroWillFocusedAcceptance_ = hostWill || guestWill;
        state_->heroWillFocused = heroWillFocusedAcceptance_;
        const bool anyWill = combat || heroWillFocusedAcceptance_;
        const bool acceptanceHost = hostCombat || hostWill;

        state_->combatTarget.Initialize(
            anyWill,
            acceptanceHost,
            heroWillFocusedAcceptance_ || manualCombat,
            services.Entities(),
            services.Creatures(),
            services.Combat(),
            services.Npcs(),
            diagnostics);
        if (manualCombat)
        {
            // Manual acceptance owns its own pacing. Keep the Hobbe fully
            // damageable and mortal so the tester can verify ordered health
            // and death replication without the automated fixture healing it.
            state_->combatTarget.AllowTargetDeath();
        }
        state_->combatVisual.Initialize(
            combat && !manualCombat,
            hostCombat,
            services.Entities(),
            services.Creatures(),
            services.Combat(),
            diagnostics);
        state_->heroWill.Initialize(
            anyWill && !manualCombat,
            acceptanceHost,
            heroWillFocusedAcceptance_,
            services.Entities(),
            services.HeroWill(),
            diagnostics);
        state_->npcTransfer.Initialize(
            configuration.ScenarioIs(L"multiplayer_host_transition"),
            services.Entities(),
            services.Npcs(),
            multiplayer,
            diagnostics);
        const bool mapStressScenario =
            configuration.ScenarioIs(L"multiplayer_host_map_stress") ||
            configuration.ScenarioIs(L"multiplayer_guest_map_stress");
        const bool mapStress = transition || mapStressScenario;
        state_->mapStress.Initialize(
            mapStress,
            configuration.ScenarioIs(L"multiplayer_host_map_stress") ||
                configuration.ScenarioIs(L"multiplayer_host_transition"),
            configuration.MapStressSeed(),
            transition ? 1 : configuration.MapStressTransitions(),
            // The transition acceptance moves one NPC from the host, which
            // must therefore own the destination map.
            transition,
            services.Entities(),
            multiplayer,
            diagnostics);
        state_->ownerPresentation.Initialize(
            mapStressScenario,
            configuration.ScenarioIs(L"multiplayer_host_map_stress"),
            services.Entities(),
            multiplayer,
            state_->mapStress,
            diagnostics);
        const bool hostSave =
            configuration.ScenarioIs(L"multiplayer_host_save");
        const bool guestSave =
            configuration.ScenarioIs(L"multiplayer_guest_save");
        state_->save.Initialize(
            hostSave || guestSave,
            hostSave,
            services.Entities(),
            services.Players(),
            multiplayer,
            diagnostics);
        const bool hostQuest =
            configuration.ScenarioIs(L"multiplayer_host_quest");
        const bool guestQuest =
            configuration.ScenarioIs(L"multiplayer_guest_quest");
        state_->questSync.Initialize(
            hostQuest || guestQuest,
            hostQuest,
            multiplayer,
            diagnostics);
        const bool hostFeatures =
            configuration.ScenarioIs(L"multiplayer_host_features");
        const bool guestFeatures =
            configuration.ScenarioIs(L"multiplayer_guest_features");
        state_->theresa.Initialize(hostFeatures || guestFeatures, hostFeatures,
            services.Entities(), multiplayer, diagnostics);
        state_->features.Initialize(
            hostFeatures || guestFeatures,
            hostFeatures,
            multiplayer,
            diagnostics);
        const bool hostCutscene =
            configuration.ScenarioIs(L"multiplayer_host_cutscene");
        const bool guestCutscene =
            configuration.ScenarioIs(L"multiplayer_guest_cutscene");
        state_->cutscene.Initialize(
            hostCutscene || guestCutscene,
            hostCutscene,
            services.Entities(),
            services.Players(),
            multiplayer,
            diagnostics);
        return true;
    }

    void AutomationRunner::Tick(float deltaSeconds, bool remotePresentationReady) noexcept
    {
        (void)deltaSeconds;
        if (state_->transitionScenario &&
            !state_->transitionConvergenceReady &&
            state_->mapStress.IsStableSameMap())
        {
            // Convergence is a distributed checkpoint, not a condition that
            // must remain true while both native transitions start. Latch it
            // before either peer leaves, then retire the one-round reunion
            // driver so the faster process cannot make the slower one miss
            // its launch window.
            state_->transitionConvergenceReady = true;
            state_->mapStress.Shutdown();
            state_->diagnostics.Event(
                "MultiplayerTransitionConvergenceReady",
                "both peer actor lifecycles are active in one numeric map; native transition acceptance may begin independently");
        }
        const bool transitionReady = !state_->transitionScenario ||
            state_->transitionConvergenceReady;
        state_->npcTransfer.Tick(
            state_->transitionScenario
                ? transitionReady
                : remotePresentationReady);
        state_->mapStress.Tick();
        state_->ownerPresentation.Tick();
        state_->save.Tick(remotePresentationReady);
        state_->questSync.Tick(remotePresentationReady);
        state_->features.Tick(remotePresentationReady);
        state_->cutscene.Tick(remotePresentationReady);
        state_->transition.Tick(
            state_->transitionScenario
                ? transitionReady
                : remotePresentationReady);
        if (state_->combatVisual.WantsTargetDeath())
        {
            state_->combatTarget.AllowTargetDeath();
        }
        state_->combatTarget.Tick(remotePresentationReady);
        state_->combatVisual.Tick(remotePresentationReady);
        state_->heroWill.Tick(
            remotePresentationReady,
            heroWillFocusedAcceptance_
                ? state_->combatTarget.IsTargetReady()
                : state_->combatVisual.IsComplete());
    }

    bool AutomationRunner::ProcessGameThreadIdle() noexcept
    {
        return state_ != nullptr &&
            (state_->transition.ProcessGameThreadIdle() ||
                state_->childhood.ProcessGameThreadIdle() ||
                state_->theresa.ProcessGameThreadIdle() ||
                state_->save.ProcessGameThreadIdle() ||
                state_->questSync.ProcessGameThreadIdle() ||
                state_->features.ProcessGameThreadIdle() ||
                state_->cutscene.ProcessGameThreadIdle() ||
                state_->mapStress.ProcessGameThreadIdle());
    }

    void AutomationRunner::Shutdown() noexcept
    {
        if (state_ == nullptr)
        {
            return;
        }
        state_->npcTransfer.Shutdown();
        state_->childhood.Shutdown();
        state_->theresa.Shutdown();
        state_->save.Shutdown();
        state_->questSync.Shutdown();
        state_->features.Shutdown();
        state_->cutscene.Shutdown();
        state_->ownerPresentation.Shutdown();
        state_->mapStress.Shutdown();
        state_->heroWill.Shutdown();
        state_->combatVisual.Shutdown();
        state_->combatTarget.Shutdown();
        state_->transition.Shutdown();
        state_->multiplayer = nullptr;
        state_->diagnostics = {};
        state_->transitionScenario = false;
        heroWillFocusedAcceptance_ = false;
    }

    bool AutomationRunner::IsTargetReady() const noexcept
    {
        return state_ != nullptr && state_->combatTarget.IsTargetReady();
    }

    bool AutomationRunner::IsCombatComplete() const noexcept
    {
        return state_ != nullptr && state_->combatVisual.IsComplete();
    }
}

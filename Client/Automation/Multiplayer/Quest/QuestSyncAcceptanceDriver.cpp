#include "QuestSyncAcceptanceDriver.h"

#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace
{
    using fable::multiplayer::protocol::QuestEventOperation;

    // Story state, unlike intentionally local GameflowAssistance warnings.
    constexpr const char* FieldQuest = "Q_SunnyvaleMaster";
    constexpr const char* HostField = "ArcheryHighScore";
    constexpr const char* GuestField = "ArcheryHighScore";
    constexpr std::uint32_t HostFieldValue = 77;
    constexpr std::uint32_t GuestFieldValue = 5;
    // A harmless global world flag from the master-quest data object.
    constexpr const char* WorldFlagQuest = "Q_SunnyvaleMaster";
    constexpr const char* WorldFlag = "ArcheryHighScore";
    constexpr std::uint32_t WorldFlagValue = 123;

    // Retail quests already sampled by the appearance-cycle script. The
    // driver uses the first one that is neither active nor completed in the
    // loaded fixture, so the test never depends on one quest's availability.
    constexpr const char* Candidates[] = {
        "Q_Arena",
        "Q_BanditCamp",
        "Q_AwakeningTheOracle",
        "Q_AmbushTraders",
        "Q_SunnyvaleMaster",
    };
}

namespace fable::automation::multiplayer::quest
{
    static_assert(sizeof(Candidates) / sizeof(Candidates[0]) ==
        QuestSyncAcceptanceDriver::CandidateCount);

    void QuestSyncAcceptanceDriver::Initialize(
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
        diagnostics_.Event("QuestSyncAcceptanceArmed", host_ ? "role=host" : "role=guest");
    }

    void QuestSyncAcceptanceDriver::Tick(const bool remotePresentationReady) noexcept
    {
        if (!enabled_ || multiplayer_ == nullptr ||
            phase_.load(std::memory_order_acquire) != Phase::WaitingForWorld)
        {
            return;
        }
        const auto& contexts = multiplayer_->Contexts();
        const bool localWorldReady =
            contexts.players.localHero.IsWorldReady() &&
            contexts.players.localHero.CurrentState() != nullptr;
        if (remotePresentationReady && localWorldReady &&
            contexts.players.remotePlayers.ActiveCount() != 0)
        {
            Enter(Phase::Baseline, GetTickCount64());
        }
    }

    bool QuestSyncAcceptanceDriver::ProcessGameThreadIdle() noexcept
    {
        if (enabled_ && multiplayer_ && multiplayer_->Contexts().players.localHero.IsWorldReady() &&
            multiplayer_->Contexts().players.remotePlayers.ActiveCount() != 0)
            CheckWorldClock(GetTickCount64());
        const Phase phase = phase_.load(std::memory_order_acquire);
        if (!enabled_ || multiplayer_ == nullptr || phase == Phase::WaitingForWorld ||
            phase == Phase::Done || phase == Phase::Failed)
        {
            return false;
        }
        const std::uint64_t now = GetTickCount64();
        if (now < nextPollAt_) return false;
        nextPollAt_ = now + PollMilliseconds;

        auto& questEvents = multiplayer_->Contexts().world.questEvents;
        auto& hook = questEvents.Hook();
        if (!questEvents.IsActive())
        {
            Fail("quest-event-sync-not-active");
            return false;
        }
        if (!hook.IsManagerReady())
        {
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("quest-manager-not-ready");
            return false;
        }

        char detail[192] = {};
        switch (phase)
        {
        case Phase::Baseline:
            if (!host_ && !hook.OpenMenuForTest()) { Fail("guest-quest-menu-not-opened"); return false; }
            if (!host_) diagnostics_.Event("QuestSyncMenuOpened", "guest quest inventory remains open during remote changes");
            for (std::size_t index = 0; index < CandidateCount; ++index)
            {
                baselineActive_[index] = hook.IsActive(Candidates[index]);
                baselineCompleted_[index] = hook.IsCompleted(Candidates[index]);
                std::snprintf(detail, sizeof(detail), "role=%s quest=%s active=%d completed=%d",
                    host_ ? "host" : "guest", Candidates[index],
                    baselineActive_[index] ? 1 : 0, baselineCompleted_[index] ? 1 : 0);
                diagnostics_.Event("QuestSyncBaseline", detail);
            }
            Enter(host_ ? Phase::HostActivate : Phase::GuestAwaitActive, now);
            return true;

        case Phase::HostActivate:
        {
            if (now - phaseStartedAt_ < HostActivateDelayMilliseconds) return false;
            const std::uint32_t arguments[3] = {1, 1, 0};
            for (std::size_t index = 0; index < CandidateCount; ++index)
            {
                if (baselineActive_[index] || baselineCompleted_[index]) continue;
                const bool accepted = hook.InvokeAsGameplay(
                    QuestEventOperation::Activate, Candidates[index], arguments);
                if (accepted && hook.IsActive(Candidates[index]))
                {
                    if (!hook.OpenMenuForTest()) { Fail("host-quest-menu-not-opened"); return false; }
                    diagnostics_.Event("QuestSyncMenuOpened", "host quest inventory remains open during remote changes");
                    chosen_ = index;
                    completionTarget_ = index;
                    std::snprintf(detail, sizeof(detail), "quest=%s", Candidates[index]);
                    diagnostics_.Event("QuestSyncHostActivated", detail);
                    Enter(Phase::HostAwaitCompleted, now);
                    return true;
                }
                std::snprintf(detail, sizeof(detail), "quest=%s accepted=%d",
                    Candidates[index], accepted ? 1 : 0);
                diagnostics_.Event("QuestSyncHostActivateRejected", detail);
            }
            Fail("no-candidate-quest-could-be-activated");
            return false;
        }

        case Phase::GuestAwaitActive:
            for (std::size_t index = 0; index < CandidateCount; ++index)
            {
                if (!baselineActive_[index] && !baselineCompleted_[index] &&
                    hook.IsActive(Candidates[index]))
                {
                    chosen_ = index;
                    // Finish the quest the host just started. Never the always-
                    // running master quest, whose data holds the world flags.
                    completionTarget_ = index;
                    std::snprintf(detail, sizeof(detail), "quest=%s", Candidates[index]);
                    diagnostics_.Event("QuestSyncGuestObservedActive", detail);
                    Enter(Phase::GuestComplete, now);
                    return true;
                }
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("guest-never-saw-host-quest");
            return false;

        case Phase::GuestComplete:
        {
            // Retail commits a completion on a later tick, so request it once
            // and then poll for the committed state.
            if (!completionRequested_)
            {
                // No completion screen or reward dialog by default. The
                // flags' retail meaning is still being mapped, so an
                // experiment can override them without a rebuild.
                std::uint32_t arguments[3] = {0, 0, 0};
                char flags[16] = {};
                if (GetEnvironmentVariableA("ALBIONTOGETHER_QUEST_TEST_COMPLETE_FLAGS", flags, sizeof(flags)) == 5)
                {
                    arguments[0] = flags[0] == '1' ? 1u : 0u;
                    arguments[1] = flags[2] == '1' ? 1u : 0u;
                    arguments[2] = flags[4] == '1' ? 1u : 0u;
                }
                hook.InvokeAsGameplay(QuestEventOperation::Complete, Candidates[completionTarget_], arguments);
                completionRequested_ = true;
                return true;
            }
            if (!deactivationRequested_)
            {
                // Retail's full completion path (0x01E283E0) marks the quest
                // complete and then deactivates it; the completed state is
                // committed by that deactivation.
                const std::uint32_t arguments[3] = {0, 0, 0};
                hook.InvokeAsGameplay(QuestEventOperation::Deactivate, Candidates[completionTarget_], arguments);
                deactivationRequested_ = true;
                return true;
            }
            if (now - lastProbeAt_ >= 5'000)
            {
                lastProbeAt_ = now;
                std::snprintf(detail, sizeof(detail), "quest=%s active=%d completed=%d",
                    Candidates[completionTarget_],
                    hook.IsActive(Candidates[completionTarget_]) ? 1 : 0,
                    hook.IsCompleted(Candidates[completionTarget_]) ? 1 : 0);
                diagnostics_.Event("QuestSyncCompletionProbe", detail);
            }
            // Retail only records quests with a card in the completed list;
            // card-less story quests just finish and leave the active set.
            // Either way the native outcome here is what the host must match.
            if (!hook.IsActive(Candidates[completionTarget_]))
            {
                std::snprintf(detail, sizeof(detail), "quest=%s active=0 completed=%d",
                    Candidates[completionTarget_],
                    hook.IsCompleted(Candidates[completionTarget_]) ? 1 : 0);
                diagnostics_.Event("QuestSyncGuestCompleted", detail);
                Enter(Phase::GuestAwaitField, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("guest-local-completion-never-committed");
            return false;
        }

        case Phase::HostAwaitCompleted:
            if (!hook.IsActive(Candidates[completionTarget_]))
            {
                std::snprintf(detail, sizeof(detail), "quest=%s active=0 completed=%d",
                    Candidates[completionTarget_],
                    hook.IsCompleted(Candidates[completionTarget_]) ? 1 : 0);
                diagnostics_.Event("QuestSyncHostObservedCompleted", detail);
                Enter(Phase::HostWriteField, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("host-never-saw-guest-completion");
            return false;

        case Phase::HostWriteField:
        {
            auto& fields = multiplayer_->Contexts().world.questFields;
            if (!fields.IsActive())
            {
                Fail("quest-field-sync-not-active");
                return false;
            }
            if (!fields.WriteFieldForTest(FieldQuest, HostField, HostFieldValue))
            {
                if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("host-field-unmapped");
                return false;
            }
            std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", FieldQuest, HostField, HostFieldValue);
            diagnostics_.Event("QuestFieldHostWrote", detail);
            Enter(Phase::HostAwaitField, now);
            return true;
        }

        case Phase::GuestAwaitField:
        {
            auto& fields = multiplayer_->Contexts().world.questFields;
            std::uint32_t value = 0;
            if (fields.ReadFieldForTest(FieldQuest, HostField, value) && value == HostFieldValue)
            {
                std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", FieldQuest, HostField, value);
                diagnostics_.Event("QuestFieldGuestObserved", detail);
                if (!fields.WriteFieldForTest(FieldQuest, GuestField, GuestFieldValue))
                {
                    Fail("guest-field-unmapped");
                    return false;
                }
                std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", FieldQuest, GuestField, GuestFieldValue);
                diagnostics_.Event("QuestFieldGuestWrote", detail);
                Enter(Phase::GuestAwaitWorldFlag, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("guest-never-saw-host-field");
            return false;
        }

        case Phase::HostAwaitField:
        {
            std::uint32_t value = 0;
            if (multiplayer_->Contexts().world.questFields.ReadFieldForTest(FieldQuest, GuestField, value) &&
                value == GuestFieldValue)
            {
                std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", FieldQuest, GuestField, value);
                diagnostics_.Event("QuestFieldHostObserved", detail);
                Enter(Phase::HostWriteWorldFlag, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("host-never-saw-guest-field");
            return false;
        }

        case Phase::HostWriteWorldFlag:
        {
            if (!multiplayer_->Contexts().world.questFields.WriteFieldForTest(
                    WorldFlagQuest, WorldFlag, WorldFlagValue))
            {
                if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("host-world-flag-unmapped");
                return false;
            }
            std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", WorldFlagQuest, WorldFlag, WorldFlagValue);
            diagnostics_.Event("WorldFlagHostWrote", detail);
            Enter(Phase::Done, now);
            return true;
        }

        case Phase::GuestAwaitWorldFlag:
        {
            std::uint32_t value = 0;
            if (multiplayer_->Contexts().world.questFields.ReadFieldForTest(WorldFlagQuest, WorldFlag, value) &&
                value == WorldFlagValue)
            {
                std::snprintf(detail, sizeof(detail), "quest=%s field=%s value=%u", WorldFlagQuest, WorldFlag, value);
                diagnostics_.Event("WorldFlagGuestObserved", detail);
                char cardQuest[128] = {};
                if (!hook.AcceptAvailableCardForTest(cardQuest)) { Fail("no-retail-quest-card-could-be-accepted"); return false; }
                diagnostics_.Event("QuestSyncGuestCardAccepted", cardQuest);
                Enter(Phase::Done, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("guest-never-saw-host-world-flag");
            return false;
        }

        default:
            return false;
        }
    }

    void QuestSyncAcceptanceDriver::CheckWorldClock(const std::uint64_t now) noexcept
    {
        auto& clock = multiplayer_->Contexts().world.worldClock;
        ::fable::multiplayer::protocol::WorldClockMessage state;
        if (!clock.ReadForTest(state)) return;
        if (!clockStartedAt_)
        {
            clockStartedAt_ = now;
            savedClockFraction_ = state.fraction; savedClockRate_ = state.rate; savedClockDay_ = state.day;
            // Deliberately disagree before the first host baseline.
            if (!host_) clock.SetForTest(0.01f, 0, 300);
        }
        const auto age = now - clockStartedAt_;
        if (host_)
        {
            if (clockStage_ >= 4 && clockStage_ <= 6)
            {
                const auto elapsed = clockDrivenAt_ ? now - clockDrivenAt_ : 0;
                clock.AdvanceForTest(static_cast<float>(elapsed < 250 ? elapsed : 250) / 1000.0f);
            }
            clockDrivenAt_ = now;
            const unsigned int stage = age < 12000 ? static_cast<unsigned int>(age / 4000) :
                age < 20000 ? 2 : 3 + static_cast<unsigned int>((age - 20000) / 4000);
            if (stage < clockStage_) return;
            bool set = true;
            switch (stage)
            {
            case 0: set = clock.SetForTest(0.5f, 0, 400); break;
            case 1: set = clock.SetForTest(0.95f, 0, 401); break;
            case 2: set = clock.SetForTest(0.999f, 0.02f, 402); break;
            case 3: set = clock.SetForTest(0.1f, 0, 404) && clock.SetForTest(0.8f, 0.02f, 404, 3); break;
            case 4: set = clock.SetForTest(0.4f, 0.02f, 405) && clock.FreezeForTest(true); break;
            case 5: set = clock.FreezeForTest(false) && clock.SetForTest(0.6f, 0.02f, 406); break;
            case 6: set = clock.SetForTest(savedClockFraction_, savedClockRate_, savedClockDay_); break;
            default: return;
            }
            if (!set) { Fail("native-world-clock-test-set-failed"); return; }
            clockStage_ = stage + 1;
            char detail[64]; std::snprintf(detail, sizeof(detail), "stage=%u", stage);
            diagnostics_.Event("WorldClockHostTestSet", detail);
            return;
        }
        unsigned int bit = 0;
        if (!clockPauseObserved_ && state.day == 403 && clock.HostWorldPausedForTest())
        {
            if (!clockPausedAt_ || std::fabs(clockPausedFraction_ - state.fraction) > 0.001f)
            { clockPausedAt_ = now; clockPausedFraction_ = state.fraction; }
            if (now - clockPausedAt_ >= 1000)
            {
                clockPauseObserved_ = true;
                diagnostics_.Event("WorldClockGuestWorldPauseObserved", "host inventory stopped world update; guest native time remained stable");
            }
        }
        if (state.day == 400 && std::fabs(state.fraction - 0.5f) < 0.001f && !clock.IsNightForTest()) bit = 1;
        if (state.day == 401 && std::fabs(state.fraction - 0.95f) < 0.001f && clock.IsNightForTest()) bit = 2;
        if (state.day == 403 && state.fraction < 0.1f && state.rate == 0.02f) bit = 4;
        if (state.day == 404 && state.jumping && state.fraction > 0.1f && state.fraction < 0.8f) bit = 8;
        if (state.day == 405 && std::fabs(state.fraction - 0.4f) < 0.001f)
        {
            if (!clockFrozenAt_) clockFrozenAt_ = now;
            if (now - clockFrozenAt_ >= 1000) bit = 16;
        }
        if (state.day == 406 && state.fraction > 0.62f && state.rate == 0.02f) bit = 32;
        if (bit && !(clockObserved_ & bit))
        {
            clockObserved_ |= bit;
            char detail[96]; std::snprintf(detail, sizeof(detail), "mask=%u day=%d fraction=%.6f", clockObserved_, state.day, state.fraction);
            diagnostics_.Event("WorldClockGuestTestObserved", detail);
            if (clockObserved_ == 63) diagnostics_.Event("WorldClockGuestAcceptancePassed", "native noon/night/midnight/animated-sleep/freeze/resume matched host");
        }
        if (age > 40000 && clockObserved_ != 63 && clockObserved_ != 0xFFFF)
        { clockObserved_ = 0xFFFF; Fail("world-clock-native-states-did-not-converge"); }
    }

    void QuestSyncAcceptanceDriver::Enter(const Phase phase, const std::uint64_t now) noexcept
    {
        phase_.store(phase, std::memory_order_release);
        phaseStartedAt_ = now;
        nextPollAt_ = 0;
    }

    void QuestSyncAcceptanceDriver::Fail(const char* const reason) noexcept
    {
        phase_.store(Phase::Failed, std::memory_order_release);
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "role=%s reason=%s", host_ ? "host" : "guest", reason);
        diagnostics_.Event("QuestSyncFailed", detail);
        diagnostics_.Event("ClientFailed", "multiplayer-quest-sync-acceptance");
    }

    void QuestSyncAcceptanceDriver::Shutdown() noexcept
    {
        multiplayer_ = nullptr;
        diagnostics_ = {};
        phase_.store(Phase::WaitingForWorld, std::memory_order_release);
        phaseStartedAt_ = 0;
        nextPollAt_ = 0;
        for (std::size_t index = 0; index < CandidateCount; ++index)
        {
            baselineActive_[index] = false;
            baselineCompleted_[index] = false;
        }
        chosen_ = CandidateCount;
        completionTarget_ = CandidateCount;
        completionRequested_ = false;
        deactivationRequested_ = false;
        lastProbeAt_ = 0;
        host_ = false;
        enabled_ = false;
        clockStartedAt_ = clockFrozenAt_ = 0;
        clockDrivenAt_ = 0;
        clockPausedAt_ = 0; clockPauseObserved_ = false; clockPausedFraction_ = 0;
        clockStage_ = clockObserved_ = 0;
    }
}

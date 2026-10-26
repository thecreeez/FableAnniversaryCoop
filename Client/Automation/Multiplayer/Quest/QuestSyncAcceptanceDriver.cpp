#include "QuestSyncAcceptanceDriver.h"

#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>

namespace
{
    using fable::multiplayer::protocol::QuestEventOperation;

    // Always-running retail bookkeeping quest with plain int progress fields.
    constexpr const char* FieldQuest = "GameflowAssistance";
    constexpr const char* HostField = "WillEnergyWarningsGiven";
    constexpr const char* GuestField = "SaveWarningsGiven";
    constexpr std::uint32_t HostFieldValue = 77;
    constexpr std::uint32_t GuestFieldValue = 5;

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
            for (std::size_t index = 0; index < CandidateCount; ++index)
            {
                baselineActive_[index] = hook.IsActive(Candidates[index]);
                baselineCompleted_[index] = hook.IsCompleted(Candidates[index]);
                std::snprintf(detail, sizeof(detail), "role=%s quest=%s active=%d completed=%d",
                    host_ ? "host" : "guest", Candidates[index],
                    baselineActive_[index] ? 1 : 0, baselineCompleted_[index] ? 1 : 0);
                diagnostics_.Event("QuestSyncBaseline", detail);
            }
            // Retail records a completion only through the quest's card
            // object, so the completion leg uses a quest that is already
            // running in the fixture (and therefore has its card) on both
            // peers, rather than the one this test activated.
            for (std::size_t index = 0; index < CandidateCount; ++index)
            {
                if (baselineActive_[index] && !baselineCompleted_[index])
                {
                    completionTarget_ = index;
                    break;
                }
            }
            if (completionTarget_ == CandidateCount)
            {
                Fail("fixture-has-no-running-candidate-quest");
                return false;
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
                    chosen_ = index;
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
                Enter(Phase::Done, now);
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
                Enter(Phase::Done, now);
                return true;
            }
            if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds) Fail("host-never-saw-guest-field");
            return false;
        }

        default:
            return false;
        }
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
    }
}

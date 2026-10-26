#include "ScenarioRunners.h"

#include "../../Diagnostics/EventLog.h"

#include <iostream>

namespace fable::launcher::multiplayer
{
    int RunQuestScenario(MultiplayerTestSession& session)
    {
        PeerHarness& peers = session.peers();
        Peer& host = peers.host();
        Peer& guest = peers.guest();

        // Hollow-pass guards: both peers must be connected, both must have the
        // lifecycle hook installed, and each observed state change must be
        // attributed to a replayed network event rather than local play.
        const bool connected =
            peers.WaitEvent(host, "MultiplayerRemoteAvatarReady") &&
            peers.WaitEvent(guest, "MultiplayerRemoteAvatarReady") &&
            peers.WaitEvent(host, "MultiplayerQuestLifecycleHookReady") &&
            peers.WaitEvent(guest, "MultiplayerQuestLifecycleHookReady");
        const bool hostToGuest = connected &&
            peers.WaitEvent(host, "QuestSyncHostActivated") &&
            peers.WaitEventDetail(host, "MultiplayerQuestEventSent", "op=activate") &&
            peers.WaitEventDetail(guest, "MultiplayerQuestEventApplied", "op=activate") &&
            peers.WaitEvent(guest, "QuestSyncGuestObservedActive");
        const bool guestToHost = hostToGuest &&
            peers.WaitEvent(guest, "QuestSyncGuestCompleted") &&
            peers.WaitEventDetail(guest, "MultiplayerQuestEventSent", "op=complete") &&
            peers.WaitEventDetail(host, "MultiplayerQuestEventApplied", "op=complete") &&
            peers.WaitEvent(host, "QuestSyncHostObservedCompleted");
        // The host's rebroadcast of the guest's own completion must be inert
        // on the guest; replaying it would complete and reward the quest twice.
        // Let the echo arrive before judging.
        const bool echoInert = guestToHost &&
            peers.WaitEventDetailCount(guest, "MultiplayerQuestEventSatisfied", "op=complete", 1) &&
            diagnostics::EventDetailCount(
                diagnostics::ReadEventFile(guest.events),
                "MultiplayerQuestEventApplied", "op=complete") == 0;
        // The host's native outcome must equal the guest's: both finished the
        // quest, and both agree on whether retail recorded it as completed.
        const std::string guestEvents = diagnostics::ReadEventFile(guest.events);
        const std::string hostEvents = diagnostics::ReadEventFile(host.events);
        const char* const completedFlag =
            diagnostics::EventDetailContains(guestEvents, "QuestSyncGuestCompleted", "completed=1")
                ? "completed=1" : "completed=0";
        const bool outcomesMatch = echoInert &&
            diagnostics::EventDetailContains(hostEvents, "QuestSyncHostObservedCompleted", completedFlag);
        // Intermediate progress: a named quest-script field changed on one
        // peer must be written on the other, in both directions.
        const bool fieldsShared = outcomesMatch &&
            peers.WaitEvent(host, "QuestFieldHostWrote") &&
            peers.WaitEventDetail(guest, "MultiplayerQuestFieldApplied", "field=WillEnergyWarningsGiven") &&
            peers.WaitEvent(guest, "QuestFieldGuestObserved") &&
            peers.WaitEvent(guest, "QuestFieldGuestWrote") &&
            peers.WaitEventDetail(host, "MultiplayerQuestFieldApplied", "field=SaveWarningsGiven") &&
            peers.WaitEvent(host, "QuestFieldHostObserved");
        const bool responsive = fieldsShared &&
            peers.IsResponsive(host) && peers.IsResponsive(guest);
        const bool stopped = session.Shutdown();
        if (!connected || !hostToGuest || !guestToHost || !echoInert || !outcomesMatch || !fieldsShared || !responsive || !stopped)
        {
            std::wcerr
                << L"Multiplayer quest acceptance failed: "
                << (!connected ? L"peers or quest hooks were not ready"
                    : !hostToGuest ? L"the host's quest start never became active on the guest"
                    : !guestToHost ? L"the guest's quest completion never reached the host"
                    : !echoInert ? L"the guest replayed its own completion from the host echo (double completion)"
                    : !outcomesMatch ? L"host and guest disagree on the finished quest's completed state"
                    : !fieldsShared ? L"a quest progress field changed on one peer did not reach the other"
                    : !responsive ? L"a peer stopped responding"
                    : L"shutdown was not clean")
                << L".\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return 1;
        }
        std::wcout
            << L"Multiplayer quest acceptance passed: a quest started on the host through the retail CQuestManager became active on the guest by network replay, and a quest the guest finished (complete then deactivate, as retail does) finished on the host with the same native outcome, without a double completion; quest-script progress fields changed on either peer were written on the other.\n"
            << L"State root: " << session.context().sessionRoot.wstring() << L"\n";
        return 0;
    }
}

#include "ScenarioRunners.h"

#include "../../Diagnostics/EventLog.h"

#include <iostream>

namespace fable::launcher::multiplayer
{
    int RunCutsceneScenario(MultiplayerTestSession& session)
    {
        PeerHarness& peers = session.peers();
        Peer& host = peers.host();
        Peer& guest = peers.guest();

        const bool connected =
            peers.WaitEvent(host, "MultiplayerRemoteAvatarReady") &&
            peers.WaitEvent(guest, "MultiplayerRemoteAvatarReady");
        // The host enters alone: the quest cutscene is held with the native
        // waiting box (1 of 2 players in the circle).
        const bool held = connected &&
            peers.WaitEventDetail(host, "CutsceneAcceptanceHostArrived", "visit=1") &&
            peers.WaitEvent(host, "CutsceneHeldForGathering") &&
            peers.WaitEvent(host, "CutsceneHoldAnchored");
        // A click on the box cancels the wait: the scripted sequence ends,
        // the script's hold on the Hero is detached and the Hero is moved
        // back to Lookout Point. Nothing plays yet.
        const bool cancelled = held &&
            peers.WaitEvent(host, "CutsceneAcceptanceHostDismissed") &&
            peers.WaitEvent(host, "CutsceneWaitCancelled") &&
            peers.WaitEventDetail(host, "CutsceneModeSuspended", "sequence=ended") &&
            peers.WaitEvent(host, "CutsceneWaitRetreated") &&
            peers.WaitEvent(host, "CutsceneAcceptanceHostRetreated");
        const bool notPlayedEarly = cancelled &&
            !diagnostics::EventWasReported(diagnostics::ReadEventFile(host.events), "CutsceneStartReturned");
        // Both players gather: the hold releases, the sequence and the
        // Hero control are restored, and the cutscene plays on both peers.
        const bool played = notPlayedEarly &&
            peers.WaitEventDetail(host, "CutsceneAcceptanceHostArrived", "visit=2") &&
            peers.WaitEvent(guest, "CutsceneAcceptanceGuestArrived") &&
            peers.WaitEvent(host, "CutsceneGatheredAndReleased") &&
            peers.WaitEventDetail(host, "CutsceneModeRestored", "sequence=begun") &&
            peers.WaitEvent(host, "CutsceneStartReturned") &&
            peers.WaitEvent(guest, "CutsceneStartReturned");
        // Enemies attack every player, with the players far apart: the wasp
        // hits the host, then (moved to the guest) the guest, whose own game
        // loses health, then the host again after the players swap.
        const bool enemies = played &&
            peers.WaitEvent(guest, "CutsceneSkipLocal") &&
            peers.WaitEvent(host, "CutsceneSkipApplied") &&
            peers.WaitEvent(host, "CutsceneAcceptanceCutsceneOver") &&
            peers.WaitEvent(host, "EnemyTestCorpseSwarmSelected") &&
            peers.WaitEvent(host, "EnemyTestHostNearWasp") &&
            peers.WaitEvent(host, "QuestScreenClosedByOwner") &&
            peers.WaitEventDetail(guest, "MultiplayerScriptedEntityMovementBound", "script=WaspVictim") &&
            peers.WaitEventDetail(host, "EnemyTestWaspAttacked", "target=host ") &&
            peers.WaitEvent(host, "EnemyTestSwarmDistributed") &&
            peers.WaitEvent(host, "MultiplayerNativeEnemyTargetRead") &&
            peers.WaitEvent(guest, "ExperienceOrbReplicaReady") &&
            peers.WaitEvent(guest, "ExperienceOrbReplicaRemoved") &&
            peers.WaitEvent(host, "ExperienceOrbCollected") &&
            peers.WaitEvent(host, "EnemyTestHostToGuestSpot") &&
            peers.WaitEvent(guest, "EnemyTestCorpseSwarmSelected") &&
            peers.WaitEvent(guest, "EnemyTestGuestNearWasp") &&
            peers.WaitEventDetail(host, "EnemyTestWaspAttacked", "target=guest") &&
            peers.WaitEvent(guest, "EnemyTestGuestHurt") &&
            peers.WaitEvent(guest, "EnemyTestGuestMovedToHost") &&
            peers.WaitEvent(host, "EnemyTestHostBackToWasp") &&
            peers.WaitEventDetail(host, "EnemyTestWaspAttacked", "target=host-again") &&
            peers.WaitEvent(guest, "ResurrectionFlaskAcceptancePassed") &&
            peers.WaitEvent(guest, "MultiplayerResurrectionFlaskConsumedNoPause") &&
            peers.WaitEvent(guest, "MultiplayerResurrectionTutorialSuppressed");
        const bool noDriverFailure = enemies &&
            !diagnostics::EventWasReported(diagnostics::ReadEventFile(host.events), "CutsceneAcceptanceFailed") &&
            !diagnostics::EventWasReported(diagnostics::ReadEventFile(guest.events), "CutsceneAcceptanceFailed");
        const bool responsive = noDriverFailure && peers.IsResponsive(host) && peers.IsResponsive(guest);
        const bool stopped = session.Shutdown();
        if (!connected || !held || !cancelled || !notPlayedEarly || !played || !enemies || !noDriverFailure || !responsive || !stopped)
        {
            std::wcerr
                << L"Multiplayer cutscene acceptance failed: "
                << (!connected ? L"peers did not connect"
                    : !held ? L"the host's quest cutscene was not held for gathering"
                    : !cancelled ? L"dismissing the waiting box did not cancel and move the host back"
                    : !notPlayedEarly ? L"the cutscene played before everyone gathered"
                    : !played ? L"the gathered cutscene did not play on both peers"
                    : !enemies ? L"the selected corpse swarms did not damage the host, then the guest, then the host again"
                    : !noDriverFailure ? L"the acceptance driver reported a failure"
                    : !responsive ? L"a peer stopped responding"
                    : L"shutdown was not clean")
                << L".\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return 1;
        }
        std::wcout
            << L"Multiplayer cutscene acceptance passed: the host's quest cutscene was held for gathering; dismissing the waiting box ended the scripted sequence and moved the host back; once both players gathered the sequence was restored and the cutscene played on both peers; a wasp attacked the host, then the guest (who lost health in its own game), then the host again, with the players far apart.\n"
            << L"State root: " << session.context().sessionRoot.wstring() << L"\n";
        return 0;
    }
}

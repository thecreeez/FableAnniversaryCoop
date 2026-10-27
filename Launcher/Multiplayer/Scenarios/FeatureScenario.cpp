#include "ScenarioRunners.h"

#include "../../Diagnostics/EventLog.h"

#include <iostream>

namespace fable::launcher::multiplayer
{
    int RunFeatureScenario(MultiplayerTestSession& session)
    {
        PeerHarness& peers = session.peers();
        Peer& host = peers.host();
        Peer& guest = peers.guest();

        // Every co-op feature hook must be installed on both peers.
        const char* const hooks[] = {
            "ChestSyncReady", "DemonDoorSyncReady", "KillExperienceShareReady",
            "QuestScreenHookReady", "QuestMapCameraGuardReady"};
        bool hooksReady = peers.WaitEvent(host, "MultiplayerRemoteAvatarReady") &&
            peers.WaitEvent(guest, "MultiplayerRemoteAvatarReady");
        const char* missingHook = nullptr;
        for (const char* hook : hooks)
        {
            if (!hooksReady) break;
            if (!peers.WaitEvent(host, hook) || !peers.WaitEvent(guest, hook))
            {
                hooksReady = false;
                missingHook = hook;
            }
        }
        // Roll and block: captured on the host, performed on the guest's
        // remote Hero presentation.
        const bool roll = hooksReady &&
            peers.WaitEventDetail(host, "FeatureHostRolled", "accepted=1") &&
            peers.WaitEventDetail(host, "MultiplayerLocalMotionCaptured", "kind=Roll") &&
            peers.WaitEventDetail(guest, "MultiplayerRemoteMotionPerformed", "kind=roll");
        const bool block = roll &&
            peers.WaitEventDetail(host, "FeatureHostBlocked", "accepted=1") &&
            peers.WaitEventDetail(host, "MultiplayerLocalMotionCaptured", "kind=Block") &&
            peers.WaitEventDetail(guest, "MultiplayerRemoteMotionPerformed", "kind=block");
        // Experience: the guest gains 0.3x of the host's strength and kill
        // experience (100 -> 30).
        const bool statExperience = block &&
            peers.WaitEventDetail(host, "FeatureHostStatExperience", "added=1") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "strength=100") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "share=(0,30,0,0)");
        const bool killExperience = statExperience &&
            peers.WaitEvent(host, "FeatureHostKillExperience") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "share=(30,0,0,0)");
        // Quest card: shown on the host, replayed on the guest.
        const bool questCard = killExperience &&
            peers.WaitEventDetail(host, "FeatureHostQuestCard", "shown=1") &&
            peers.WaitEventDetail(guest, "MultiplayerQuestEventApplied", "quest-screen");
        const bool responsive = questCard && peers.IsResponsive(host) && peers.IsResponsive(guest);
        const bool stopped = session.Shutdown();
        if (!hooksReady || !roll || !block || !statExperience || !killExperience || !questCard || !responsive || !stopped)
        {
            std::wcerr << L"Multiplayer feature acceptance failed: ";
            if (!hooksReady)
            {
                std::wcerr << L"a co-op hook was not installed on both peers";
                if (missingHook != nullptr) std::wcerr << L" (" << missingHook << L")";
            }
            else
            {
                std::wcerr
                    << (!roll ? L"the host's roll was not performed on the guest's remote Hero"
                        : !block ? L"the host's block was not performed on the guest's remote Hero"
                        : !statExperience ? L"the host's strength experience did not give the guest 0.3x"
                        : !killExperience ? L"the host's kill experience did not give the guest 0.3x"
                        : !questCard ? L"the host's quest card was not shown on the guest"
                        : !responsive ? L"a peer stopped responding"
                        : L"shutdown was not clean");
            }
            std::wcerr << L".\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return 1;
        }
        std::wcout
            << L"Multiplayer feature acceptance passed: co-op hooks are installed on both peers; the host's roll and block were performed on the guest's remote Hero; host strength and kill experience gave the guest 0.3x; the host's quest card was shown on the guest.\n"
            << L"State root: " << session.context().sessionRoot.wstring() << L"\n";
        return 0;
    }
}

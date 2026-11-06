#include "ScenarioRunners.h"

#include "../../Diagnostics/EventLog.h"

#include <iostream>
#include <Windows.h>

namespace fable::launcher::multiplayer
{
    int RunFeatureScenario(MultiplayerTestSession& session)
    {
        PeerHarness& peers = session.peers();
        Peer& host = peers.host();
        Peer& guest = peers.guest();
        wchar_t theresaMode[32] = {};
        if (GetEnvironmentVariableW(L"ALBIONTOGETHER_THERESA_TEST", theresaMode, 32))
        {
            const bool checkpoint = std::wstring(theresaMode) == L"checkpoint";
            const char* finalEvent = checkpoint ? "TheresaCheckpointRestoredVerified" : "TheresaOutfitRestoredVerified";
            bool okay = checkpoint || (peers.WaitEvent(host,"TheresaProgressionDampingVerified") &&
                peers.WaitEvent(guest,"TheresaProgressionDampingVerified") &&
                peers.WaitEvent(host,"TheresaOutfitChangedVerified") &&
                peers.WaitEvent(guest,"TheresaOutfitChangedVerified"));
            okay = okay && peers.WaitEvent(host,finalEvent) && peers.WaitEvent(guest,finalEvent) &&
                peers.IsResponsive(host) && peers.IsResponsive(guest);
            if (!checkpoint)
            {
                automation::CaptureGameWindow(host.game.processId,host.root/L"theresa-outfit.bmp");
                automation::CaptureGameWindow(guest.game.processId,guest.root/L"theresa-outfit.bmp");
            }
            const bool stopped = session.Shutdown();
            std::wcout << L"Multiplayer Theresa acceptance " << (okay && stopped ? L"passed" : L"failed")
                << L": " << theresaMode << L".\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return okay && stopped ? 0 : 1;
        }

        if (GetEnvironmentVariableW(L"ALBIONTOGETHER_SHARED_WORLD_TEST", nullptr, 0))
        {
            bool okay = peers.WaitEvent(host, "SharedWorldRangeVerified") && peers.WaitEvent(guest, "SharedWorldRangeVerified") &&
                peers.WaitEvent(host, "MultiplayerGuestSleepAppliedByHost") &&
                peers.WaitEvent(host, "SharedWorldGuestSleepVerified") && peers.WaitEvent(guest, "SharedWorldGuestSleepVerified") &&
                peers.WaitEvent(guest, "SharedWorldGuestSmashIssued") && peers.WaitEvent(host, "MultiplayerDestructibleApplied") &&
                peers.WaitEvent(guest, "SharedWorldTerminalDeathVerified") && peers.WaitEvent(host, "SharedWorldTerminalDeathVerified") &&
                peers.WaitEvent(host, "SharedWorldCheckpointVerified") && peers.WaitEvent(guest, "SharedWorldCheckpointVerified") &&
                peers.IsResponsive(host) && peers.IsResponsive(guest);
            const bool stopped = session.Shutdown();
            std::wcout << L"Shared world acceptance: " << (okay && stopped ? L"PASS" : L"FAIL") << L"\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return okay && stopped ? 0 : 1;
        }

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
            peers.WaitEventDetail(host, "FeatureHostBlocked", "accepted=1");
        // Held block replication was manually accepted by the owner; its
        // duration is not inferred from this one-shot native smoke action.
        // Experience: the guest gains 0.3x of the host's strength and kill
        // experience (100 -> 30).
        const bool stance = block &&
            peers.WaitEvent(host, "FeatureHostCombatStance") &&
            peers.WaitEventDetail(guest, "MultiplayerCombatStanceApplied", "active=1 native=1");
        const bool statExperience = stance &&
            peers.WaitEventDetail(host, "FeatureHostStatExperience", "added=1") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "strength=100") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "share=(0,30,0,0)");
        const bool fractionalExperience = statExperience &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "strength=1 skill=0 will=0 share=(0,1,0,0)");
        const bool killExperience = fractionalExperience &&
            peers.WaitEvent(host, "FeatureHostKillExperience") &&
            peers.WaitEventDetail(guest, "KillExperienceShared", "share=(30,0,0,0)");
        // Quest card: shown on the host, replayed on the guest.
        const bool questCard = killExperience &&
            peers.WaitEventDetail(host, "FeatureHostQuestCard", "shown=1") &&
            peers.WaitEventDetail(guest, "MultiplayerQuestEventApplied", "quest-screen");
        const bool responsive = questCard && peers.IsResponsive(host) && peers.IsResponsive(guest);
        const bool stopped = session.Shutdown();
        if (!hooksReady || !roll || !block || !stance || !statExperience || !fractionalExperience || !killExperience || !questCard || !responsive || !stopped)
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
                        : !stance ? L"host combat stance was not applied on the guest"
                        : !statExperience ? L"the host's strength experience did not give the guest 0.3x"
                        : !fractionalExperience ? L"one-point experience awards were rounded away"
                        : !killExperience ? L"the host's kill experience did not give the guest 0.3x"
                        : !questCard ? L"the host's quest card was not shown on the guest"
                        : !responsive ? L"a peer stopped responding"
                        : L"shutdown was not clean");
            }
            std::wcerr << L".\nState root: " << session.context().sessionRoot.wstring() << L"\n";
            return 1;
        }
        std::wcout
            << L"Multiplayer feature acceptance passed: co-op hooks are installed on both peers; the host's roll was performed on the guest's remote Hero; the native block smoke action was accepted; host strength and kill experience gave the guest 0.3x; the host's quest card was shown on the guest.\n"
            << L"State root: " << session.context().sessionRoot.wstring() << L"\n";
        return 0;
    }
}

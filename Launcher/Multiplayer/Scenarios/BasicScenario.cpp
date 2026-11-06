#include "ScenarioRunners.h"

#include "../../Diagnostics/EventLog.h"

#include <iostream>
#include <Windows.h>

namespace fable::launcher::multiplayer
{
int RunBasicScenario(MultiplayerTestSession& session)
{
    PeerHarness& peers = session.peers();
    const std::uint64_t hostActor =
        diagnostics::StablePlayerActorId(L"host", L"Host");
    const std::uint64_t guestActor =
        diagnostics::StablePlayerActorId(L"guest", L"Guest");
    const bool hostMoved = peers.Move(peers.host()) &&
        peers.WaitBackgroundMovement(peers.guest(), hostActor);
    const bool guestMoved = hostMoved && peers.Move(peers.guest()) &&
        peers.WaitBackgroundMovement(peers.host(), guestActor);
    const bool nativeAnimation = guestMoved &&
        (diagnostics::EventWasReported(
             diagnostics::ReadEventFile(peers.host().events),
             "MultiplayerRemoteAvatarWalking") ||
            diagnostics::EventWasReported(
                diagnostics::ReadEventFile(peers.guest().events),
                "MultiplayerRemoteAvatarWalking"));
    const bool childhood = GetEnvironmentVariableW(L"ALBIONTOGETHER_CHILDHOOD_TEST", nullptr, 0) != 0;
    const char* skin = childhood
        ? "attached=true stage=attached skin_bones=98 driver_bones=91 map_count=98"
        : "attached=true stage=attached skin_bones=80 driver_bones=91 map_count=80";
    const bool appearancesApplied = nativeAnimation &&
        peers.WaitEventDetail(peers.host(), "MultiplayerTheresaHeroSkinAttached",
            skin) &&
        peers.WaitEventDetail(peers.guest(), "MultiplayerTheresaHeroSkinAttached",
            skin) &&
        (childhood || (peers.WaitEvent(peers.host(), "MultiplayerRemoteClothingApplied") &&
        peers.WaitEvent(peers.guest(), "MultiplayerRemoteAppearanceModifiersApplied") &&
        peers.WaitEvent(peers.guest(), "MultiplayerRemoteBoneScalesApplied")));
    const bool guestStopped = session.Shutdown();
    if (!nativeAnimation || !appearancesApplied || !guestStopped)
    {
        std::wcerr << L"Multiplayer acceptance failed during movement or shutdown.\n";
        return 1;
    }
    std::wcout
        << L"Multiplayer structural acceptance passed: both remote definitions were created, both peers applied remote movement, native animation remained live, and both observers attached the correct Sister mesh to the native Hero driver with a complete bone map. This command does not claim pixel-level visibility.\n"
        << L"State root: " << session.context().sessionRoot.wstring() << L"\n";
    return 0;
}
}

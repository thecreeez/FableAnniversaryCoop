#pragma once

#include "../MultiplayerTestSession.h"

namespace fable::launcher::multiplayer
{
int RunBasicScenario(MultiplayerTestSession& session);
int RunRosterScenario(MultiplayerTestSession& session);
int RunManualScenario(MultiplayerTestSession& session);
int RunTransitionScenario(MultiplayerTestSession& session);
int RunMapStressScenario(MultiplayerTestSession& session);
int RunSaveScenario(MultiplayerTestSession& session);
int RunQuestScenario(MultiplayerTestSession& session);
int RunFeatureScenario(MultiplayerTestSession& session);
int RunCutsceneScenario(MultiplayerTestSession& session);
int RunAuthorityScenario(MultiplayerTestSession& session);
int RunCombatScenario(MultiplayerTestSession& session);
int RunHeroWillScenario(MultiplayerTestSession& session);
int RunMultiplayerTest(const MultiplayerTestContext& context);
}

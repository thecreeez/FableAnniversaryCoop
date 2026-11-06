#include "ChildhoodAcceptanceDriver.h"
#include "Game/Runtime/GameServiceRuntime.h"
#include "Game/Entity/Entity.h"
#include "Game/Player/PlayerService.h"
#include "Game/Quest/QuestService.h"
#include "Game/Persistence/Native/AutoSaveFunction.h"
#include "Game/Cutscene/CutsceneObserver.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>

namespace fable::automation::local_instance
{
    void ChildhoodAcceptanceDriver::Initialize(game::GameServiceRuntime* services,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        services_ = services;
        diagnostics_ = diagnostics;
        if (services_) diagnostics_.Event("ChildhoodAcceptanceArmed", "real-new-game; no quest or age mutation");
    }

    void ChildhoodAcceptanceDriver::Shutdown() noexcept
    {
        services_ = nullptr;
        phase_ = 0;
        nextPoll_ = saveAt_ = 0;
        origin_ = {};
        diagnostics_ = {};
    }

    bool ChildhoodAcceptanceDriver::ProcessGameThreadIdle() noexcept
    {
        if (!services_ || phase_ == 3 || GetTickCount64() < nextPoll_) return false;
        nextPoll_ = GetTickCount64() + 250;
        bool inCutscene=false;
        if (!game::cutscene::CutsceneObserver::ReadNativeState(GetModuleHandleW(nullptr),inCutscene) || inCutscene) return false;
        game::Entity* hero = services_->Players().GetHero();
        if (!hero) return false;
        const bool child = hero->IsValid() && hero->GetDefinitionName() == "CREATURE_HERO_CHILD";
        const auto position = child ? hero->GetPosition() : game::Vector3{};
        const auto map = child ? hero->GetCurrentMapName() : std::string{};
        hero->Release();
        if (!child || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
            !services_->Quests().IsActive("Q_NewOakValeIntro")) return false;
        if (phase_ == 0)
        {
            origin_ = position;
            phase_ = 1;
            char detail[192] = {};
            std::snprintf(detail, sizeof(detail), "quest=Q_NewOakValeIntro definition=CREATURE_HERO_CHILD map=%s", map.c_str());
            diagnostics_.Event("ChildhoodStoryReady", detail);
        }
        else if (phase_ == 1)
        {
            wchar_t inputFile[MAX_PATH]={};
            if (!GetEnvironmentVariableW(L"ALBIONTOGETHER_CHILDHOOD_INPUT_FILE",inputFile,MAX_PATH) ||
                GetFileAttributesW(inputFile)==INVALID_FILE_ATTRIBUTES) return false;
            const float dx = position.x - origin_.x, dy = position.y - origin_.y;
            if (dx * dx + dy * dy < 1.0f) return false;
            if (!game::persistence::native::AutoSaveFunction::Invoke(services_->Entities())) return false;
            saveAt_ = GetTickCount64();
            phase_ = 2;
            diagnostics_.Event("ChildhoodStoryMovementVerified", "active intro quest; native child moved at least 1m after launcher input; native AutoSave invoked");
        }
        else if (phase_ == 2 && GetTickCount64() - saveAt_ >= 3000)
        {
            game::persistence::native::AutoSaveState state;
            if (!game::persistence::native::AutoSaveFunction::ReadState(services_->Entities(), state) || state.saveState != 0) return false;
            phase_ = 3;
            diagnostics_.Event("ChildhoodAcceptancePassed", "real child intro quest remained active after native save; launcher must verify save file");
        }
        return false;
    }
}

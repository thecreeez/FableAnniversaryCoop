#pragma once

#include "GameplayRuntime.h"
#include "GameplayFrameMailbox.h"
#include "Automation/Runtime/AutomationRunner.h"
#include "DeveloperTools/Game/MultiplayerSaveSectionStatusProvider.h"
#include "Game/Runtime/GameServiceRuntime.h"
#include "Multiplayer/Runtime/MultiplayerSession.h"
#include "Scripting/Runtime/Host/ScriptHost.h"
#include "UI/ImGui/Runtime/ImGuiDx9Runtime.h"
#include "Game/HeroPawn/Appearance/Hooks/TheresaAppearanceHook.h"

#include <atomic>

namespace fable::game
{
    // Private composition state shared by initialization and frame dispatch.
    // Gameplay requests cross through the mailbox; loaded status is atomic.
    // The ImGui runtime separately owns its window/render synchronization.
    class GameplayRuntime::State final
    {
    public:
        GameServiceRuntime services;
        multiplayer::MultiplayerSession multiplayer;
        automation::runtime::AutomationRunner automation;
        scripting::ScriptHost scripts;
        developer_tools::MultiplayerSaveSectionStatusProvider developerSaveSections;
        developer_tools::runtime::DeveloperToolsRuntime developerTools;
        ui::imgui::ImGuiDx9Runtime scriptUi;
        core::Diagnostics diagnostics = {};
        hero_pawn::appearance::hooks::TheresaAppearanceHook previewAppearance;
        void* previewHero = nullptr;
        bool theresaPreview = false;
        GameplayFrameMailbox mailbox;
        ULONGLONG lastFrameAt = 0;
        bool servicesReady = false;
        bool multiplayerReady = false;
        bool automationReady = false;
        std::atomic_bool scriptsReady{false};
    };
}

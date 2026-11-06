#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"
#include "Game/Quest/Sync/QuestScreenHook.h"
#include "CutsceneAcceptanceDriver.h"
#include "Game/HeroPawn/Appearance/Hooks/TheresaHeroSkin.h"

#include "Automation/Multiplayer/Transition/NativeRegionRoute.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Game/Player/PlayerService.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/World/Travel/Native/WorldTravelFunctions.h"
#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{
    // Native map ids of the wasp fixture's two regions (the region exit
    // descriptors and PlayerState use the same ids).
    constexpr std::uint16_t LookoutPointMapId = 1;
    constexpr std::uint16_t PicnicAreaMapId = 2;
    constexpr const char* LookoutPoint = "LookoutPoint";
    constexpr const char* PicnicArea = "PicnicArea";
    constexpr std::uint64_t WaspTeleportMilliseconds = 6'000;
    constexpr float NearDistance = 8.0f;

    // CTCQuestCompletionUI (Hero component 0xFC): +0x0D card shown, +0x20
    // accepted. The card's Enter/accept sets the latter and the component's
    // next update closes the card and unpauses the world, as retail does.
    constexpr std::int32_t QuestCardComponent = 0xFC;

    int FlaskCount(void* hero, bool ensureOne = false) noexcept
    {
        using namespace fable::game::native;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        CharString name = {};
        int count = -1;
        bool built = false;
        __try
        {
            reinterpret_cast<void(__thiscall*)(CharString*, const char*, int)>(base + rva::CharStringConstructor)(
                &name, "OBJECT_RESURRECTION_PHIAL", -1);
            built = true;
            void* manager = *reinterpret_cast<void**>(base + rva::ThingManagerSlot);
            void* definitions = manager != nullptr ? *reinterpret_cast<void**>(static_cast<unsigned char*>(manager) + 0x2C) : nullptr;
            const int definition = definitions != nullptr ? reinterpret_cast<int(__thiscall*)(void*, CharString*)>(
                base + rva::DefinitionIndexByName)(definitions, &name) : 0;
            void* inventory = fable::game::entity::native::ThingComponentAccess::Find(hero,
                static_cast<fable::game::entity::native::ThingComponentType>(0x11));
            if (definition > 0 && inventory != nullptr)
            {
                using Count = int(__thiscall*)(void*, int);
                count = reinterpret_cast<Count>(base + 0x019F1108)(inventory, definition);
                if (ensureOne && count == 0)
                {
                    using Add = bool(__thiscall*)(void*, int, int, bool, bool, int, bool);
                    reinterpret_cast<Add>(base + 0x019F2375)(inventory, definition, 1, false, false, 0, false);
                    count = reinterpret_cast<Count>(base + 0x019F1108)(inventory, definition);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; }
        if (built)
        {
            __try { reinterpret_cast<void(__thiscall*)(CharString*)>(base + rva::CharStringDestructor)(&name); }
            __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; }
        }
        return count;
    }

    bool SetOrbAmount(void* orb, int amount) noexcept
    {
        if (orb == nullptr) return false;
        __try { *reinterpret_cast<int*>(static_cast<unsigned char*>(orb) + 0x10) = amount; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int ReadOrbAmount(void* orb) noexcept
    {
        if (orb == nullptr) return -1;
        __try { return *reinterpret_cast<int*>(static_cast<unsigned char*>(orb) + 0x10); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    bool DeathCardOpen(void* hero) noexcept
    {
        auto* card = static_cast<unsigned char*>(fable::game::entity::native::ThingComponentAccess::Find(hero,
            static_cast<fable::game::entity::native::ThingComponentType>(0xFC)));
        if (card == nullptr) return false;
        __try { return card[0xD] != 0 && *reinterpret_cast<int*>(card + 0x24) == 3; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }

    bool ModalTutorialOpen() noexcept
    {
        __try
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            auto* hud = *reinterpret_cast<unsigned char**>(base + 0x0322B3CC);
            auto* manager = hud != nullptr ? hud + 0x18 : nullptr;
            return manager != nullptr && manager[0xED] != 0 &&
                (*reinterpret_cast<std::uint32_t*>(manager + 0x6C) & 0x20000u) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }

    bool AcceptQuestCard(void* const hero) noexcept
    {
        return fable::multiplayer::persistence::QuestScreenHook::ConfirmLocalCard(hero);
    }

    bool IsWaspName(const std::string& name) noexcept
    {
        return (name.find("Hornet") != std::string::npos || name.find("Wasp") != std::string::npos) &&
            name.find("Attacker") == std::string::npos && name.find("Victim") == std::string::npos && name.find("Woman") == std::string::npos &&
            name.find("Queen") == std::string::npos && name.find("Pos") == std::string::npos;
    }
}

namespace fable::automation::multiplayer::features
{
    void CutsceneAcceptanceDriver::Initialize(
        const bool enabled,
        const bool host,
        game::EntityService& entities,
        game::PlayerService& players,
        ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        enabled_ = enabled;
        if (!enabled_) return;
        host_ = host;
        entities_ = &entities;
        players_ = &players;
        multiplayer_ = &multiplayer;
        diagnostics_ = diagnostics;
        picnicMapId_ = PicnicAreaMapId;
        lookoutMapId_ = LookoutPointMapId;
        diagnostics_.Event("CutsceneAcceptanceArmed", host_ ? "role=host" : "role=guest");
    }

    void CutsceneAcceptanceDriver::Shutdown() noexcept
    {
        enabled_ = false;
        entities_ = nullptr;
        multiplayer_ = nullptr;
        hostVisits_ = 0;
        swarmUids_.clear();
        swarmSelected_ = false;
        lastHostMap_ = 0;
        theresaHiddenSeen_=theresaOverlapPlaced_=false;
        theresaSceneEndedAt_=0;
        phase_.store(Phase::WaitingForWorld, std::memory_order_release);
    }

    void CutsceneAcceptanceDriver::Enter(const Phase phase, const std::uint64_t now) noexcept
    {
        phase_.store(phase, std::memory_order_release);
        phaseStartedAt_ = now;
        nextTravelAt_ = 0;
        travelAttempts_ = 0;
    }

    void CutsceneAcceptanceDriver::Fail(const char* const reason) noexcept
    {
        diagnostics_.Event("CutsceneAcceptanceFailed", reason);
        phase_.store(Phase::Failed, std::memory_order_release);
    }

    std::uint16_t CutsceneAcceptanceDriver::LocalMapId() const noexcept
    {
        const auto* const state = multiplayer_->Contexts().players.localHero.CurrentState();
        return state != nullptr ? state->mapId : 0;
    }

    bool CutsceneAcceptanceDriver::LocalMapIs(const char* const mapName) const noexcept
    {
        const auto& localHero = multiplayer_->Contexts().players.localHero;
        const auto* const state = localHero.CurrentState();
        return state != nullptr && localHero.IsWorldReady() && state->mapName == mapName;
    }

    void CutsceneAcceptanceDriver::Tick(const bool remotePresentationReady) noexcept
    {
        if (!enabled_ || multiplayer_ == nullptr ||
            phase_.load(std::memory_order_acquire) != Phase::WaitingForWorld)
        {
            return;
        }
        const auto& contexts = multiplayer_->Contexts();
        if (remotePresentationReady && contexts.players.localHero.IsWorldReady() &&
            contexts.players.localHero.CurrentState() != nullptr &&
            contexts.players.remotePlayers.ActiveCount() != 0)
        {
            Enter(Phase::Settle, GetTickCount64());
        }
    }

    bool CutsceneAcceptanceDriver::TeleportLocalHero(const float (&position)[3], const char* const reason) noexcept
    {
        game::Entity* const hero = entities_->GetHero();
        if (hero == nullptr) return false;
        const bool moved = hero->IsValid() &&
            hero->Teleport(game::Vector3{position[0], position[1], position[2]}, hero->GetFacing(), false);
        hero->Release();
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "reason=%s position=(%.1f,%.1f,%.1f) moved=%d",
            reason, position[0], position[1], position[2], moved ? 1 : 0);
        diagnostics_.Event("CutsceneAcceptanceTeleport", detail);
        return moved;
    }

    bool CutsceneAcceptanceDriver::FindCorpseSwarm(const float (&nearby)[3], float (&position)[3]) noexcept
    {
        const auto* local = multiplayer_->Contexts().players.localHero.CurrentState();
        if (local == nullptr) return false;
        struct Point { std::uint64_t uid; game::Vector3 at; };
        std::vector<Point> wasps;
        std::vector<game::Vector3> corpses;
        for (const auto& record : multiplayer_->Contexts().entities.entityPresence.LiveEntities().Snapshot())
        {
            if (record.thing == nullptr || record.mapId != local->mapId) continue;
            const auto nativeUid = multiplayer_->Contexts().entities.entityIdentities.FindLocal(record.thingUid);
            game::Entity* entity = entities_->FindByUid(nativeUid != 0 ? nativeUid : record.thingUid);
            if (entity == nullptr) continue;
            const bool valid = entity->IsValid();
            const bool dead = valid && entity->IsDead();
            const auto at = valid ? entity->GetPosition() : game::Vector3{};
            const auto definition = valid && !record.creature ? entity->GetDefinitionName() : std::string{};
            entity->Release();
            if (!valid || !std::isfinite(at.x) || !std::isfinite(at.y)) continue;
            if (record.creature && !dead && IsWaspName(record.scriptName)) wasps.push_back({record.thingUid, at});
            else if ((dead && !IsWaspName(record.scriptName)) ||
                definition.find("CORPSE") != std::string::npos || definition.find("DEAD_BODY") != std::string::npos ||
                record.scriptName.find("_DEAD_CREATURE") != std::string::npos)
                corpses.push_back(at);
        }
        float best = 1.0e30f;
        std::vector<std::uint64_t> selected;
        for (const auto& corpse : corpses)
        {
            std::vector<std::uint64_t> members;
            game::Vector3 center{};
            for (const auto& wasp : wasps)
            {
                const float dx = wasp.at.x - corpse.x, dy = wasp.at.y - corpse.y;
                if (dx * dx + dy * dy > 144.0f) continue;
                members.push_back(wasp.uid);
                center.x += wasp.at.x; center.y += wasp.at.y;
            }
            if (members.size() < 2) continue;
            center.x /= static_cast<float>(members.size()); center.y /= static_cast<float>(members.size());
            const float dx = center.x - nearby[0], dy = center.y - nearby[1];
            const float distance = dx * dx + dy * dy;
            if (distance >= best) continue;
            best = distance;
            selected = std::move(members);
            position[0] = center.x; position[1] = center.y; position[2] = local->position.z;
        }
        if (selected.empty())
        {
            static std::uint64_t reportAt = 0;
            const auto now = GetTickCount64();
            if (now >= reportAt)
            {
                reportAt = now + 5000;
                char detail[128] = {};
                std::snprintf(detail, sizeof(detail), "corpses=%zu eligible_wasps=%zu required_group=2 radius=12", corpses.size(), wasps.size());
                diagnostics_.Event("EnemyTestCorpseSwarmUnavailable", detail);
            }
            return false;
        }
        swarmUids_ = std::move(selected);
        std::memcpy(swarmCenter_, position, sizeof(swarmCenter_));
        swarmSelected_ = true;
        char detail[128] = {};
        std::snprintf(detail, sizeof(detail), "members=%zu center=(%.1f,%.1f,%.1f) excludes=WaspAttacker,WaspVictim",
            swarmUids_.size(), position[0], position[1], position[2]);
        diagnostics_.Event("EnemyTestCorpseSwarmSelected", detail);
        return true;
    }

    bool CutsceneAcceptanceDriver::FindWasp(std::uint64_t& uid, float (&position)[3]) noexcept
    {
        const auto* local = multiplayer_->Contexts().players.localHero.CurrentState();
        if (local == nullptr) return false;
        const float at[3] = {local->position.x, local->position.y, local->position.z};
        if (!FindCorpseSwarm(at, position)) return false;
        uid = swarmUids_.front(); // diagnostic only, never a teleport target
        return true;
    }

    bool CutsceneAcceptanceDriver::WaspPosition(float (&position)[3]) noexcept
    {
        if (!swarmSelected_) return false;
        std::memcpy(position, swarmCenter_, sizeof(swarmCenter_));
        return true;
    }

    bool CutsceneAcceptanceDriver::FindWaspNear(const float (&nearby)[3], std::uint64_t& uid, float (&position)[3]) noexcept
    {
        if (!FindCorpseSwarm(nearby, position)) return false;
        uid = swarmUids_.front();
        return true;
    }

    bool CutsceneAcceptanceDriver::SwarmDamagedPlayer(std::uint64_t actor) const noexcept
    {
        for (const auto uid : swarmUids_)
            if (multiplayer_->Contexts().actions.combatHits.WasDamagedByWorldEntity(uid, actor, phaseStartedAt_)) return true;
        return false;
    }

    bool CutsceneAcceptanceDriver::LocalHeroWasHit() noexcept
    {
        const float health = players_->GetHealth();
        if (!(health < healthBaseline_ - 0.01f)) return false;
        // Keep the test alive: restore what the hit took.
        players_->SetHealth(players_->GetMaximumHealth());
        healthBaseline_ = players_->GetHealth();
        const auto* local = multiplayer_->Contexts().players.localHero.CurrentState();
        return local != nullptr && SwarmDamagedPlayer(local->actorId);
    }

    float CutsceneAcceptanceDriver::DistanceToRemote() const noexcept
    {
        std::uint64_t actor = 0;
        float remote[3] = {};
        const auto* const local = multiplayer_->Contexts().players.localHero.CurrentState();
        if (local == nullptr || !RemoteHost(actor, remote)) return -1.0f;
        const float dx = remote[0] - local->position.x;
        const float dy = remote[1] - local->position.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    bool CutsceneAcceptanceDriver::RemoteHost(std::uint64_t& actorId, float (&position)[3]) const noexcept
    {
        for (const auto& remote : multiplayer_->Contexts().transport.remotePlayerChannels.Snapshots())
        {
            actorId = remote.state.actorId;
            position[0] = remote.state.position.x;
            position[1] = remote.state.position.y;
            position[2] = remote.state.position.z;
            return actorId != 0;
        }
        return false;
    }

    bool CutsceneAcceptanceDriver::TravelTo(const char* const mapName, const std::uint64_t now) noexcept
    {
        if (LocalMapIs(mapName)) return true;
        if (now < nextTravelAt_) return false;
        nextTravelAt_ = now + TravelRetryMilliseconds;
        const std::uint16_t source = LocalMapId();
        const std::uint16_t destination = std::strcmp(mapName, PicnicArea) == 0 ? picnicMapId_ : lookoutMapId_;
        if (source == 0 || source == destination) return false;
        const HMODULE module = entities_->GameModule();
        bool heroReady = false;
        std::uint16_t heroMap = 0;
        std::uint32_t requestState = 0;
        if (!game::world::travel::native::WorldTravelFunctions::ReadRegionExitHeroState(module, heroReady, heroMap) ||
            !game::world::travel::native::WorldTravelFunctions::ReadRegionTravelRequestState(module, requestState) ||
            !heroReady || (requestState != 0 && requestState < 10))
        {
            return false;
        }
        transition::native_route::Descriptor route;
        if (!transition::native_route::SelectFirst(*entities_, *multiplayer_, source, route, destination))
        {
            if (++travelAttempts_ >= TravelAttemptLimit) Fail("no-region-exit-to-destination");
            return false;
        }
        if (++travelAttempts_ >= TravelAttemptLimit)
        {
            Fail("travel-not-accepted");
            return false;
        }
        game::Entity* const hero = entities_->GetHero();
        if (hero == nullptr) return false;
        const bool invoked = hero->IsValid() && hero->Teleport(route.destinationPosition, route.destinationFacing, false);
        hero->Release();
        char detail[128] = {};
        std::snprintf(detail, sizeof(detail), "to=%s source_map_id=%u destination_map_id=%u invoked=%d attempt=%u",
            mapName, static_cast<unsigned>(source), static_cast<unsigned>(destination), invoked ? 1 : 0, travelAttempts_);
        diagnostics_.Event("CutsceneAcceptanceTravel", detail);
        // A started transition unsettles the world; wait before retrying.
        nextTravelAt_ = now + TravelRetryMilliseconds * 5;
        return false;
    }

    bool CutsceneAcceptanceDriver::ProcessGameThreadIdle() noexcept
    {
        const Phase phase = phase_.load(std::memory_order_acquire);
        if (!enabled_ || multiplayer_ == nullptr || entities_ == nullptr || phase == Phase::WaitingForWorld ||
            phase == Phase::Done || phase == Phase::Failed)
        {
            return false;
        }
        const std::uint64_t now = GetTickCount64();
        if (phase != Phase::Settle && phase != Phase::GuestWatchHost && phase != Phase::GuestAwaitHostArrival &&
            now - phaseStartedAt_ > PhaseTimeoutMilliseconds)
        {
            char reason[64] = {};
            std::snprintf(reason, sizeof(reason), "phase-%u-timeout", static_cast<unsigned>(phase));
            Fail(reason);
            return false;
        }
        auto& contexts = multiplayer_->Contexts();
        auto& cutsceneSync = contexts.world.cutsceneSync;
        if (!host_ && phase >= Phase::AwaitCutsceneEnd)
        {
            bool found = false;
            for (const auto& record : contexts.entities.entityLifecycle.Directory().Snapshot())
            {
                if (record.lootExperience != 20) continue;
                const auto* live = contexts.entities.entityPresence.LiveEntities().Find(record.thingUid);
                void* orb = live != nullptr ? game::entity::native::ThingComponentAccess::Find(live->thing,
                    static_cast<game::entity::native::ThingComponentType>(0x69)) : nullptr;
                found = record.live && orb != nullptr;
                if (!orbReplicaSeen_ && found && record.lootExperience == 20 && ReadOrbAmount(orb) == 20)
                {
                    orbReplicaSeen_ = true;
                    diagnostics_.Event("ExperienceOrbReplicaReady", "canonical native orb amount20 visible on guest");
                }
            }
            if (orbReplicaSeen_ && !found && !orbReplicaRemoved_)
            {
                orbReplicaRemoved_ = true;
                diagnostics_.Event("ExperienceOrbReplicaRemoved", "host collection removed the guest orb");
            }
        }
        // The quest start card that follows the cutscene pauses the world
        // until the player accepts it; accept it like Enter would.
        if (!host_ && phase >= Phase::AwaitCutsceneEnd && phase < Phase::Done &&
            AcceptQuestCard(contexts.players.localHero.NativeHero()))
        {
            diagnostics_.Event("CutsceneAcceptanceQuestCardAccepted", host_ ? "role=host" : "role=guest");
        }
        switch (phase)
        {
        case Phase::Settle:
            if (now - phaseStartedAt_ < SettleMilliseconds) return false;
            if (!LocalMapIs(LookoutPoint))
            {
                Fail("fixture-not-at-lookout-point");
                return false;
            }
            Enter(host_ ? Phase::HostEnter : Phase::GuestWatchHost, now);
            return false;

        case Phase::HostEnter:
            if (!TravelTo(PicnicArea, now)) return false;
            diagnostics_.Event("CutsceneAcceptanceHostArrived", "visit=1");
            promptSeenAt_ = 0;
            Enter(Phase::HostAwaitPrompt, now);
            return true;

        case Phase::HostAwaitPrompt:
            if (!cutsceneSync.IsWaitPromptShowingForTest())
            {
                promptSeenAt_ = 0;
                return false;
            }
            if (promptSeenAt_ == 0) promptSeenAt_ = now;
            if (now - promptSeenAt_ < PromptShownMilliseconds) return false;
            // Exactly what the player's click does: the retail close path.
            if (!cutsceneSync.DismissWaitPromptForTest())
            {
                Fail("prompt-dismiss-failed");
                return false;
            }
            diagnostics_.Event("CutsceneAcceptanceHostDismissed", "click-equivalent close of the waiting box");
            Enter(Phase::HostAwaitRetreat, now);
            return true;

        case Phase::HostAwaitRetreat:
            if (!LocalMapIs(LookoutPoint)) return false;
            diagnostics_.Event("CutsceneAcceptanceHostRetreated", "back at Lookout Point");
            Enter(Phase::HostReenter, now);
            return true;

        case Phase::HostReenter:
            if (now - phaseStartedAt_ < SettleMilliseconds) return false;
            if (!TravelTo(PicnicArea, now)) return false;
            diagnostics_.Event("CutsceneAcceptanceHostArrived", "visit=2");
            sawCutscene_ = false;
            Enter(Phase::AwaitCutsceneEnd, now);
            return true;

        case Phase::GuestWatchHost:
        {
            std::uint16_t hostMap = 0;
            for (const auto& remote : contexts.transport.remotePlayerChannels.Snapshots())
            {
                hostMap = remote.state.mapId;
            }
            if (hostMap != 0 && hostMap != lastHostMap_)
            {
                if ((hostVisits_ == 0 && hostMap == picnicMapId_) ||
                    (hostVisits_ == 1 && hostMap == lookoutMapId_) ||
                    (hostVisits_ == 2 && hostMap == picnicMapId_))
                {
                    ++hostVisits_;
                    char detail[64] = {};
                    std::snprintf(detail, sizeof(detail), "host_visit=%d map_id=%u", hostVisits_, static_cast<unsigned>(hostMap));
                    diagnostics_.Event("CutsceneAcceptanceGuestSawHost", detail);
                }
                lastHostMap_ = hostMap;
            }
            if (hostVisits_ < 3)
            {
                if (now - phaseStartedAt_ > PhaseTimeoutMilliseconds * 3) Fail("host-never-returned-to-picnic-area");
                return false;
            }
            Enter(Phase::GuestEnter, now);
            return false;
        }

        case Phase::GuestEnter:
            if (now - phaseStartedAt_ < 3'000) return false;
            if (!TravelTo(PicnicArea, now)) return false;
            diagnostics_.Event("CutsceneAcceptanceGuestArrived", "joined the host at the Picnic Area");
            sawCutscene_ = false;
            Enter(Phase::AwaitCutsceneEnd, now);
            return true;

        case Phase::AwaitCutsceneEnd:
        {
            const bool inCutscene = contexts.world.cutscenes.IsInCutscene();
            const bool theresaScene = GetEnvironmentVariableW(L"ALBIONTOGETHER_THERESA_SCENE_TEST", nullptr, 0)!=0;
            void* theresa = host_?nullptr:contexts.players.localHero.NativeHero();
            if (theresaScene && host_)
                for (const auto& peer:contexts.transport.remotePlayerChannels.Snapshots())
                    if (peer.state.role==::fable::multiplayer::PeerRole::Guest)
                        theresa=contexts.players.playerCombatants.FindCreature(peer.state.actorId);
            if (theresaScene && inCutscene && !theresaHiddenSeen_ &&
                game::hero_pawn::appearance::hooks::TheresaHeroSkin::InspectForTest(theresa,true))
            {
                theresaHiddenSeen_=true;
                diagnostics_.Event("TheresaSceneHiddenVerified","actual Sister primitive hidden; native Hero animation driver remains ticking");
            }
            if (inCutscene) sawCutscene_ = true;
            if (!host_ && inCutscene && !skipRequestedForTest_ && now - phaseStartedAt_ > 6000 &&
                contexts.world.cutscenes.RequestSkip("CS_WASPBOSS_INTRO", true))
            {
                skipRequestedForTest_ = true;
                diagnostics_.Event("CutsceneAcceptanceGuestSkipRequested", "native input event39 requested");
            }
            if (!sawCutscene_ || inCutscene) return false;
            if (theresaScene)
            {
                if (!theresaHiddenSeen_) { Fail("theresa-not-hidden-during-native-scene"); return false; }
                if (!theresaSceneEndedAt_) theresaSceneEndedAt_=now;
                if (!host_ && !theresaOverlapPlaced_)
                {
                    std::uint64_t actor=0; float position[3]={};
                    if (!RemoteHost(actor,position) || !TeleportLocalHero(position,"theresa-overlap-after-scene")) return false;
                    theresaOverlapPlaced_=true;
                    return false;
                }
                if (now-theresaSceneEndedAt_<1500 ||
                    !game::hero_pawn::appearance::hooks::TheresaHeroSkin::InspectForTest(theresa,false)) return false;
                if (!host_ && DistanceToRemote()<1.6f) return false;
                diagnostics_.Event("TheresaSceneRestoredVerified",host_?"actual remote Sister visible after scene":"actual local Sister visible; deliberately overlapping guest separated by runtime after scene");
                Enter(Phase::Done,now);
                return false;
            }
            if (now - phaseStartedAt_ < SettleMilliseconds) return false;
            diagnostics_.Event("CutsceneAcceptanceCutsceneOver", host_ ? "role=host" : "role=guest");
            players_->SetHealth(players_->GetMaximumHealth());
            healthBaseline_ = players_->GetHealth();
            hostFightSeen_ = false;
            Enter(host_ ? Phase::HostToWasp : Phase::GuestAwaitHostArrival, now);
            return false;
        }

        case Phase::HostToWasp:
        {
            if (orbProbeUid_ == 0)
            {
                const auto* state = contexts.players.localHero.CurrentState();
                if (state == nullptr) return false;
                auto position = state->position;
                position.x += 10;
                game::Entity* orb = entities_->CreateExperienceOrb(position, 20);
                if (orb == nullptr || !orb->IsValid()) { if (orb) orb->Release(); Fail("native-orb-probe-spawn-failed"); return false; }
                void* component = game::entity::native::ThingComponentAccess::Find(entities_->ResolveNative(orb->NativeHandle()),
                    static_cast<game::entity::native::ThingComponentType>(0x69));
                const bool assigned = SetOrbAmount(component, 20);
                orbProbeUid_ = orb->GetUid();
                orb->Release();
                if (!assigned) { Fail("native-orb-probe-amount-failed"); return false; }
                orbCollectAt_ = now + 4000;
                diagnostics_.Event("ExperienceOrbProbeSpawned", "native orb amount20 created away from Hero");
                return true;
            }
            if (now < orbCollectAt_) return false;
            if (orbCollectAt_ != 0)
            {
                if (!contexts.actions.killExperience.CollectOrbForAcceptance(orbProbeUid_)) return false;
                orbCollectAt_ = 0;
            }
            float position[3] = {};
            if (!FindWasp(waspUid_, position))
            {
                if (now - phaseStartedAt_ > 30'000) Fail("no-corpse-swarm-with-two-living-wasps");
                return false;
            }
            TeleportLocalHero(position, "host-to-corpse-swarm");
            nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "wasp_uid=%016llX", static_cast<unsigned long long>(waspUid_));
            diagnostics_.Event("EnemyTestHostNearWasp", detail);
            Enter(Phase::HostAwaitWaspAttack, now);
            return true;
        }

        case Phase::HostAwaitWaspAttack:
        {
            const auto* const local = contexts.players.localHero.CurrentState();
            if (local != nullptr &&
                SwarmDamagedPlayer(local->actorId) &&
                LocalHeroWasHit())
            {
                char detail[96] = {};
                std::snprintf(detail, sizeof(detail), "target=host players_apart=%.1f", DistanceToRemote());
                diagnostics_.Event("EnemyTestWaspAttacked", detail);
                std::uint64_t guestActor = 0;
                if (!RemoteHost(guestActor, retreatAt_)) { Fail("guest-position-unknown"); return false; }
                retreatAt_[0] += 2.0f;
                Enter(Phase::HostAwaitGuestReady, now);
                return true;
            }
            // Wasps fly about; stay at the wasp until it hits.
            if (now >= nextWaspTeleportAt_)
            {
                float position[3] = {};
                if (WaspPosition(position)) TeleportLocalHero(position, "host-holds-swarm-center");
                nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            }
            return false;
        }

        case Phase::HostAwaitGuestReady:
        {
            if (DistanceToRemote() < 0 || DistanceToRemote() > NearDistance) return false;
            const auto* local = contexts.players.localHero.CurrentState();
            std::uint64_t guest = 0; float at[3] = {};
            if (local == nullptr || !RemoteHost(guest, at)) return false;
            if (contexts.actions.enemyTargets.NativeTargetCount(local->actorId) == 0 ||
                contexts.actions.enemyTargets.NativeTargetCount(guest) == 0) { balancedSince_ = 0; return false; }
            if (balancedSince_ == 0) balancedSince_ = now;
            if (now - balancedSince_ < 1000) return false;
            diagnostics_.Event("EnemyTestSwarmDistributed", "native enemy getters target both living players concurrently");
            TeleportLocalHero(retreatAt_, "host-leaves-occupied-swarm");
            diagnostics_.Event("EnemyTestHostToGuestSpot", "guest already present; host retreats");
            Enter(Phase::HostAwaitGuestAttacked, now);
            return true;
        }

        case Phase::HostAwaitGuestAttacked:
        {
            std::uint64_t guestActor = 0;
            float guestAt[3] = {};
            if (!RemoteHost(guestActor, guestAt)) return false;
            if (SwarmDamagedPlayer(guestActor))
            {
                char detail[96] = {};
                std::snprintf(detail, sizeof(detail), "target=guest players_apart=%.1f", DistanceToRemote());
                diagnostics_.Event("EnemyTestWaspAttacked", detail);
                Enter(Phase::HostAwaitGuestArrival, now);
                return true;
            }
            return false;
        }

        case Phase::HostAwaitGuestArrival:
        {
            std::uint64_t guestActor = 0;
            float guestAt[3] = {};
            if (!RemoteHost(guestActor, guestAt)) return false;
            TeleportLocalHero(guestAt, "host-returns-before-guest-leaves");
            diagnostics_.Event("EnemyTestHostBackToWasp", "host joins the guest before the second handoff");
            Enter(Phase::HostAwaitGuestDeparture, now);
            return true;
        }
        case Phase::HostAwaitGuestDeparture:
            if (DistanceToRemote() <= 2.0f * NearDistance) return false;
            players_->SetHealth(players_->GetMaximumHealth());
            healthBaseline_ = players_->GetHealth();
            nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            Enter(Phase::HostAwaitWaspAttackAgain, now);
            return true;

        case Phase::HostAwaitWaspAttackAgain:
        {
            const auto* const local = contexts.players.localHero.CurrentState();
            // Only a hit after the host came back counts, not the first one.
            if (local != nullptr && now - phaseStartedAt_ >= 500 &&
                SwarmDamagedPlayer(local->actorId) &&
                LocalHeroWasHit())
            {
                char detail[96] = {};
                std::snprintf(detail, sizeof(detail), "target=host-again players_apart=%.1f", DistanceToRemote());
                diagnostics_.Event("EnemyTestWaspAttacked", detail);
                Enter(Phase::Done, now);
                return true;
            }
            if (now >= nextWaspTeleportAt_)
            {
                float position[3] = {};
                if (WaspPosition(position)) TeleportLocalHero(position, "host-holds-swarm-center");
                nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            }
            return false;
        }

        case Phase::GuestAwaitHostArrival:
        {
            // The host fights the wasp away from here, then takes this spot.
            std::uint64_t hostActor = 0;
            float hostAt[3] = {};
            if (!RemoteHost(hostActor, hostAt)) return false;
            const float apart = DistanceToRemote();
            if (apart <= 2.0f * NearDistance) return false;
            if (!swarmSelected_)
            {
                float selected[3] = {};
                if (!FindWaspNear(hostAt, waspUid_, selected)) return false;
            }
            if (!SwarmDamagedPlayer(hostActor)) return false;
            for (int axis = 0; axis < 3; ++axis) hostFightAt_[axis] = hostAt[axis];
            float position[3] = {};
            if (!FindWaspNear(hostFightAt_, waspUid_, position))
            {
                Fail("guest-sees-no-wasp");
                return false;
            }
            TeleportLocalHero(hostAt, "guest-joins-occupied-corpse-swarm");
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "wasp_uid=%016llX", static_cast<unsigned long long>(waspUid_));
            diagnostics_.Event("EnemyTestGuestNearWasp", detail);
            players_->SetHealth(players_->GetMaximumHealth());
            healthBaseline_ = players_->GetHealth();
            nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            Enter(Phase::GuestAwaitHostRetreat, now);
            return true;
        }

        case Phase::GuestAwaitHostRetreat:
        {
            if (DistanceToRemote() <= 2.0f * NearDistance) return false;
            std::uint64_t actor = 0;
            if (!RemoteHost(actor, retreatAt_)) return false;
            players_->SetHealth(players_->GetMaximumHealth());
            healthBaseline_ = players_->GetHealth();
            Enter(Phase::GuestAwaitHit, now);
            return true;
        }

        case Phase::GuestAwaitHit:
        {
            if (!LocalHeroWasHit())
            {
                // Wasps fly about; stay at the wasp until it hits.
                if (now >= nextWaspTeleportAt_)
                {
                    float position[3] = {};
                    if (WaspPosition(position)) TeleportLocalHero(position, "guest-holds-swarm-center");
                    nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
                }
                return false;
            }
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "players_apart=%.1f", DistanceToRemote());
            diagnostics_.Event("EnemyTestGuestHurt", detail);
            std::uint64_t hostActor = 0;
            float hostAt[3] = {};
            if (!RemoteHost(hostActor, hostAt))
            {
                Fail("host-position-unknown");
                return false;
            }
            Enter(Phase::GuestAwaitHostReturn, now);
            return true;
        }

        case Phase::GuestAwaitHostReturn:
            if (DistanceToRemote() < 0 || DistanceToRemote() > NearDistance) return false;
            TeleportLocalHero(retreatAt_, "guest-leaves-occupied-swarm");
            diagnostics_.Event("EnemyTestGuestMovedToHost", "host already present; guest retreats");
            Enter(Phase::GuestFlaskProbe, now);
            return true;

        case Phase::GuestFlaskProbe:
        {
            if (now - phaseStartedAt_ < 1000) return false;
            void* hero = contexts.players.localHero.NativeHero();
            flaskBaseline_ = FlaskCount(hero, true);
            if (flaskBaseline_ <= 0) { Fail("flask-probe-inventory-unavailable"); return false; }
            if (!contexts.actions.playerDeath.IsReady()) { Fail("native-death-hook-unavailable"); return false; }
            // A real lethal owned-health mutation; the normal native death
            // coordinator/retail path must consume the item and recover.
            if (!contexts.actions.playerDeath.DamageForAcceptance(hero, players_->GetMaximumHealth() * 2))
            { Fail("flask-probe-damage-rejected"); return false; }
            Enter(Phase::GuestAwaitFlaskRecovery, now);
            return true;
        }
        case Phase::GuestAwaitFlaskRecovery:
        {
            void* hero = contexts.players.localHero.NativeHero();
            if (DeathCardOpen(hero) || ModalTutorialOpen()) { Fail("flask-showed-modal-death-card-or-tutorial"); return false; }
            if (players_->GetHealth() <= 0.01f) return false;
            const int remaining = FlaskCount(hero);
            if (remaining != flaskBaseline_ - 1) { Fail("flask-not-consumed-exactly-once"); return false; }
            if (now - phaseStartedAt_ < 6000) return false;
            diagnostics_.Event("ResurrectionFlaskAcceptancePassed", "one native flask consumed; positive health; no modal death card");
            Enter(Phase::Done, now);
            return true;
        }

        default:
            return false;
        }
    }
}

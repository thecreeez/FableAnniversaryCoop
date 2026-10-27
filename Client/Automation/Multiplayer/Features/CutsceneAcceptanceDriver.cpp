#include "CutsceneAcceptanceDriver.h"

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

namespace
{
    // Native map ids of the wasp fixture's two regions (the region exit
    // descriptors and PlayerState use the same ids).
    constexpr std::uint16_t LookoutPointMapId = 1;
    constexpr std::uint16_t PicnicAreaMapId = 2;
    constexpr const char* LookoutPoint = "LookoutPoint";
    constexpr const char* PicnicArea = "PicnicArea";
    constexpr std::uint64_t EngagementWindowMilliseconds = 8'000;
    constexpr std::uint64_t WaspTeleportMilliseconds = 6'000;
    constexpr float NearDistance = 8.0f;

    // CTCQuestCompletionUI (Hero component 0xFC): +0x0D card shown, +0x20
    // accepted. The card's Enter/accept sets the latter and the component's
    // next update closes the card and unpauses the world, as retail does.
    constexpr std::int32_t QuestCardComponent = 0xFC;

    bool AcceptQuestCard(void* const hero) noexcept
    {
        if (hero == nullptr) return false;
        auto* const card = static_cast<std::uint8_t*>(fable::game::entity::native::ThingComponentAccess::Find(
            hero, static_cast<fable::game::entity::native::ThingComponentType>(QuestCardComponent)));
        if (card == nullptr) return false;
        __try
        {
            if (card[0x0D] == 0 || card[0x20] != 0) return false;
            card[0x20] = 1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsWaspName(const std::string& name) noexcept
    {
        return (name.find("Hornet") != std::string::npos || name.find("Wasp") != std::string::npos) &&
            name.find("Victim") == std::string::npos && name.find("Woman") == std::string::npos &&
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
        lastHostMap_ = 0;
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

    bool CutsceneAcceptanceDriver::FindWasp(std::uint64_t& uid, float (&position)[3]) noexcept
    {
        auto& contexts = multiplayer_->Contexts();
        const auto* const local = contexts.players.localHero.CurrentState();
        if (local == nullptr) return false;
        bool found = false;
        float best = 0.0f;
        for (const auto& record : contexts.entities.entityPresence.LiveEntities().Snapshot())
        {
            if (!record.creature || record.thing == nullptr || record.mapId != local->mapId ||
                !IsWaspName(record.scriptName) || !contexts.world.entitySimulation.IsLocallySimulated(record.thing))
            {
                continue;
            }
            game::Entity* const entity = entities_->FindByUid(record.thingUid);
            if (entity == nullptr) continue;
            const bool valid = entity->IsValid();
            const game::Vector3 at = valid ? entity->GetPosition() : game::Vector3{};
            entity->Release();
            if (!valid || !std::isfinite(at.x) || !std::isfinite(at.y)) continue;
            const float dx = at.x - local->position.x;
            const float dy = at.y - local->position.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (!found || distance < best)
            {
                found = true;
                best = distance;
                uid = record.thingUid;
                position[0] = at.x;
                position[1] = at.y;
                position[2] = local->position.z;
            }
        }
        return found;
    }

    bool CutsceneAcceptanceDriver::WaspPosition(float (&position)[3]) noexcept
    {
        game::Entity* const wasp = waspUid_ != 0 ? entities_->FindByUid(waspUid_) : nullptr;
        if (wasp == nullptr) return false;
        const bool valid = wasp->IsValid();
        const game::Vector3 at = valid ? wasp->GetPosition() : game::Vector3{};
        wasp->Release();
        if (!valid || !std::isfinite(at.x) || !std::isfinite(at.y)) return false;
        const auto* const local = multiplayer_->Contexts().players.localHero.CurrentState();
        position[0] = at.x;
        position[1] = at.y;
        position[2] = local != nullptr ? local->position.z : at.z;
        return true;
    }

    bool CutsceneAcceptanceDriver::FindWaspNear(
        const float (&nearby)[3], std::uint64_t& uid, float (&position)[3]) noexcept
    {
        auto& contexts = multiplayer_->Contexts();
        const auto* const local = contexts.players.localHero.CurrentState();
        if (local == nullptr) return false;
        bool found = false;
        float best = 0.0f;
        for (const auto& record : contexts.entities.entityPresence.LiveEntities().Snapshot())
        {
            if (!record.creature || record.thing == nullptr || record.mapId != local->mapId ||
                !IsWaspName(record.scriptName))
            {
                continue;
            }
            game::Entity* const entity = entities_->FindByUid(record.thingUid);
            if (entity == nullptr) continue;
            const bool valid = entity->IsValid();
            const game::Vector3 at = valid ? entity->GetPosition() : game::Vector3{};
            entity->Release();
            if (!valid || !std::isfinite(at.x) || !std::isfinite(at.y)) continue;
            const float dx = at.x - nearby[0];
            const float dy = at.y - nearby[1];
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (!found || distance < best)
            {
                found = true;
                best = distance;
                uid = record.thingUid;
                position[0] = at.x;
                position[1] = at.y;
                position[2] = local->position.z;
            }
        }
        return found;
    }

    bool CutsceneAcceptanceDriver::LocalHeroWasHit() noexcept
    {
        const float health = players_->GetHealth();
        if (!(health < healthBaseline_ - 0.01f)) return false;
        // Keep the test alive: restore what the hit took.
        players_->SetHealth(players_->GetMaximumHealth());
        healthBaseline_ = players_->GetHealth();
        return true;
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
        // The quest start card that follows the cutscene pauses the world
        // until the player accepts it; accept it like Enter would.
        if (phase >= Phase::AwaitCutsceneEnd && phase < Phase::Done &&
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
            if (inCutscene) sawCutscene_ = true;
            if (!sawCutscene_ || inCutscene) return false;
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
            float position[3] = {};
            if (!FindWasp(waspUid_, position))
            {
                if (now - phaseStartedAt_ > 30'000) Fail("no-locally-simulated-wasp");
                return false;
            }
            TeleportLocalHero(position, "host-to-wasp");
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
                contexts.actions.entityActions.IsAnyEngagedWith(local->actorId, now, EngagementWindowMilliseconds) &&
                LocalHeroWasHit())
            {
                char detail[96] = {};
                std::snprintf(detail, sizeof(detail), "target=host players_apart=%.1f", DistanceToRemote());
                diagnostics_.Event("EnemyTestWaspAttacked", detail);
                // Quest wasps are leashed to their nest, so the players swap
                // places instead of moving the wasp: the host takes the
                // guest's spot and the guest comes to the wasp.
                std::uint64_t guestActor = 0;
                float guestAt[3] = {};
                if (!RemoteHost(guestActor, guestAt))
                {
                    Fail("guest-position-unknown");
                    return false;
                }
                guestAt[0] += 2.0f;
                TeleportLocalHero(guestAt, "host-to-guest-spot");
                diagnostics_.Event("EnemyTestHostToGuestSpot", "the guest goes to the wasp");
                Enter(Phase::HostAwaitGuestAttacked, now);
                return true;
            }
            // Wasps fly about; stay at the wasp until it hits.
            if (now >= nextWaspTeleportAt_)
            {
                float position[3] = {};
                if (WaspPosition(position)) TeleportLocalHero(position, "host-follows-wasp");
                nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            }
            return false;
        }

        case Phase::HostAwaitGuestAttacked:
        {
            std::uint64_t guestActor = 0;
            float guestAt[3] = {};
            if (!RemoteHost(guestActor, guestAt)) return false;
            if (contexts.actions.entityActions.IsAnyEngagedWith(guestActor, now, EngagementWindowMilliseconds))
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
            // The guest comes to the host once it has been hit.
            const float apart = DistanceToRemote();
            if (apart < 0.0f || apart > NearDistance) return false;
            float position[3] = {};
            if (!WaspPosition(position))
            {
                Fail("wasp-lost");
                return false;
            }
            TeleportLocalHero(position, "host-back-to-wasp");
            diagnostics_.Event("EnemyTestHostBackToWasp", "players swapped");
            healthBaseline_ = players_->GetHealth();
            nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
            Enter(Phase::HostAwaitWaspAttackAgain, now);
            return true;
        }

        case Phase::HostAwaitWaspAttackAgain:
        {
            const auto* const local = contexts.players.localHero.CurrentState();
            // Only a hit after the host came back counts, not the first one.
            if (local != nullptr && now - phaseStartedAt_ >= 500 &&
                contexts.actions.entityActions.IsAnyEngagedWith(local->actorId, now, now - phaseStartedAt_) &&
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
                if (WaspPosition(position)) TeleportLocalHero(position, "host-follows-wasp");
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
            if (apart > 2.0f * NearDistance)
            {
                for (int axis = 0; axis < 3; ++axis) hostFightAt_[axis] = hostAt[axis];
                hostFightSeen_ = true;
                return false;
            }
            if (!hostFightSeen_ || apart < 0.0f || apart > NearDistance) return false;
            float position[3] = {};
            if (!FindWaspNear(hostFightAt_, waspUid_, position))
            {
                Fail("guest-sees-no-wasp");
                return false;
            }
            TeleportLocalHero(position, "guest-to-wasp");
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "wasp_uid=%016llX", static_cast<unsigned long long>(waspUid_));
            diagnostics_.Event("EnemyTestGuestNearWasp", detail);
            players_->SetHealth(players_->GetMaximumHealth());
            healthBaseline_ = players_->GetHealth();
            nextWaspTeleportAt_ = now + WaspTeleportMilliseconds;
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
                    if (WaspPosition(position)) TeleportLocalHero(position, "guest-follows-wasp");
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
            TeleportLocalHero(hostAt, "guest-to-host");
            diagnostics_.Event("EnemyTestGuestMovedToHost", "guest moved to the host");
            Enter(Phase::Done, now);
            return true;
        }

        default:
            return false;
        }
    }
}

#include "CutsceneAcceptanceDriver.h"

#include "Automation/Multiplayer/Transition/NativeRegionRoute.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/EntityService.h"
#include "Game/World/Travel/Native/WorldTravelFunctions.h"
#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"

#include <Windows.h>

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
}

namespace fable::automation::multiplayer::features
{
    void CutsceneAcceptanceDriver::Initialize(
        const bool enabled,
        const bool host,
        game::EntityService& entities,
        ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        enabled_ = enabled;
        if (!enabled_) return;
        host_ = host;
        entities_ = &entities;
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
        if (phase != Phase::Settle && phase != Phase::GuestWatchHost &&
            now - phaseStartedAt_ > PhaseTimeoutMilliseconds)
        {
            char reason[64] = {};
            std::snprintf(reason, sizeof(reason), "phase-%u-timeout", static_cast<unsigned>(phase));
            Fail(reason);
            return false;
        }
        auto& contexts = multiplayer_->Contexts();
        auto& cutsceneSync = contexts.world.cutsceneSync;
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
            Enter(Phase::Done, now);
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
            Enter(Phase::Done, now);
            return true;

        default:
            return false;
        }
    }
}

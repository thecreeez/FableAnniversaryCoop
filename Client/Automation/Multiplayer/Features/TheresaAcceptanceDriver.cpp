#include "TheresaAcceptanceDriver.h"
#include "Multiplayer/Runtime/MultiplayerRuntimeGraph.h"
#include "Game/HeroPawn/Appearance/Native/HeroClothingComponent.h"
#include "Game/HeroPawn/Appearance/Hooks/TheresaHeroSkin.h"
#include "Game/Entity/EntityService.h"
#include "Game/Entity/Entity.h"
#include "Game/Persistence/Native/AutoSaveFunction.h"
#include <Windows.h>
#include <cmath>
#include <cwchar>
#include <cstdio>

namespace fable::automation::multiplayer::features
{
    namespace appearance = game::hero_pawn::appearance;
    void TheresaAcceptanceDriver::Initialize(bool enabled, bool host, game::EntityService& entities,
        ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer, const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        wchar_t mode[32] = {};
        if (!enabled || !GetEnvironmentVariableW(L"ALBIONTOGETHER_THERESA_TEST", mode, 32)) return;
        host_=host; entities_=&entities; multiplayer_=&multiplayer; diagnostics_=diagnostics;
        checkpoint_=std::wcscmp(mode,L"checkpoint")==0;
        diagnostics_.Event("TheresaAcceptanceArmed", checkpoint_?"paired-checkpoint":"native-outfit-change");
    }
    void TheresaAcceptanceDriver::Shutdown() noexcept
    {
        multiplayer_=nullptr; entities_=nullptr; diagnostics_={}; baseline_={}; changed_={}; savedPosition_={};
        started_=next_=phaseAt_=revision_=0; phase_=0; host_=checkpoint_=false;
    }
    void TheresaAcceptanceDriver::Fail(const char* reason) noexcept
    {
        diagnostics_.Event("ClientFailed",reason); phase_=99;
    }
    bool TheresaAcceptanceDriver::ProcessGameThreadIdle() noexcept
    {
        if (!multiplayer_ || phase_>=90) return false;
        const auto now=GetTickCount64();
        if (now<next_) return false;
        next_=now+250;
        auto& c=multiplayer_->Contexts();
        if (!started_) started_=now;
        if (now-started_>150000) { Fail("theresa-acceptance-timeout"); return false; }
        if (!c.players.localHero.IsWorldReady() || c.world.checkpoints.IsBarrierActive() ||
            c.players.remotePlayers.ActiveCount()==0) return false;
        void* local=c.players.localHero.NativeHero();
        void* subject=host_?nullptr:local;
        for (const auto& peer:c.transport.remotePlayerChannels.Snapshots())
            if (host_ && peer.state.role==::fable::multiplayer::PeerRole::Guest)
                subject=c.players.playerCombatants.FindCreature(peer.state.actorId);
        if (!subject || !appearance::hooks::TheresaHeroSkin::InspectForTest(subject,false)) return false;
        if (!checkpoint_)
        {
            appearance::HeroClothingState current;
            if (!appearance::native::HeroClothingComponent::Capture(subject,current)) return false;
            if (phase_==0)
            {
                baseline_=current; phase_=1; phaseAt_=now;
                changed_=baseline_;
                bool found=false;
                for (auto& id:changed_.definitionIndices) if (id>0) { id=-1; found=true; break; }
                if (!found) { Fail("theresa-fixture-has-no-equipped-garment"); return false; }
                diagnostics_.Event("TheresaOutfitBaseline",host_?"host actual remote inventory and render component":"guest actual local inventory and render component");
            }
            else if (phase_==1)
            {
                if (!host_ && now-phaseAt_>=10000)
                {
                    if (!appearance::native::HeroClothingComponent::Apply(local,changed_)) return false;
                    phase_=2; phaseAt_=now;
                }
                else if (host_ && current.Equals(changed_))
                {
                    phase_=2; phaseAt_=now;
                    diagnostics_.Event("TheresaOutfitChangedVerified","actual remote selected clothing changed; Sister remains bound and visible; hidden driver animates");
                }
            }
            else if (phase_==2)
            {
                if (!host_ && current.Equals(changed_) && now-phaseAt_>=10000)
                {
                    diagnostics_.Event("TheresaOutfitChangedVerified","actual local selected clothing changed; Sister remains bound and visible");
                    if (!appearance::native::HeroClothingComponent::Apply(local,baseline_)) return false;
                    phase_=3;
                }
                else if (host_ && current.Equals(baseline_))
                {
                    diagnostics_.Event("TheresaOutfitRestoredVerified","actual remote outfit restored after a second network update; skin binding valid"); phase_=90;
                }
            }
            else if (phase_==3 && current.Equals(baseline_))
            {
                diagnostics_.Event("TheresaOutfitRestoredVerified","actual local outfit restored; skin binding valid"); phase_=90;
            }
            return false;
        }
        // Checkpoint mode records this player's transform, moves both players
        // away after a new paired commit, and requests the real host selector.
        game::Entity* hero=entities_->GetHero();
        if (!hero) return false;
        const auto position=hero->GetPosition();
        if (phase_==0)
        {
            revision_=c.world.checkpoints.RevisionForTest(); phase_=1; phaseAt_=now;
            if (!host_)
            {
                auto target=position;
                for (const auto& peer:c.transport.remotePlayerChannels.Snapshots())
                    if (peer.state.role==::fable::multiplayer::PeerRole::Host)
                    { target.x=peer.state.position.x+4.0f; target.y=peer.state.position.y; }
                if (!hero->Teleport(target,hero->GetFacing(),false)) { hero->Release(); return false; }
            }
        }
        else if (phase_==1 && now-phaseAt_>=5000)
        {
            savedPosition_=position; revision_=c.world.checkpoints.RevisionForTest(); phase_=2; phaseAt_=now;
            char detail[160]={};
            std::snprintf(detail,sizeof(detail),"role=%s position=(%.3f,%.3f,%.3f)",host_?"host":"guest",position.x,position.y,position.z);
            diagnostics_.Event("TheresaCheckpointPositionRecorded",detail);
        }
        else if (phase_==2 && host_ && now-phaseAt_>=8000 && c.world.checkpoints.RevisionForTest()<=revision_)
        {
            if (game::persistence::native::AutoSaveFunction::Invoke(*entities_)) phase_=3;
        }
        else if ((phase_==2 || phase_==3) && c.world.checkpoints.RevisionForTest()>revision_)
        {
            auto away=position; away.x+=6.0f;
            if (hero->Teleport(away,hero->GetFacing(),false)) { phase_=4; phaseAt_=now; }
        }
        else if (phase_==4 && now-phaseAt_>=8000)
        {
            if (!host_ || c.world.checkpoints.RequestReloadForTest()) phase_=5;
        }
        else if (phase_==5 && c.world.checkpoints.LoadedAttemptForTest()!=0)
        { phase_=6; phaseAt_=now; }
        else if (phase_==6 && now-phaseAt_>=5000)
        {
            if (position.HorizontalDistanceTo(savedPosition_)>0.75f || std::fabs(position.z-savedPosition_.z)>1.5f)
            { hero->Release(); Fail("theresa-checkpoint-restored-wrong-player-position"); return false; }
            unsigned ghosts=0;
            for (const auto& record:c.entities.entityPresence.LiveEntities().Snapshot())
            {
                if (record.scriptName!="SCRIPT_NAME_ALBION_TOGETHER_REMOTE_PLAYER" || record.thing==local) continue;
                bool live=false;
                for (const auto& peer:c.transport.remotePlayerChannels.Snapshots())
                    if (record.thing==c.players.playerCombatants.FindCreature(peer.state.actorId)) live=true;
                if (!live) ++ghosts;
            }
            if (!ghosts)
            { diagnostics_.Event("TheresaCheckpointRestoredVerified","own pre-save position restored; live Sister driver valid; no orphan marked remote-player bandit"); phase_=90; }
        }
        hero->Release();
        return false;
    }
}

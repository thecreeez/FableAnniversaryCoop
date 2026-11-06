#include "TheresaHeroSkin.h"
#include "TheresaAppearanceHook.h"
#include "Game/HeroPawn/Appearance/Native/HeroMorphComponent.h"

#include <cstdio>
#include <cstring>

namespace
{
    // Native additional skeletal-mesh command used by CTCRandomAppearanceMorph.
    constexpr std::uintptr_t SubmitMeshRva = 0x01C50170; // stdcall(handle,name,slot), RET0C
    constexpr std::uintptr_t AddMeshRva = 0x01C4E830; // cdecl(command)
    constexpr std::uintptr_t AddMeshHandlerRva = 0x025690B8;
    // Rebuilds ParentBoneMap by FName after attachment / mesh changes.
    constexpr std::uintptr_t BuildParentMapRva = 0x00701190;
    constexpr std::uintptr_t SetHiddenRva = 0x0034F570; // UPrimitiveComponent::vf155
    // UObject::LoadPackage, same cdecl(outer, filename, flags) as pawn vf332.
    constexpr std::uintptr_t LoadPackageRva = 0x001E37B0;
    constexpr std::uintptr_t LoadPackageHandlerRva = 0x0239D432;
    constexpr std::uintptr_t LoadObjectRva = 0x001E3000; // StaticLoadObject, seven cdecl args
    constexpr std::uintptr_t LoadObjectHandlerRva = 0x0239D3B4;
    constexpr std::uintptr_t MeshLoadCallRva = 0x01C4E990; // CALL StaticLoadObject in additional-mesh command
    constexpr std::uintptr_t StreamingCheckCallRva = 0x01C4E897; // CALL legacy stream/defer gate
    constexpr std::uintptr_t StreamingCheckRva = 0x01C4B060;
    constexpr std::uintptr_t PawnCastCallRva = 0x01C4E93F; // additional-mesh CALL Cast<AIPawn>
    constexpr std::uintptr_t AiPawnCastRva = 0x01C46130;
    constexpr std::uintptr_t WellingtonPawnCastRva = 0x01C46010;
    fable::core::hooking::CodePatch meshLoadCallPatch;
    fable::core::hooking::CodePatch streamingCheckCallPatch;
    fable::core::hooking::CodePatch pawnCastCallPatch;
    thread_local void* requestedSkinAsset=nullptr;
    constexpr std::size_t BoneStride = 0x50;
    // Export70's full outer chain in WeSISTER_01_SF.upk. The short name is
    // insufficient when Sister's package has not been loaded by the level.
    constexpr char SkinName[] = "wesister_01.MESH_SISTER_01";
    constexpr wchar_t SkinObjectPath[] = L"wesister_01.MESH_SISTER_01";
    constexpr wchar_t SkinPackage[] = L"WeSISTER_01_SF.upk";

    template<class T> T& Field(void* object, std::size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
    }

    bool IsSkinCommand(void* command) noexcept
    {
        __try
        {
            return command && std::memcmp(static_cast<char*>(command)+0x20,
                SkinName,sizeof(SkinName))==0;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ContainsSkin(void* pawn, void* skin) noexcept
    {
        __try
        {
            const int count=Field<int>(pawn,0x4FC);
            auto** meshes=Field<void**>(pawn,0x4F8);
            if (count<0 || count>128 || (count && !meshes)) return false;
            for (int i=0;i<count;++i)
                if (meshes[i]==skin) return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    bool PatchCall(fable::core::hooking::CodePatch& patch, std::uintptr_t site,
        std::uintptr_t expectedTarget, const void* replacement) noexcept
    {
        std::uint8_t expected[5]={0xE8};
        std::uint8_t redirected[5]={0xE8};
        const auto oldDelta=static_cast<std::uint32_t>(expectedTarget-site-5);
        const auto newDelta=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(replacement)-site-5);
        std::memcpy(expected+1,&oldDelta,sizeof(oldDelta));
        std::memcpy(redirected+1,&newDelta,sizeof(newDelta));
        return patch.Install(reinterpret_cast<void*>(site),expected,sizeof(expected),redirected,sizeof(redirected));
    }
}

namespace fable::game::hero_pawn::appearance::hooks
{
    TheresaHeroSkin* TheresaHeroSkin::active_ = nullptr;

    bool TheresaHeroSkin::Install(HMODULE module, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_==this) return true;
        if (!module || active_) return false;
        module_=module;
        diagnostics_=diagnostics;
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        std::uint8_t addSignature[]={0x6A,0xFF,0x68,0,0,0,0};
        const auto handler=base+AddMeshHandlerRva;
        std::memcpy(addSignature+3,&handler,sizeof(handler));
        const std::uint8_t mapSignature[]={0x83,0xEC,0x08,0x55,0x56,0x57};
        const std::uint8_t submitSignature[]={0x55,0x8B,0x6C,0x24,0x08,0x85,0xED};
        const std::uint8_t hiddenSignature[]={0x8B,0x81,0x28,0x01,0x00,0x00};
        const std::uint8_t pawnCastSignature[]={0x56,0x8B,0x74,0x24,0x08,0x85,0xF6};
        std::uint8_t packageSignature[]={0x55,0x8B,0xEC,0x6A,0xFF,0x68,0,0,0,0};
        std::uint8_t objectSignature[]={0x55,0x8B,0xEC,0x6A,0xFF,0x68,0,0,0,0};
        const auto packageHandler=base+LoadPackageHandlerRva;
        const auto objectHandler=base+LoadObjectHandlerRva;
        std::memcpy(packageSignature+6,&packageHandler,sizeof(packageHandler));
        std::memcpy(objectSignature+6,&objectHandler,sizeof(objectHandler));
        if (std::memcmp(reinterpret_cast<void*>(base+SubmitMeshRva),submitSignature,sizeof(submitSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+SetHiddenRva),hiddenSignature,sizeof(hiddenSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+LoadPackageRva),packageSignature,sizeof(packageSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+LoadObjectRva),objectSignature,sizeof(objectSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+WellingtonPawnCastRva),pawnCastSignature,sizeof(pawnCastSignature)))
        {
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","native helper signature mismatch");
            return false;
        }
        active_=this;
        if (!mapPatch_.Install(reinterpret_cast<void*>(base+BuildParentMapRva),
                mapSignature,sizeof(mapSignature),reinterpret_cast<void*>(&BuildSkinMap),sizeof(mapSignature)))
        {
            active_=nullptr;
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","parent bone map signature mismatch");
            return false;
        }
        originalMap_=reinterpret_cast<BuildMap>(mapPatch_.Original());
        if (!addPatch_.Install(reinterpret_cast<void*>(base+AddMeshRva),addSignature,
                sizeof(addSignature),reinterpret_cast<void*>(&AddSkin),sizeof(addSignature)))
        {
            Shutdown();
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","additional mesh signature mismatch");
            return false;
        }
        originalAdd_=reinterpret_cast<AddMesh>(addPatch_.Original());
        if (!PatchCall(meshLoadCallPatch,base+MeshLoadCallRva,base+LoadObjectRva,
                reinterpret_cast<const void*>(&LoadSkinObject)) ||
            !PatchCall(streamingCheckCallPatch,base+StreamingCheckCallRva,base+StreamingCheckRva,
                reinterpret_cast<const void*>(&CheckSkinStreaming)) ||
            !PatchCall(pawnCastCallPatch,base+PawnCastCallRva,base+AiPawnCastRva,
                reinterpret_cast<const void*>(&ResolveSkinPawn)))
        {
            Shutdown();
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","native mesh call-site signature mismatch");
            return false;
        }
        diagnostics_.Event("MultiplayerTheresaHeroSkinReady",
            "Hero owns skeleton/animations/weapons; Sister is an additional render mesh; fit=unverified");
        return true;
    }

    bool TheresaHeroSkin::Shutdown() noexcept
    {
        if (!pawnCastCallPatch.Shutdown() || !streamingCheckCallPatch.Shutdown() ||
            !meshLoadCallPatch.Shutdown()) return false;
        if (!addPatch_.Shutdown()) return false;
        if (!mapPatch_.Shutdown()) return false;
        if (active_==this) active_=nullptr;
        originalAdd_=nullptr;
        originalMap_=nullptr;
        for (auto& entry:entries_)
        {
            entry.graphic.store(nullptr);
            entry.thing.store(nullptr);
            entry.master.store(nullptr);
            entry.skin.store(nullptr);
            entry.queued.store(false);
        }
        return true;
    }

    TheresaHeroSkin::Entry* TheresaHeroSkin::FindGraphic(void* graphic) noexcept
    {
        if (graphic)
            for (auto& entry:entries_)
                if (entry.graphic.load(std::memory_order_acquire)==graphic) return &entry;
        return nullptr;
    }

    void TheresaHeroSkin::Forget(void* thing) noexcept
    {
        auto* hook=active_;
        if (!hook || !thing) return;
        for (auto& entry:hook->entries_)
            if (entry.thing.load()==thing)
            {
                // No stale pawn dereference here: native world retirement owns
                // destruction of the additional mesh and queued commands.
                entry.graphic.store(nullptr,std::memory_order_release);
                entry.skin.store(nullptr);
                entry.master.store(nullptr);
                entry.queued.store(false);
                entry.thing.store(nullptr,std::memory_order_release);
            }
    }

    void TheresaHeroSkin::Update(void* thing) noexcept
    {
        auto* hook=active_;
        if (!hook || !TheresaAppearanceHook::UsesTheresaPresentation(thing)) return;
        native::HeroMorphResolutionState resolved;
        if (!native::HeroMorphComponent::InspectResolution(thing,resolved)) return;
        Entry* entry=hook->FindGraphic(resolved.graphic);
        if (entry && entry->master.load()!=resolved.skeletalMeshComponent)
        {
            Forget(thing);
            entry=nullptr;
        }
        if (!entry)
        {
            Forget(thing);
            for (auto& candidate:hook->entries_)
            {
                void* empty=nullptr;
                if (!candidate.thing.compare_exchange_strong(empty,thing)) continue;
                candidate.master.store(resolved.skeletalMeshComponent);
                candidate.skin.store(nullptr);
                candidate.queued.store(false);
                candidate.graphic.store(resolved.graphic,std::memory_order_release);
                entry=&candidate;
                break;
            }
        }
        if (!entry) return;
        void* skin=entry->skin.load(std::memory_order_acquire);
        if (skin && !ContainsSkin(resolved.pawn,skin))
        {
            // A native appearance rebuild can remove all additional meshes.
            // Reattach to the current Hero driver without dereferencing skin.
            entry->skin.store(nullptr);
            entry->queued.store(false);
        }
        // Submit only once for a live graphic. Native streaming owns retries;
        // repeated submissions could create duplicate components/slot races.
        bool expected=false;
        if (!entry->queued.compare_exchange_strong(expected,true)) return;
        const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
        using Submit=void(__stdcall*)(void*,const char*,int);
        reinterpret_cast<Submit>(base+SubmitMeshRva)(resolved.graphic,SkinName,0);
        hook->diagnostics_.Event("MultiplayerTheresaHeroSkinQueued",
            "mesh=wesister_01.MESH_SISTER_01 package=WeSISTER_01_SF.upk driver=Hero slot=append-on-execution fit=unverified");
    }

    bool TheresaHeroSkin::FixMap(void* mesh) noexcept
    {
        __try
        {
            void* master=Field<void*>(mesh,0x29C);
            Entry* entry=nullptr;
            for (auto& candidate:entries_)
                if (master && candidate.graphic.load() && candidate.master.load()==master)
                { entry=&candidate; break; }
            if (!entry) return false;
            void* asset=Field<void*>(mesh,0x1E4);
            void* heroAsset=Field<void*>(master,0x1E4);
            if (!asset || !heroAsset || Field<int>(asset,0x98)!=80 ||
                Field<int>(heroAsset,0x98)!=91 || Field<int>(mesh,0x2A4)!=80) return false;
            auto* bones=Field<std::uint8_t*>(asset,0x94);
            auto* heroBones=Field<std::uint8_t*>(heroAsset,0x94);
            auto* map=Field<int*>(mesh,0x2A0);
            if (!bones || !heroBones || !map) return false;
            int resolved[80]={};
            unsigned int common=0;
            unsigned int fallback=0;
            for (int i=0;i<80;++i)
            {
                resolved[i]=-1;
                for (int j=0;j<91;++j)
                    if (!std::memcmp(bones+i*BoneStride,heroBones+j*BoneStride,8))
                    { resolved[i]=j; ++common; break; }
                if (resolved[i]>=0) continue;
                // Reference skeleton parent precedes its children. Bones absent
                // in Hero (breasts/stomach) follow the nearest mapped ancestor.
                // Runtime FMeshBone differs from its serialized layout:
                // GetParentBoneName RVA006FA680 reads parent at +40, stride50.
                const int parent=*reinterpret_cast<int*>(bones+i*BoneStride+0x40);
                if (i==0 || parent<0 || parent>=i || resolved[parent]<0) return false;
                resolved[i]=resolved[parent];
                ++fallback;
            }
            if (common!=77 || fallback!=3) return false;
            std::memcpy(map,resolved,sizeof(resolved));
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void __fastcall TheresaHeroSkin::BuildSkinMap(void* mesh, void*)
    {
        auto* hook=active_;
        if (!hook || !hook->originalMap_) return;
        hook->originalMap_(mesh);
        hook->FixMap(mesh);
    }

    void* __cdecl TheresaHeroSkin::LoadSkinObject(void* type, void* outer,
        const wchar_t* name, const wchar_t* file, unsigned int flags, void* sandbox, int reconcile)
    {
        // Only this single native apparel call site is patched. The request
        // scope below supplies a class-checked mesh already loaded on this
        // thread; other garments retain their exact native lookup arguments.
        if (requestedSkinAsset) return requestedSkinAsset;
        auto* hook=active_;
        if (!hook) return nullptr;
        using Load=void*(__cdecl*)(void*,void*,const wchar_t*,const wchar_t*,unsigned int,void*,int);
        return reinterpret_cast<Load>(reinterpret_cast<std::uintptr_t>(hook->module_)+LoadObjectRva)(
            type,outer,name,file,flags,sandbox,reconcile);
    }

    bool __cdecl TheresaHeroSkin::CheckSkinStreaming(void* graphic, void* command,
        std::size_t size, void* callback, char replace)
    {
        // The owner-run logs prove that explicit package/object loading alone
        // did not create a component. A preloaded request needs neither the
        // legacy actor-name stream gate nor its cached command replay.
        if (requestedSkinAsset) return false;
        auto* hook=active_;
        if (!hook) return false;
        using Check=bool(__cdecl*)(void*,void*,std::size_t,void*,char);
        return reinterpret_cast<Check>(reinterpret_cast<std::uintptr_t>(hook->module_)+StreamingCheckRva)(
            graphic,command,size,callback,replace);
    }

    void* __cdecl TheresaHeroSkin::ResolveSkinPawn(void* pawn)
    {
        auto* hook=active_;
        if (!hook) return nullptr;
        using Cast=void*(__cdecl*)(void*);
        const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
        // The original command is AI-only: its Cast<AIPawn> rejects both local
        // and promoted AHeroPawn, before component allocation. For our scoped
        // Hero request use the actual common base; all fields subsequently
        // accessed are WellingtonPawn members (main mesh / apparel / material
        // callbacks), also used by the native Hero initialization path.
        if (requestedSkinAsset)
        {
            void* result=reinterpret_cast<Cast>(base+WellingtonPawnCastRva)(pawn);
            hook->diagnostics_.Event(result?"MultiplayerTheresaHeroSkinPawnAccepted":
                "MultiplayerTheresaHeroSkinPawnRejected","scoped additional-mesh cast uses WellingtonPawn instead of AIPawn");
            return result;
        }
        return reinterpret_cast<Cast>(base+AiPawnCastRva)(pawn);
    }

    void __cdecl TheresaHeroSkin::AddSkin(void* command)
    {
        auto* hook=active_;
        if (!hook || !hook->originalAdd_) return;
        Entry* entry=nullptr;
        void* graphic=nullptr;
        void* pawn=nullptr;
        void* master=nullptr;
        int slot=-1;
        __try
        {
            if (IsSkinCommand(command))
            {
                graphic=Field<void*>(command,0);
                entry=hook->FindGraphic(graphic);
                if (entry)
                {
                    pawn=Field<void*>(graphic,8);
                    master=entry->master.load();
                    if (!pawn || Field<void*>(pawn,0x384)!=master) entry=nullptr;
                    else
                    {
                        // Append immediately before execution, also on a replay
                        // after package streaming. Never reserve sparse slots.
                        slot=Field<int>(pawn,0x4FC);
                        if (slot<0 || slot>=128) entry=nullptr;
                        else Field<int>(command,4)=slot;
                    }
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { entry=nullptr; }
        void* loadedAsset=nullptr;
        if (entry)
        {
            // A clothing rebuild may already have removed a previous skin.
            // Reveal the driver before a potentially deferred/failed load.
            using Hide=void(__thiscall*)(void*,unsigned int);
            const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
            reinterpret_cast<Hide>(base+SetHiddenRva)(master,0);
            // This runs on the native graphics command thread. Resolve the
            // cooked package explicitly before asking the apparel loader to
            // attach the fully qualified object; do not rely on level preloads
            // or the legacy short-name-to-package lookup.
            using LoadPackage=void*(__cdecl*)(void*,const wchar_t*,unsigned int);
            using LoadObject=void*(__cdecl*)(void*,void*,const wchar_t*,const wchar_t*,
                unsigned int,void*,int);
            void* package=reinterpret_cast<LoadPackage>(base+LoadPackageRva)(nullptr,SkinPackage,0);
            if (package)
            {
                void* heroAsset=Field<void*>(master,0x1E4);
                void* meshClass=heroAsset?Field<void*>(heroAsset,0x34):nullptr;
                if (meshClass)
                    loadedAsset=reinterpret_cast<LoadObject>(base+LoadObjectRva)(
                        meshClass,nullptr,SkinObjectPath,nullptr,0,nullptr,1);
            }
            char detail[192]={};
            std::snprintf(detail,sizeof(detail),"package=%p asset=%p path=wesister_01.MESH_SISTER_01",package,loadedAsset);
            hook->diagnostics_.Event(loadedAsset?"MultiplayerTheresaHeroSkinAssetLoaded":
                "MultiplayerTheresaHeroSkinAssetFailed",detail);
            if (!loadedAsset) return; // Leave the original visible Hero intact.
        }
        void* previous=requestedSkinAsset;
        __try
        {
            // Explicitly clear scope for nested unrelated apparel commands.
            requestedSkinAsset=entry?loadedAsset:nullptr;
            hook->originalAdd_(command);
        }
        __finally { requestedSkinAsset=previous; }
        if (!entry || entry->graphic.load()!=graphic) return;
        bool installed=false;
        const char* stage="component-missing";
        int skinBones=0;
        int driverBones=0;
        int mapCount=0;
        __try
        {
            if (Field<int>(pawn,0x4FC)>slot && Field<void*>(pawn,0x384)==master)
            {
                void* skin=Field<void**>(pawn,0x4F8)[slot];
                using Hide=void(__thiscall*)(void*,unsigned int);
                const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
                stage="asset-or-parent-mismatch";
                if (skin && Field<void*>(skin,0x1E4)==loadedAsset && Field<void*>(skin,0x29C)==master)
                {
                    skinBones=Field<int>(loadedAsset,0x98);
                    driverBones=Field<int>(Field<void*>(master,0x1E4),0x98);
                    mapCount=Field<int>(skin,0x2A4);
                    stage="bone-map-rejected";
                }
                if (skin && Field<void*>(skin,0x1E4)==loadedAsset &&
                    Field<void*>(skin,0x29C)==master && hook->FixMap(skin))
                {
                    // Native vf82 gates animation-node ticking on bit80 and
                    // pose evaluation on bit20 when the driver is not drawn.
                    // Both must continue while only the follower is visible.
                    Field<std::uint32_t>(master,0x3B0)|=0xA0u;
                    reinterpret_cast<Hide>(base+SetHiddenRva)(master,1);
                    entry->skin.store(skin,std::memory_order_release);
                    installed=true;
                    stage="attached";
                }
                else if (skin)
                {
                    // A streamed asset with unexpected bones must not replace
                    // the visible Hero or draw an unbound second body.
                    reinterpret_cast<Hide>(base+SetHiddenRva)(skin,1);
                    reinterpret_cast<Hide>(base+SetHiddenRva)(master,0);
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { installed=false; stage="native-field-fault"; }
        if (hook->reports_.fetch_add(1)<24)
        {
            char detail[256]={};
            std::snprintf(detail,sizeof(detail),
                "graphic=%p master=%p slot=%d attached=%s stage=%s skin_bones=%d driver_bones=%d map_count=%d fit=unverified",
                graphic,master,slot,installed?"true":"false",stage,skinBones,driverBones,mapCount);
            hook->diagnostics_.Event(installed?"MultiplayerTheresaHeroSkinAttached":
                "MultiplayerTheresaHeroSkinDeferred",detail);
        }
    }
}

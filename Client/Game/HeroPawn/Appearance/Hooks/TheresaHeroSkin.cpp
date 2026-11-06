#include "TheresaHeroSkin.h"
#include "TheresaAppearanceHook.h"
#include "Game/HeroPawn/Appearance/Native/HeroMorphComponent.h"
#include "Game/HeroPawn/Appearance/Native/HeroClothingComponent.h"
#include "Game/HeroPawn/Appearance/Native/TheresaGarmentShape.h"

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
    constexpr std::uintptr_t SkeletalComponentVtableRva=0x02B51FAC;
    // USkeletalMeshComponent::ShowMaterialSection(index, show, LOD), RET0C.
    constexpr std::uintptr_t ShowMaterialSectionRva = 0x0070A450;
    constexpr std::uintptr_t ShowMaterialSectionHandlerRva = 0x023D1078;
    constexpr std::uintptr_t SetMaterialRva = 0x006F7730; // USkeletalMeshComponent::vf169, RET8
    constexpr std::uintptr_t ObjectNameRva=0x000551D0; // UObject::GetName, thiscall(FString*)
    constexpr std::uintptr_t FreeArrayRva=0x0016B8C0;
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
    constexpr char ChildSkinName[]="weyoung_sister_01.MESH_YOUNG_SISTER_01";
    constexpr wchar_t ChildSkinObjectPath[]=L"weyoung_sister_01.MESH_YOUNG_SISTER_01";
    constexpr wchar_t ChildSkinPackage[]=L"WeYOUNG_SISTER_01_SF.upk";
    constexpr char LimbSkinName[]="wesister_c1.MESH_SISTER_01";
    constexpr wchar_t LimbSkinObjectPath[]=L"wesister_c1.MESH_SISTER_01";
    constexpr wchar_t LimbSkinPackage[]=L"WeSISTER_C1_SF.upk";
    // Private command tag: LoadSkinObject supplies the current Hero asset.
    constexpr char ArmSkinName[]="theresa_preview.HeroBareArms";
    constexpr std::uintptr_t CommandAllocateRva=0x01C52620;
    constexpr std::uintptr_t CommandSubmitRva=0x01C52B90;
    std::atomic_bool cutsceneHidden{false};

    template<class T> T& Field(void* object, std::size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
    }

    unsigned int CoveredOutfit(const fable::game::hero_pawn::appearance::HeroClothingState& clothing)
    {
        if (!clothing.IsSane()) return 0;
        // CTCInventoryClothing categories: head, torso, gloves, legs, feet, suit.
        const bool suit=clothing.definitionIndices[5]>0;
        return ((suit || clothing.definitionIndices[1]>0)?1u:0u) |
            ((suit || clothing.definitionIndices[3]>0)?2u:0u);
    }

    bool IsHeroArmMaterial(int index) noexcept
    {
        // Validated MESH_HERO material order: L_forearm, L_upperarm,
        // R_upperarm, R_forearm. Torso, head and hands stay out of this layer.
        return index==6 || index==8 || index==10 || index==11;
    }

    void MaskWholeMesh(void* mesh,std::uintptr_t base)
    {
        void* asset=Field<void*>(mesh,0x1E4);
        if (!asset) return;
        const int materials=Field<int>(asset,0x5C),lods=Field<int>(asset,0xE4);
        if (materials<1 || materials>64 || lods<1 || lods>8) return;
        using Show=void(__thiscall*)(void*,int,int,int);
        auto show=reinterpret_cast<Show>(base+ShowMaterialSectionRva);
        for (int lod=0;lod<lods;++lod)
        {
            auto* array=Field<std::uint8_t*>(mesh,0x3D4);
            void* info=array && lod<Field<int>(mesh,0x3D8)?array+lod*0x18:nullptr;
            if (!info || Field<int>(info,4)!=materials || !Field<void*>(info,0))
            {
                show(mesh,0,0,lod); // Native allocation/initialization first.
                array=Field<std::uint8_t*>(mesh,0x3D4);
                info=array && lod<Field<int>(mesh,0x3D8)?array+lod*0x18:nullptr;
            }
            if (!info || Field<int>(info,4)!=materials) continue;
            auto* flags=Field<unsigned int*>(info,0);
            if (!flags) continue;
            bool changed=false;
            for (int part=0;part<materials;++part)
                if (!flags[part]) { flags[part]=1; changed=true; }
            // 0070A450 copies the entire hidden-material array to the render
            // object. Mask by material directly, avoiding LOD section remaps.
            if (changed) show(mesh,0,0,lod);
        }
    }

    void AssetName(void* asset,std::uintptr_t base,char* out,int size)
    {
        struct String { wchar_t* data; int count,capacity; } text{};
        using GetName=String*(__thiscall*)(void*,String*);
        reinterpret_cast<GetName>(base+ObjectNameRva)(asset,&text);
        int used=0;
        for (;text.data && used+1<size && used<text.count && text.data[used];++used)
            out[used]=text.data[used]<128?static_cast<char>(text.data[used]):'_';
        out[used]=0;
        if (text.data)
        {
            using Free=void(__cdecl*)(void*);
            reinterpret_cast<Free>(base+FreeArrayRva)(text.data);
        }
    }

    bool IsSkinCommand(void* command) noexcept
    {
        __try
        {
            return command && (std::strcmp(static_cast<char*>(command)+0x20,SkinName)==0 ||
                std::strcmp(static_cast<char*>(command)+0x20,LimbSkinName)==0 ||
                std::strcmp(static_cast<char*>(command)+0x20,ArmSkinName)==0 ||
                std::strcmp(static_cast<char*>(command)+0x20,ChildSkinName)==0);
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
        native::TheresaGarmentShape::Initialize(module,diagnostics);
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
        std::uint8_t sectionSignature[]={0x6A,0xFF,0x68,0,0,0,0};
        const auto sectionHandler=base+ShowMaterialSectionHandlerRva;
        const std::uint8_t materialSignature[]={0x53,0x8B,0x5C,0x24,0x08,0x56,0x57};
        const std::uint8_t objectNameSignature[]={0x51,0x56,0x8B,0x74,0x24,0x0C};
        std::uint8_t freeSignature[]={0x8B,0x0D,0,0,0,0,0x85,0xC9};
        const auto allocator=base+0x03094C6C;
        std::memcpy(freeSignature+2,&allocator,sizeof(allocator));
        std::memcpy(sectionSignature+3,&sectionHandler,sizeof(sectionHandler));
        std::memcpy(packageSignature+6,&packageHandler,sizeof(packageHandler));
        std::memcpy(objectSignature+6,&objectHandler,sizeof(objectHandler));
        if (std::memcmp(reinterpret_cast<void*>(base+ObjectNameRva),objectNameSignature,sizeof(objectNameSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+FreeArrayRva),freeSignature,sizeof(freeSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+SetMaterialRva),materialSignature,sizeof(materialSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+ShowMaterialSectionRva),sectionSignature,sizeof(sectionSignature)) ||
            std::memcmp(reinterpret_cast<void*>(base+SubmitMeshRva),submitSignature,sizeof(submitSignature)) ||
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
        if (!hiddenPatch_.Install(reinterpret_cast<void*>(base+SetHiddenRva),hiddenSignature,
                sizeof(hiddenSignature),reinterpret_cast<void*>(&GuardHidden),sizeof(hiddenSignature)))
        {
            Shutdown();
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","visibility guard signature mismatch");
            return false;
        }
        originalHidden_=reinterpret_cast<SetHidden>(hiddenPatch_.Original());
        if (!sectionPatch_.Install(reinterpret_cast<void*>(base+ShowMaterialSectionRva),sectionSignature,
                sizeof(sectionSignature),reinterpret_cast<void*>(&GuardSection),sizeof(sectionSignature)))
        {
            Shutdown();
            diagnostics_.Event("MultiplayerTheresaHeroSkinRejected","material-section guard signature mismatch");
            return false;
        }
        originalSection_=reinterpret_cast<ShowSection>(sectionPatch_.Original());
        diagnostics_.Event("MultiplayerTheresaVisibilityGuardReady","native driver unhide/section-reset guarded, including world-entry meshes");
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
        if (!hiddenPatch_.Shutdown()) return false;
        if (!sectionPatch_.Shutdown()) return false;
        if (active_==this) active_=nullptr;
        originalAdd_=nullptr;
        originalMap_=nullptr;
        originalHidden_=nullptr;
        originalSection_=nullptr;
        native::TheresaGarmentShape::Shutdown();
        for (auto& entry:entries_)
        {
            entry.graphic.store(nullptr);
            entry.thing.store(nullptr);
            entry.master.store(nullptr);
            entry.skin.store(nullptr);
            entry.limbs.store(nullptr);
            entry.limbsQueued.store(false);
            entry.arms.store(nullptr);
            entry.armsQueued.store(false);
            entry.nextVisualAt=0;
            entry.outfitRefreshAt=0;
            entry.outfitRefreshAttempts=0;
            entry.visibleBeforeSceneCount=0;
            entry.sceneWasHidden=false;
            entry.shape={};
            entry.nextShapeAt=0;
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
                entry.limbs.store(nullptr);
                entry.limbsQueued.store(false);
                entry.arms.store(nullptr);
                entry.armsQueued.store(false);
                entry.nextVisualAt=0;
                entry.outfitRefreshAt=0;
                entry.outfitRefreshAttempts=0;
                entry.visibleBeforeSceneCount=0;
                entry.sceneWasHidden=false;
                entry.shape={};
                entry.nextShapeAt=0;
                entry.master.store(nullptr);
                entry.queued.store(false);
                entry.thing.store(nullptr,std::memory_order_release);
            }
    }

    void TheresaHeroSkin::Update(void* thing, bool childhood) noexcept
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
                candidate.limbs.store(nullptr);
                candidate.limbsQueued.store(false);
                candidate.arms.store(nullptr);
                candidate.armsQueued.store(false);
                candidate.nextVisualAt=0;
                candidate.outfitRefreshAt=GetTickCount64()+1500;
                candidate.outfitRefreshAttempts=childhood?0u:3u;
                candidate.visibleBeforeSceneCount=0;
                candidate.sceneWasHidden=false;
                candidate.shape={};
                candidate.nextShapeAt=0;
                candidate.queued.store(false);
                candidate.childhood.store(childhood);
                candidate.visualKey.store(0);
                candidate.outfitMask.store(0);
                candidate.appliedOutfitMask.store(~0u);
                candidate.graphic.store(resolved.graphic,std::memory_order_release);
                entry=&candidate;
                break;
            }
        }
        if (!entry) return;
        if (!childhood && GetTickCount64()>=entry->nextShapeAt)
        {
            entry->nextShapeAt=GetTickCount64()+500;
            const bool first=entry->shape.controller==nullptr;
            if (native::HeroMorphComponent::ApplyTheresaShape(thing,entry->shape) && first)
                hook->diagnostics_.Event("MultiplayerTheresaVisualShapeApplied",
                    "adult visual bones only; progression_influence=0.20 progression_bounds=0.98..1.04 clavicles0.86 arm_roots_compensated waist0.97 pelvis1.035 chest=painted_weights_native_bone_depth; gameplay=unchanged fit=unverified");
        }
        void* skin=entry->skin.load(std::memory_order_acquire);
        if (skin && !ContainsSkin(resolved.pawn,skin))
        {
            // A native appearance rebuild can remove all additional meshes.
            // Reattach to the current Hero driver without dereferencing skin.
            entry->skin.store(nullptr);
            entry->queued.store(false);
            entry->visualKey.store(0);
        }
        if (skin && ContainsSkin(resolved.pawn,skin))
        {
            // A manual clothing change repairs the post-transition Hero head
            // according to the owner's observation. Re-publish those same
            // modifiers once the new presentation has settled, on this game
            // update thread (never from the graphics command callback).
            const auto refreshNow=GetTickCount64();
            if (entry->outfitRefreshAttempts && !cutsceneHidden.load() && refreshNow>=entry->outfitRefreshAt)
            {
                --entry->outfitRefreshAttempts; // Bound retries before callbacks.
                entry->outfitRefreshAt=refreshNow+1000;
                if (native::HeroClothingComponent::RefreshEquipped(thing))
                {
                    entry->outfitRefreshAttempts=0;
                    entry->visualKey.store(0);
                    entry->nextVisualAt=0;
                    hook->diagnostics_.Event("MultiplayerTheresaTransitionOutfitRefreshed",
                        "equipped modifiers republished after binding/transition; equipped slots unchanged; head fix visually unverified");
                    return; // Native rebuild may queue removal of old followers.
                }
                if (!entry->outfitRefreshAttempts)
                    hook->diagnostics_.Event("MultiplayerTheresaTransitionOutfitRefreshFailed",
                        "equipped clothing unavailable after three attempts; no wear/unwear fallback");
            }
            void* limbs=entry->limbs.load();
            if (limbs && !ContainsSkin(resolved.pawn,limbs))
            { entry->limbs.store(nullptr); entry->limbsQueued.store(false); entry->visualKey.store(0); }
            void* arms=entry->arms.load();
            if (arms && !ContainsSkin(resolved.pawn,arms))
            { entry->arms.store(nullptr); entry->armsQueued.store(false); entry->visualKey.store(0); }
            if (!childhood && !entry->armsQueued.exchange(true))
            {
                using Submit=void(__stdcall*)(void*,const char*,int);
                reinterpret_cast<Submit>(reinterpret_cast<std::uintptr_t>(hook->module_)+SubmitMeshRva)(
                    resolved.graphic,ArmSkinName,0);
            }
            if (!childhood && !entry->limbsQueued.exchange(true))
            {
                using Submit=void(__stdcall*)(void*,const char*,int);
                reinterpret_cast<Submit>(reinterpret_cast<std::uintptr_t>(hook->module_)+SubmitMeshRva)(
                    resolved.graphic,LimbSkinName,0);
            }
            HeroClothingState clothing;
            // Childhood can have no clothing inventory. Cutscene visibility
            // must still update for that presentation.
            const bool capturedClothing=native::HeroClothingComponent::Capture(thing,clothing);
            entry->outfitMask.store(!entry->childhood.load() && capturedClothing?CoveredOutfit(clothing):0);
            {
                std::uint64_t key=cutsceneHidden.load()?2:1;
                if (capturedClothing)
                    for (auto id:clothing.definitionIndices) key=(key^static_cast<std::uint32_t>(id))*1099511628211ull;
                // Native world/body commands may reset visibility without a
                // clothing change. Reconcile on the graphics thread as well.
                const auto now=GetTickCount64();
                const bool changed=entry->visualKey.exchange(key)!=key;
                if (changed || now>=entry->nextVisualAt)
                {
                    entry->nextVisualAt=now+500;
                    const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
                    using Allocate=void*(__cdecl*)(unsigned int,unsigned int);
                    using Submit=void(__cdecl*)(void(__cdecl*)(void*),void*);
                    void* command=reinterpret_cast<Allocate>(base+CommandAllocateRva)(0x20,4);
                    Field<void*>(command,0)=resolved.graphic;
                    reinterpret_cast<Submit>(base+CommandSubmitRva)(&RefreshVisual,command);
                    if (changed)
                        hook->diagnostics_.Event("MultiplayerTheresaClothingOverlayQueued",
                            "native synchronized equipped clothing remains on Hero driver; render visibility refreshed");
                }
            }
        }
        // Submit only once for a live graphic. Native streaming owns retries;
        // repeated submissions could create duplicate components/slot races.
        bool expected=false;
        if (!entry->queued.compare_exchange_strong(expected,true)) return;
        const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
        using Submit=void(__stdcall*)(void*,const char*,int);
        reinterpret_cast<Submit>(base+SubmitMeshRva)(resolved.graphic,entry->childhood.load()?ChildSkinName:SkinName,0);
        hook->diagnostics_.Event("MultiplayerTheresaHeroSkinQueued",
            entry->childhood.load()?"mesh=MESH_YOUNG_SISTER_01 driver=child-Hero fit=unverified":
                "mesh=MESH_SISTER_01 driver=Hero fit=unverified");
    }

    bool TheresaHeroSkin::FixMap(void* mesh, bool bareArms) noexcept
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
            if (!asset || !heroAsset) return false;
            const int boneCount=Field<int>(asset,0x98);
            if (asset==heroAsset && boneCount==91 && Field<int>(mesh,0x2A4)==91 &&
                (bareArms || requestedSkinAsset==asset || entry->arms.load()==mesh))
            {
                auto* map=Field<int*>(mesh,0x2A0);
                if (!map) return false;
                for (int i=0;i<91;++i) map[i]=i;
                return true;
            }
            if ((boneCount!=80 && boneCount!=98) || Field<int>(heroAsset,0x98)!=91 ||
                Field<int>(mesh,0x2A4)!=boneCount) return false;
            auto* bones=Field<std::uint8_t*>(asset,0x94);
            auto* heroBones=Field<std::uint8_t*>(heroAsset,0x94);
            auto* map=Field<int*>(mesh,0x2A0);
            if (!bones || !heroBones || !map) return false;
            int resolved[128]={};
            unsigned int common=0;
            unsigned int fallback=0;
            for (int i=0;i<boneCount;++i)
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
            if (common!=77 || fallback!=static_cast<unsigned int>(boneCount-77)) return false;
            std::memcpy(map,resolved,boneCount*sizeof(int));
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

    void TheresaHeroSkin::SetCutsceneHidden(bool hidden) noexcept
    {
        cutsceneHidden.store(hidden,std::memory_order_release);
    }

    void TheresaHeroSkin::OnWorldReady() noexcept
    {
        auto* hook=active_;
        if (!hook) return;
        for (auto& entry:hook->entries_)
            if (entry.thing.load() && !entry.childhood.load())
            {
                entry.outfitRefreshAt=GetTickCount64()+1500;
                entry.outfitRefreshAttempts=3;
            }
    }

    void TheresaHeroSkin::InvalidateVisual(void* thing) noexcept
    {
        if (!active_ || !thing) return;
        for (auto& entry:active_->entries_)
            if (entry.thing.load()==thing)
            {
                entry.appliedOutfitMask.store(~0u);
                entry.visualKey.store(0);
            }
    }

    bool TheresaHeroSkin::InspectForTest(void* thing, bool hidden) noexcept
    {
        auto* hook=active_;
        if (!hook || !thing) return false;
        __try
        {
            for (auto& entry:hook->entries_)
            {
                if (entry.thing.load()!=thing) continue;
                void* graphic=entry.graphic.load();
                void* skin=entry.skin.load();
                void* master=entry.master.load();
                if (!graphic || !skin || !master) return false;
                void* pawn=Field<void*>(graphic,8);
                if (!pawn || !ContainsSkin(pawn,skin) || Field<void*>(pawn,0x384)!=master ||
                    Field<void*>(skin,0x29C)!=master ||
                    ((Field<unsigned int>(skin,0x128)&4u)!=0)!=hidden ||
                    (Field<unsigned int>(master,0x3B0)&0xA0u)!=0xA0u) return false;
                const int count=Field<int>(skin,0x2A4);
                auto* map=Field<int*>(skin,0x2A0);
                if (count!=(entry.childhood.load()?98:80) || !map) return false;
                for (int i=0;i<count;++i) if (map[i]<0 || map[i]>=91) return false;
                if (!entry.childhood.load())
                {
                    HeroClothingState clothing;
                    if (!native::HeroClothingComponent::Capture(thing,clothing)) return false;
                    const auto mask=CoveredOutfit(clothing);
                    if (!(Field<unsigned int>(master,0x128)&4u)) return false;
                    void* arms=entry.arms.load();
                    if (!arms || !ContainsSkin(pawn,arms) || Field<void*>(arms,0x29C)!=master ||
                        Field<void*>(arms,0x1E4)!=Field<void*>(master,0x1E4) || Field<int>(arms,0x2A4)!=91 ||
                        ((Field<unsigned int>(arms,0x128)&4u)!=0)!=(hidden || !(mask&1u))) return false;
                    auto* armMap=Field<int*>(arms,0x2A0);
                    if (!armMap) return false;
                    for (int bone=0;bone<91;++bone) if (armMap[bone]!=bone) return false;
                    {
                        void* armLod=Field<void*>(arms,0x3D4);
                        if (!armLod || Field<int>(armLod,4)!=18) return false;
                        auto* bodyFlags=Field<unsigned int*>(armLod,0);
                        if (!bodyFlags) return false;
                        for (int part=0;part<18;++part)
                            if (bodyFlags[part]!=((mask&1u) && IsHeroArmMaterial(part)?0u:1u)) return false;
                    }
                    // Native LOD0 hidden-material array: actual state, not our cache.
                    void* lod=Field<void*>(skin,0x3D4);
                    if (!lod || Field<int>(lod,4)!=4) return false;
                    auto* flags=Field<unsigned int*>(lod,0);
                    if (!flags || flags[0]!=(mask&1u?1u:0u) || flags[1]!=(mask&2u?1u:0u) ||
                        flags[2] || flags[3]) return false;
                    void* limbs=entry.limbs.load();
                    if (!limbs || !ContainsSkin(pawn,limbs) || Field<void*>(limbs,0x29C)!=master ||
                        Field<int>(limbs,0x2A4)!=80 || ((Field<unsigned int>(limbs,0x128)&4u)!=0)!=hidden) return false;
                    void* limbLod=Field<void*>(limbs,0x3D4);
                    if (!limbLod || Field<int>(limbLod,4)!=4) return false;
                    auto* limbFlags=Field<unsigned int*>(limbLod,0);
                    if (!limbFlags || limbFlags[0]!=(mask&1u?0u:1u) || limbFlags[1]!=(mask&2u?0u:1u) ||
                        !limbFlags[2] || !limbFlags[3]) return false;
                    void* limbAsset=Field<void*>(limbs,0x1E4);
                    if (Field<int>(limbs,0x1DC)!=4 || !limbAsset || Field<int>(limbAsset,0x5C)!=4) return false;
                    auto** overrides=Field<void**>(limbs,0x1D8);
                    auto** materials=Field<void**>(limbAsset,0x58);
                    if (!overrides || !materials) return false;
                    for (int material=0;material<4;++material)
                        if (!materials[material] || overrides[material]!=materials[material]) return false;
                    native::HeroMorphResolutionState resolved;
                    if (!native::HeroMorphComponent::InspectResolution(thing,resolved) ||
                        !entry.shape.controller || resolved.massBoneScaling!=entry.shape.controller) return false;
                    const int scaleCount=Field<int>(entry.shape.controller,0xC0);
                    auto* scales=Field<float*>(entry.shape.controller,0xBC);
                    if (!scales) return false;
                    for (std::size_t b=0;b<entry.shape.applied.size();++b)
                    {
                        const auto& expected=entry.shape.lastApplied[b];
                        if (!entry.shape.applied[b] || expected.boneIndex>=scaleCount ||
                            std::fabs(scales[expected.boneIndex*3]-expected.x)>0.0001f ||
                            std::fabs(scales[expected.boneIndex*3+1]-expected.y)>0.0001f ||
                            std::fabs(scales[expected.boneIndex*3+2]-expected.z)>0.0001f) return false;
                    }
                }
                else if (!(Field<unsigned int>(master,0x128)&4u)) return false;
                return true;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    bool TheresaHeroSkin::IsDriver(void* mesh) noexcept
    {
        if (!mesh) return false;
        if (TheresaAppearanceHook::IsLocalHeroDriverMesh(mesh)) return true;
        __try
        {
            for (const auto& entry:entries_)
            {
                void* graphic=entry.graphic.load();
                if (!graphic || !entry.thing.load()) continue;
                if (entry.master.load()==mesh) return true;
                // A transition can replace the main mesh before Update has
                // rebound the entry. Resolve the current graphic as well.
                void* pawn=Field<void*>(graphic,8);
                if (pawn && (Field<void*>(pawn,0x384)==mesh || Field<void*>(pawn,0x460)==mesh)) return true;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    void __fastcall TheresaHeroSkin::GuardHidden(void* mesh,void*,unsigned int hidden)
    {
        auto* hook=active_;
        if (!hook || !hook->originalHidden_) return;
        bool blocked=false;
        __try
        {
            if (!hidden && hook->IsDriver(mesh)) { hidden=1; blocked=true; }
            else if (!hidden && mesh && Field<std::uintptr_t>(mesh,0)==
                reinterpret_cast<std::uintptr_t>(hook->module_)+SkeletalComponentVtableRva)
            {
                void* parent=Field<void*>(mesh,0x29C);
                if (parent && hook->IsDriver(parent) && Field<void*>(mesh,0x1E4)==Field<void*>(parent,0x1E4))
                {
                    bool current=false;
                    for (const auto& entry:hook->entries_)
                        if (entry.arms.load()==mesh && entry.master.load()==parent) current=true;
                    void* asset=Field<void*>(mesh,0x1E4);
                    const int lods=Field<int>(mesh,0x3D8);
                    auto* info=Field<std::uint8_t*>(mesh,0x3D4);
                    bool masked=current && asset && Field<int>(asset,0x5C)==18 && lods>=1 && lods<=8 && info;
                    for (int lod=0;masked && lod<lods;++lod)
                    {
                        void* record=info+lod*0x18;
                        auto* flags=Field<unsigned int*>(record,0);
                        if (Field<int>(record,4)!=18 || !flags) { masked=false; break; }
                        for (int part=0;part<18;++part)
                            if (!IsHeroArmMaterial(part) && !flags[part]) masked=false;
                    }
                    if (!masked) { hidden=1; blocked=true; }
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        hook->originalHidden_(mesh,hidden);
        if (blocked && hook->hiddenReports_.fetch_add(1)<16)
            hook->diagnostics_.Event("MultiplayerTheresaHeroRevealBlocked","native unhide refused for Hero driver or unmasked/retired arm follower");
    }

    void __fastcall TheresaHeroSkin::GuardSection(void* mesh,void*,int section,int show,int lod)
    {
        auto* hook=active_;
        if (!hook || !hook->originalSection_) return;
        __try
        {
            if (show && hook->IsDriver(mesh)) show=0;
            else if (show && mesh)
            {
                void* parent=Field<void*>(mesh,0x29C);
                void* asset=Field<void*>(mesh,0x1E4);
                if (parent && asset && hook->IsDriver(parent) && asset==Field<void*>(parent,0x1E4))
                {
                    int material=section;
                    if (lod>0 && lod<Field<int>(asset,0x134))
                    {
                        auto* info=Field<std::uint8_t*>(asset,0x130)+lod*0x48;
                        if (section>=0 && section<Field<int>(info,0xC)) material=Field<int*>(info,8)[section];
                    }
                    if (!IsHeroArmMaterial(material)) show=0;
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        hook->originalSection_(mesh,section,show,lod);
    }

    void __cdecl TheresaHeroSkin::RefreshVisual(void* command)
    {
        auto* hook=active_;
        if (!hook || !command) return;
        __try
        {
            void* graphic=Field<void*>(command,0);
            auto* entry=hook->FindGraphic(graphic);
            if (!entry || !entry->skin.load()) return;
            void* pawn=Field<void*>(graphic,8);
            if (!pawn || Field<void*>(pawn,0x384)!=entry->master.load()) return;
            const int count=Field<int>(pawn,0x4FC);
            auto** meshes=Field<void**>(pawn,0x4F8);
            if (count<0 || count>128 || (count && !meshes)) return;
            using Hide=void(__thiscall*)(void*,unsigned int);
            auto hide=reinterpret_cast<Hide>(reinterpret_cast<std::uintptr_t>(hook->module_)+SetHiddenRva);
            const auto module=reinterpret_cast<std::uintptr_t>(hook->module_);
            hide(entry->master.load(),1);
            MaskWholeMesh(entry->master.load(),module);
            void* nativeMesh=Field<void*>(pawn,0x460);
            if (nativeMesh && nativeMesh!=entry->master.load())
            {
                Field<unsigned int>(nativeMesh,0x3B0)|=0xA0u;
                hide(nativeMesh,1);
                MaskWholeMesh(nativeMesh,module);
            }
            void* primary=entry->skin.load();
            if (!ContainsSkin(pawn,primary)) return;
            void* limb=entry->limbs.load();
            void* skinAsset=Field<void*>(primary,0x1E4);
            // Native appearance refresh can re-project Hero texture variants
            // onto additional meshes. Sister's face/hair must keep their own
            // authored materials, not only remain geometrically visible.
            if (skinAsset && Field<int>(skinAsset,0x5C)>0 && Field<int>(skinAsset,0x5C)<=8)
            {
                auto** source=Field<void**>(skinAsset,0x58);
                using SetMaterial=void(__thiscall*)(void*,int,void*);
                for (int part=0;source && part<Field<int>(skinAsset,0x5C);++part)
                {
                    auto** overrides=Field<void**>(primary,0x1D8);
                    const int bound=Field<int>(primary,0x1DC);
                    if (source[part] && (bound<=part || !overrides || overrides[part]!=source[part]))
                        reinterpret_cast<SetMaterial>(reinterpret_cast<std::uintptr_t>(hook->module_)+SetMaterialRva)(primary,part,source[part]);
                }
            }
            void* limbAsset=limb && ContainsSkin(pawn,limb)?Field<void*>(limb,0x1E4):nullptr;
            void* driverAsset=Field<void*>(entry->master.load(),0x1E4);
            // AActor::AttachComponent (006878F0) records every attached
            // component in actor+3C, independently of Wellington's apparel
            // slots at +4F8. Reconcile full Hero copies in that list too.
            const int registeredCount=Field<int>(pawn,0x40);
            auto** registered=Field<void**>(pawn,0x3C);
            static std::array<void*,64> inspected{};
            static unsigned int inspectedCount=0;
            bool report=inspectedCount<inspected.size();
            for (unsigned int i=0;i<inspectedCount;++i)
                if (inspected[i]==primary) report=false;
            if (report) inspected[inspectedCount++]=primary;
            if (registeredCount>=0 && registeredCount<=256 && (!registeredCount || registered))
                for (int i=0;i<registeredCount;++i)
                {
                    void* mesh=registered[i];
                    if (!mesh || Field<std::uintptr_t>(mesh,0)!=module+SkeletalComponentVtableRva ||
                        Field<void*>(mesh,0x4C)!=pawn) continue;
                    void* asset=Field<void*>(mesh,0x1E4);
                    if (!asset) continue;
                    const bool hero=driverAsset && (asset==driverAsset ||
                        Field<std::uint64_t>(asset,0x2C)==Field<std::uint64_t>(driverAsset,0x2C));
                    if (hero && mesh!=entry->arms.load())
                    { hide(mesh,1); MaskWholeMesh(mesh,module); }
                    if (report)
                    {
                        char name[128]={},detail[320]={};
                        AssetName(asset,module,name,sizeof(name));
                        std::snprintf(detail,sizeof(detail),"pawn=%p component=%p asset=%s driver=%u arms=%u hidden=%u bones=%d materials=%d parent=%p",
                            pawn,mesh,name,hero?1u:0u,mesh==entry->arms.load()?1u:0u,
                            (Field<unsigned int>(mesh,0x128)>>2)&1u,Field<int>(asset,0x98),Field<int>(asset,0x5C),Field<void*>(mesh,0x29C));
                        hook->diagnostics_.Event("MultiplayerTheresaRegisteredRenderComponent",detail);
                    }
                }
            const bool scene=cutsceneHidden.load();
            const bool restoreScene=entry->sceneWasHidden && !scene;
            if (scene && !entry->sceneWasHidden) entry->visibleBeforeSceneCount=0;
            // Preserve native clothing visibility. Blindly unhiding this array
            // can resurrect retired layers and full Hero arm-copy meshes.
            for (int i=0;i<count;++i)
            {
                void* mesh=meshes[i];
                if (!mesh || mesh==entry->arms.load()) continue;
                void* asset=Field<void*>(mesh,0x1E4);
                if (mesh==primary || mesh==limb) { hide(mesh,scene?1:0); continue; }
                if (asset==driverAsset || asset==skinAsset || (limbAsset && asset==limbAsset))
                { hide(mesh,1); continue; } // Retired private helper, never clothing.
                if (Field<void*>(mesh,0x4C)!=pawn) continue; // Detached/retired native clothing.
                if (scene)
                {
                    if (!(Field<unsigned int>(mesh,0x128)&4u) && entry->visibleBeforeSceneCount<128)
                    {
                        bool recorded=false;
                        for (unsigned int j=0;j<entry->visibleBeforeSceneCount;++j)
                            if (entry->visibleBeforeScene[j]==mesh) recorded=true;
                        if (!recorded) entry->visibleBeforeScene[entry->visibleBeforeSceneCount++]=mesh;
                    }
                    hide(mesh,1);
                }
                else if (restoreScene)
                    for (unsigned int j=0;j<entry->visibleBeforeSceneCount;++j)
                        if (entry->visibleBeforeScene[j]==mesh) { hide(mesh,0); break; }
            }
            entry->sceneWasHidden=scene;
            if (restoreScene) entry->visibleBeforeSceneCount=0;
            if (!entry->childhood.load())
                for (int i=0;i<count;++i)
                    if (meshes[i] && Field<void*>(meshes[i],0x4C)==pawn && Field<void*>(meshes[i],0x248) &&
                        !(Field<unsigned int>(meshes[i],0x128)&4u))
                        native::TheresaGarmentShape::Apply(meshes[i]);
            if (!entry->childhood.load() && ContainsSkin(pawn,entry->skin.load()))
            {
                void* skin=entry->skin.load();
                void* asset=Field<void*>(skin,0x1E4);
                void* limbMesh=entry->limbs.load();
                // Keep the stock outfit if the generated overlay failed to
                // load: missing hands/feet are worse than the old overlap.
                const unsigned mask=limbMesh && ContainsSkin(pawn,limbMesh)?entry->outfitMask.load():0;
                void* arms=entry->arms.load();
                if (arms && ContainsSkin(pawn,arms))
                {
                    hide(arms,1);
                    void* armAsset=Field<void*>(arms,0x1E4);
                    const int lods=armAsset?Field<int>(armAsset,0xE4):0;
                    if (armAsset && Field<int>(armAsset,0x5C)==18 && lods>=1 && lods<=8)
                    {
                        using ShowSection=void(__thiscall*)(void*,int,int,int);
                        auto show=reinterpret_cast<ShowSection>(reinterpret_cast<std::uintptr_t>(hook->module_)+ShowMaterialSectionRva);
                        for (int lod=0;lod<lods;++lod)
                        {
                            // ShowMaterialSection remaps the input index for
                            // LOD>0. Select by the resulting material, so a
                            // reduced LOD cannot accidentally enable the face.
                            void* info=static_cast<std::uint8_t*>(Field<void*>(armAsset,0x130))+lod*0x48;
                            auto* remap=Field<int*>(info,8);
                            const int remapCount=Field<int>(info,0xC);
                            for (int part=0;part<18;++part)
                            {
                                int material=part;
                                if (lod>0 && remap && part<remapCount) material=remap[part];
                                if (material<0) material=0; // Native remap fallback.
                                const bool visible=(mask&1u) && IsHeroArmMaterial(material);
                                auto* lodArray=Field<std::uint8_t*>(arms,0x3D4);
                                void* componentLod=lodArray && Field<int>(arms,0x3D8)>lod?lodArray+lod*0x18:nullptr;
                                auto* flags=componentLod?Field<unsigned int*>(componentLod,0):nullptr;
                                // The native helper synchronizes rendering:
                                // avoid that work when its actual mask agrees.
                                if (!flags || Field<int>(componentLod,4)!=18 ||
                                    (material<18 && flags[material]!=(visible?0u:1u)))
                                    show(arms,part,visible?1:0,lod);
                            }
                        }
                        hide(arms,cutsceneHidden.load() || !(mask&1u)?1:0);
                    }
                }
                if (asset && Field<int>(asset,0x98)==80 && Field<int>(asset,0x5C)==4 &&
                    entry->appliedOutfitMask.load()!=mask)
                {
                    const int lodCount=Field<int>(asset,0xE4);
                    if (lodCount<1 || lodCount>8) return;
                    using ShowSection=void(__thiscall*)(void*,int,int,int);
                    auto show=reinterpret_cast<ShowSection>(reinterpret_cast<std::uintptr_t>(hook->module_)+ShowMaterialSectionRva);
                    for (int lod=0;lod<lodCount;++lod)
                    {
                        show(skin,0,(mask&1u)?0:1,lod);
                        show(skin,1,(mask&2u)?0:1,lod);
                    }
                    void* limbs=entry->limbs.load();
                    if (limbs && ContainsSkin(pawn,limbs))
                        for (int lod=0;lod<lodCount;++lod)
                        {
                            show(limbs,0,(mask&1u)?1:0,lod);
                            show(limbs,1,(mask&2u)?1:0,lod);
                            show(limbs,2,0,lod);
                            show(limbs,3,0,lod);
                        }
                    entry->appliedOutfitMask.store(mask);
                    char detail[160]={};
                    std::snprintf(detail,sizeof(detail),"torso_hidden=%u legs_hidden=%u lods=%d face_hair=preserved fit=unverified",
                        mask&1u,(mask>>1)&1u,lodCount);
                    hook->diagnostics_.Event("MultiplayerTheresaStockOutfitSectionsApplied",detail);
                    hook->diagnostics_.Event("MultiplayerTheresaBareArmSectionsApplied",
                        "separate Hero arm follower; driver stays hidden; arm visibility reconciled independently of outfit cache");
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
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
        bool limbCommand=false;
        bool armCommand=false;
        __try
        {
            if (IsSkinCommand(command))
            {
                limbCommand=std::strcmp(static_cast<char*>(command)+0x20,LimbSkinName)==0;
                armCommand=std::strcmp(static_cast<char*>(command)+0x20,ArmSkinName)==0;
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
            // Keep the animation driver hidden during world/body rebuilds;
            // showing it even briefly exposes the original Hero face.
            using Hide=void(__thiscall*)(void*,unsigned int);
            const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
            reinterpret_cast<Hide>(base+SetHiddenRva)(master,1);
            // This runs on the native graphics command thread. Resolve the
            // cooked package explicitly before asking the apparel loader to
            // attach the fully qualified object; do not rely on level preloads
            // or the legacy short-name-to-package lookup.
            using LoadPackage=void*(__cdecl*)(void*,const wchar_t*,unsigned int);
            using LoadObject=void*(__cdecl*)(void*,void*,const wchar_t*,const wchar_t*,
                unsigned int,void*,int);
            const bool child=entry->childhood.load();
            void* package=armCommand?nullptr:reinterpret_cast<LoadPackage>(base+LoadPackageRva)(nullptr,
                limbCommand?LimbSkinPackage:child?ChildSkinPackage:SkinPackage,0);
            if (armCommand)
            {
                loadedAsset=Field<void*>(master,0x1E4);
                if (!loadedAsset || Field<int>(loadedAsset,0x98)!=91 || Field<int>(loadedAsset,0x5C)!=18)
                    return;
            }
            else if (package)
            {
                void* heroAsset=Field<void*>(master,0x1E4);
                void* meshClass=heroAsset?Field<void*>(heroAsset,0x34):nullptr;
                if (meshClass)
                    loadedAsset=reinterpret_cast<LoadObject>(base+LoadObjectRva)(
                        meshClass,nullptr,limbCommand?LimbSkinObjectPath:child?ChildSkinObjectPath:SkinObjectPath,nullptr,0,nullptr,1);
            }
            char detail[192]={};
            std::snprintf(detail,sizeof(detail),"package=%p asset=%p childhood=%s",package,loadedAsset,child?"true":"false");
            hook->diagnostics_.Event(loadedAsset?"MultiplayerTheresaHeroSkinAssetLoaded":
                "MultiplayerTheresaHeroSkinAssetFailed",detail);
            if (!loadedAsset) return; // AssetFailed records a missing presentation.
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
                    Field<void*>(skin,0x29C)==master && hook->FixMap(skin,armCommand))
                {
                    if (loadedAsset)
                    {
                        // The apparel builder creates dynamic material instances
                        // and routes them through WellingtonPawn::vf328's
                        // texture projection. This body-only mesh must retain
                        // its own authored skin/boot materials, not armour UVs.
                        using SetMaterial=void(__thiscall*)(void*,int,void*);
                        auto** materials=Field<void**>(loadedAsset,0x58);
                        const int materialCount=Field<int>(loadedAsset,0x5C);
                        if (!materials || materialCount<1 || materialCount>(armCommand?18:8) ||
                            (armCommand && materialCount!=18)) return;
                        for (int i=0;i<materialCount;++i)
                            reinterpret_cast<SetMaterial>(base+SetMaterialRva)(skin,i,materials[i]);
                        hook->diagnostics_.Event(armCommand?"MultiplayerTheresaArmMaterialsRestored":limbCommand?"MultiplayerTheresaLimbMaterialsRestored":"MultiplayerTheresaSkinMaterialsRestored",
                            armCommand?"18 authored Hero materials restored on independent arm follower":
                                "authored Sister materials restored after native apparel texture projection; face/hair protected");
                    }
                    // Native vf82 gates animation-node ticking on bit80 and
                    // pose evaluation on bit20 when the driver is not drawn.
                    // Both must continue while only the follower is visible.
                    Field<std::uint32_t>(master,0x3B0)|=0xA0u;
                    reinterpret_cast<Hide>(base+SetHiddenRva)(master,1);
                    if (armCommand) entry->arms.store(skin,std::memory_order_release);
                    else if (limbCommand) entry->limbs.store(skin,std::memory_order_release);
                    else entry->skin.store(skin,std::memory_order_release);
                    entry->appliedOutfitMask.store(~0u);
                    entry->visualKey.store(0);
                    // Arm follower starts hidden until RefreshVisual has
                    // excluded its head/torso at every LOD.
                    reinterpret_cast<Hide>(base+SetHiddenRva)(skin,armCommand || cutsceneHidden.load()?1:0);
                    installed=true;
                    stage="attached";
                    if (!armCommand && !limbCommand)
                    {
                        void* nativeMesh=Field<void*>(pawn,0x460);
                        char binding[160]={};
                        std::snprintf(binding,sizeof(binding),"pawn=%p driver384=%p render460=%p sister=%p authored_materials=%d",
                            pawn,master,nativeMesh,skin,Field<int>(loadedAsset,0x5C));
                        hook->diagnostics_.Event("MultiplayerTheresaRenderBindings",binding);
                    }
                }
                else if (skin)
                {
                    // A streamed asset with unexpected bones must not draw
                    // an unbound second body or expose the Hero face.
                    reinterpret_cast<Hide>(base+SetHiddenRva)(skin,1);
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
            hook->diagnostics_.Event(installed?(armCommand?"MultiplayerTheresaArmFollowerAttached":limbCommand?"MultiplayerTheresaLimbOverlayAttached":"MultiplayerTheresaHeroSkinAttached"):
                "MultiplayerTheresaHeroSkinDeferred",detail);
        }
    }
}

#include "TheresaWeaponAttachmentHook.h"

#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/HeroPawn/Appearance/Hooks/TheresaAppearanceHook.h"
#include "Game/Native/Addresses.h"

#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    // CTCGraphicAppearance named point transform, thiscall, RET1C verified.
    constexpr std::uintptr_t BoneQueryRva = 0x0199F840;
    constexpr std::uintptr_t BoneQueryHandlerRva = 0x0252F9AC;
    // Primary slot-bone query from CTCCarrying::ResolveCarriedTransform.
    // Excludes its secondary look/aim query and all gameplay callers.
    constexpr std::uintptr_t CarryBoneQueryReturnRva = 0x01955114;
    constexpr std::uintptr_t ObjectPresentationReturnRva = 0x019537B9;
    constexpr std::uintptr_t CarryPresentationReturnRva = 0x01953831;
    constexpr std::uintptr_t WeaponComponentVtableRva = 0x02B183D4;

    struct QueryContext final
    {
        void* owner = nullptr;
        void* weapon = nullptr;
        unsigned int applied = 0;
    };
    thread_local QueryContext* currentQuery = nullptr;

    bool ResolveUnmodified(
        fable::game::hero_pawn::equipment::hooks::TheresaWeaponAttachmentHook::ResolveTransform original,
        void* attachment, float* position,
        fable::game::hero_pawn::equipment::hooks::TheresaWeaponAttachmentHook::Basis* basis,
        bool interpolate, float alpha, bool solid)
    {
        QueryContext* previous = currentQuery;
        bool result = false;
        // Nested gameplay/other-actor resolves must not inherit an outer
        // presentation query's permission to adapt the named bone.
        __try
        {
            currentQuery = nullptr;
            result = original(attachment, position, basis, interpolate, alpha, solid);
        }
        __finally { currentQuery = previous; }
        return result;
    }

    struct Vec final { float x, y, z; };
    struct Quat final { float x, y, z, w; };
    struct Grip final
    {
        const char* name;
        Quat sister;
        Vec sisterPosition;
        Quat hero;
        Vec heroPosition;
        bool scabbard;
    };

    // Actual serialized MESH_SISTER_01 / MESH_HERO local bind transforms.
    // See local/*-skeleton.json and docs/theresa-skeleton-comparison.md.
    constexpr Grip Grips[] = {
        {"WEAPON_FOCUS_01", {-0.00101541646f, 0.00272751693f, 0.999874532f, -0.0155701097f},
            {8.01620102f, -4.42562103f, 0.728581786f},
            {0.000714656024f, -0.0294250026f, 0.0107368287f, -0.999509037f},
            {11.9020090f, -5.72331476f, 0.535990357f}, false},
        {"WEAPON_FOCUS_02", {-0.00188235880f, -0.999993980f, -0.00270810514f, 0.00106377574f},
            {7.95501089f, -4.89503050f, -0.731137931f},
            {0.999026060f, 0.0328711160f, 0.0294273887f, 0.000570749398f},
            {11.7985144f, -5.54266977f, -0.492898792f}, false},
        {"WEAPON_SCABBARD_01", {0, 0, 0, 1}, {0, 0, 0},
            {0.0381625183f, 0.572335541f, 0.0485716797f, -0.817689598f},
            {38.7836609f, 11.0185194f, -17.4633274f}, true},
        {"WEAPON_SCABBARD_02", {0, 0, 0, 1}, {0, 0, 0},
            {0.818817854f, 0.0104321176f, 0.571399987f, 0.0541330688f},
            {-57.2949333f, 22.1644573f, 13.8456211f}, true},
    };

    Vec Add(Vec a, Vec b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
    Vec Scale(Vec v, float s) noexcept { return {v.x*s, v.y*s, v.z*s}; }
    Vec Cross(Vec a, Vec b) noexcept { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
    bool Normalize(Vec& v) noexcept
    {
        const float length = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (!std::isfinite(length) || length < 0.001f) return false;
        v = Scale(v, 1.0f / length);
        return true;
    }
    Quat Normalize(Quat q) noexcept
    {
        const float scale = 1.0f / std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        return {q.x*scale, q.y*scale, q.z*scale, q.w*scale};
    }
    Quat Multiply(Quat a, Quat b) noexcept
    {
        return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
    }
    Vec Rotate(Quat q, Vec v) noexcept
    {
        const Vec twiceCross = Scale(Cross({q.x,q.y,q.z},v),2.0f);
        return Add(v,Add(Scale(twiceCross,q.w),Cross({q.x,q.y,q.z},twiceCross)));
    }

    bool Adjust(const Grip& grip, float* position,
        fable::game::hero_pawn::equipment::hooks::TheresaWeaponAttachmentHook::Basis* basis) noexcept
    {
        if (position == nullptr || basis == nullptr) return false;
        __try
        {
            Vec up{basis->up[0],basis->up[1],basis->up[2]};
            Vec forward{basis->forward[0],basis->forward[1],basis->forward[2]};
            if (!Normalize(up) || !Normalize(forward)) return false;
            Vec right = Cross(forward,up);
            if (!Normalize(right)) return false;
            forward = Cross(up,right);
            const auto world = [right,forward,up](Vec v) noexcept
            { return Add(Add(Scale(right,v.x),Scale(forward,v.y)),Scale(up,v.z)); };
            const Quat sister = Normalize(grip.sister);
            const Quat inverse{-sister.x,-sister.y,-sister.z,sister.w};
            const Quat correction = Normalize(Multiply(inverse,Normalize(grip.hero)));
            const Vec offset = world(Scale(Rotate(inverse,
                Add(grip.heroPosition,Scale(grip.sisterPosition,-1))),0.01f));
            Vec newUp = world(Rotate(correction,{0,0,1}));
            Vec newForward = world(Rotate(correction,{0,1,0}));
            if (!Normalize(newUp) || !Normalize(newForward)) return false;
            const Vec moved{position[0]+offset.x,position[1]+offset.y,position[2]+offset.z};
            if (!std::isfinite(moved.x) || !std::isfinite(moved.y) || !std::isfinite(moved.z)) return false;
            position[0]=moved.x; position[1]=moved.y; position[2]=moved.z;
            basis->up[0]=newUp.x; basis->up[1]=newUp.y; basis->up[2]=newUp.z;
            basis->forward[0]=newForward.x; basis->forward[1]=newForward.y; basis->forward[2]=newForward.z;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadName(const fable::game::native::CharString* name, char (&text)[64]) noexcept
    {
        __try
        {
            if (name == nullptr || name->stringData == nullptr) return false;
            const char* data = *reinterpret_cast<const char* const*>(
                static_cast<const std::uint8_t*>(name->stringData) + 4);
            if (!data) return false;
            for (unsigned int i=0; i<sizeof(text); ++i)
            {
                text[i]=data[i];
                if (text[i]=='\0') return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

namespace fable::game::hero_pawn::equipment::hooks
{
    TheresaWeaponAttachmentHook* TheresaWeaponAttachmentHook::active_ = nullptr;

    bool TheresaWeaponAttachmentHook::Install(HMODULE module, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_ == this) return true;
        if (!module || active_) return false;
        module_ = module;
        diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        auto* target = reinterpret_cast<std::uint8_t*>(base + BoneQueryRva);
        std::uint8_t prefix[] = {0x6A,0xFF,0x68,0,0,0,0};
        const auto handler = base + BoneQueryHandlerRva;
        std::memcpy(prefix+3,&handler,sizeof(handler));
        if (std::memcmp(target,prefix,sizeof(prefix)) != 0 || target[7]!=0x64 || target[8]!=0xA1)
        {
            diagnostics_.Event("MultiplayerTheresaWeaponHookRejected","named bone-query signature mismatch");
            return false;
        }
        active_ = this;
        if (!patch_.Install(target,prefix,sizeof(prefix),reinterpret_cast<void*>(&QueryWeaponBone),sizeof(prefix)))
        { active_=nullptr; return false; }
        original_ = reinterpret_cast<BoneQuery>(patch_.Original());
        diagnostics_.Event("MultiplayerTheresaWeaponHookReady",
            "scope=visual-weapon-attachment; focus=bind-frame-delta; missing-scabbard=spine3; fit=unverified");
        return true;
    }

    bool TheresaWeaponAttachmentHook::Shutdown() noexcept
    {
        if (!patch_.Shutdown()) return false;
        if (active_==this) active_=nullptr;
        original_=nullptr;
        module_=nullptr;
        diagnosticCount_.store(0,std::memory_order_relaxed);
        diagnostics_={};
        return true;
    }

    bool TheresaWeaponAttachmentHook::ResolveAttachment(ResolveTransform original,
        void* attachment, float* position, Basis* basis, bool interpolate,
        float alpha, bool solid, bool& adapted, std::uintptr_t caller)
    {
        adapted = false;
        const auto base=reinterpret_cast<std::uintptr_t>(module_);
        // The same native resolver is also used by weapon segment/hit queries
        // (01B179F0/01B17D80). Keep those calls on their existing native path.
        if (caller!=base+ObjectPresentationReturnRva && caller!=base+CarryPresentationReturnRva)
            return ResolveUnmodified(original,attachment,position,basis,interpolate,alpha,solid);
        QueryContext context;
        __try
        {
            if (attachment)
            {
                const auto* bytes=static_cast<const std::uint8_t*>(attachment);
                context.weapon=*reinterpret_cast<void* const*>(bytes+4);
                void* carrying=*reinterpret_cast<void* const*>(bytes+0x10);
                if (carrying) context.owner=*reinterpret_cast<void**>(static_cast<std::uint8_t*>(carrying)+4);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { context={}; }
        const bool eligible = active_==this &&
            appearance::hooks::TheresaAppearanceHook::UsesTheresaPresentation(context.owner) &&
            game::entity::native::ThingComponentAccess::FindByVtableRva(
                context.weapon,module_,WeaponComponentVtableRva) != nullptr;
        if (!eligible) return ResolveUnmodified(original,attachment,position,basis,interpolate,alpha,solid);
        QueryContext* previous=currentQuery;
        bool result=false;
        __try
        {
            currentQuery=&context;
            result=original(attachment,position,basis,interpolate,alpha,solid);
        }
        __finally { currentQuery=previous; }
        adapted=context.applied!=0;
        if (diagnosticCount_.fetch_add(1,std::memory_order_relaxed)<24)
        {
            char detail[192]={};
            std::snprintf(detail,sizeof(detail),"owner=%p weapon=%p resolved=%s adapted_mask=%u solid=%s fit=unverified",
                context.owner,context.weapon,result?"true":"false",context.applied,solid?"true":"false");
            diagnostics_.Event(adapted?"MultiplayerTheresaWeaponAttachmentAdapted":"MultiplayerTheresaWeaponAttachmentNative",detail);
        }
        return result;
    }

    bool __fastcall TheresaWeaponAttachmentHook::QueryWeaponBone(void* component, void*,
        const game::native::CharString* name, int occurrence, std::uint32_t flags,
        float* position, Basis* basis, float alpha, bool solid)
    {
        auto* hook=active_;
        if (!hook || !hook->original_) return false;
        QueryContext* context=currentQuery;
        const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto base=reinterpret_cast<std::uintptr_t>(hook->module_);
        void* owner=nullptr;
        __try { if (component) owner=*reinterpret_cast<void**>(static_cast<std::uint8_t*>(component)+4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { owner=nullptr; }
        char text[64]={};
        const Grip* grip=nullptr;
        if (context && context->owner==owner && occurrence==0 &&
            caller==base+CarryBoneQueryReturnRva && ReadName(name,text))
        {
            for (const auto& candidate:Grips)
            {
                const auto length=std::strlen(candidate.name);
                if (std::strncmp(text,candidate.name,length)==0 &&
                    (text[length]=='\0' || std::strcmp(text+length,"S0")==0))
                { grip=&candidate; break; }
            }
        }
        const bool nativeResult=hook->original_(component,name,occurrence,flags,position,basis,alpha,solid);
        if (!grip) return nativeResult;
        if (nativeResult)
        {
            // A real native scabbard wins over the UPK-derived fallback.
            if (!grip->scabbard && Adjust(*grip,position,basis)) context->applied|=1;
            return true;
        }
        if (!grip->scabbard) return false;
        using Construct=game::native::CharString* (__thiscall*)(game::native::CharString*,const char*,int);
        using Destroy=void(__thiscall*)(game::native::CharString*);
        const char* parents[]={"Bip01 Spine3","Bip01_Spine3S0"};
        for (const char* parent:parents)
        {
            game::native::CharString replacement;
            reinterpret_cast<Construct>(base+game::native::rva::CharStringConstructor)(&replacement,parent,-1);
            bool found=false;
            __try { found=hook->original_(component,&replacement,occurrence,flags,position,basis,alpha,solid); }
            __finally { reinterpret_cast<Destroy>(base+game::native::rva::CharStringDestructor)(&replacement); }
            if (found && Adjust(*grip,position,basis)) { context->applied|=2; return true; }
        }
        return false;
    }
}

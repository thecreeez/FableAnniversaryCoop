#include "TheresaGarmentShape.h"
#include "Core/Hooking/CodePatch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    // Traced through native MorphTarget class registration/import and the
    // slave component's manual-morph -> active-morph -> render update path.
    constexpr std::uintptr_t MorphClassRva=0x004BB570;
    constexpr std::uintptr_t MorphSetClassRva=0x00497500;
    constexpr std::uintptr_t ConstructObjectRva=0x001E1050;
    constexpr std::uintptr_t ConstructHandlerRva=0x0239D003;
    constexpr std::uintptr_t WarnGlobalRva=0x03094C58;
    constexpr std::uintptr_t ArrayReallocateRva=0x0016B890;
    constexpr std::uintptr_t AllocatorGlobalRva=0x03094C6C;
    constexpr std::uintptr_t FNameConstructRva=0x001D1100;
    constexpr std::uintptr_t InitMorphTargetsRva=0x0070BC00;
    constexpr std::uintptr_t InitMorphHandlerRva=0x023D11A2;
    constexpr std::uintptr_t UpdateActiveMorphsRva=0x00704530;
    constexpr std::uintptr_t ActiveMorphHandlerRva=0x023D0D88;
    constexpr std::uintptr_t SetHiddenRva=0x0034F570;
    constexpr std::uintptr_t DuplicateMeshRva=0x001E2E30; // StaticDuplicateObject, eight cdecl args
    constexpr std::uintptr_t SerializeVertexBufferRva=0x00724910; // cdecl(archive, GPU buffer)
    constexpr std::uintptr_t AllocateVertexDataRva=0x0071D2F0; // GPU buffer::AllocateData
    constexpr std::uintptr_t MeshPreEditRva=0x00712F20; // release resources + wait fence
    constexpr std::uintptr_t MeshInitResourcesRva=0x007146E0;
    constexpr std::uintptr_t SetSkeletalMeshRva=0x0070C930; // thiscall(component, mesh, preserve pose)
    fable::core::hooking::InlineHook vertexSerializeHook;
    using SerializeBuffer=int(__cdecl*)(void*,void*);
    SerializeBuffer originalSerializeBuffer=nullptr;
    thread_local bool copyingGarment=false;
    struct Baseline;
    thread_local Baseline* copyingBase=nullptr;
    thread_local void* copyingSource=nullptr;

    template<class T> T& Field(void* object, std::size_t offset)
    { return *reinterpret_cast<T*>(static_cast<unsigned char*>(object)+offset); }

    struct Name final { unsigned int index=0, number=0; };
    struct Vec final { float x=0,y=0,z=0; };
    struct Vertex final { Vec position,normal; float torso=0; };
    struct Delta final { Vec position; unsigned char normal[4]{}; unsigned int index=0; };
    static_assert(sizeof(Vertex)==28 && sizeof(Delta)==20);
    template<class T> struct Array final { T* data=nullptr; int count=0,capacity=0; };
    struct MorphLod final { Array<Delta> vertices; unsigned int baseVertices=0; };
    struct ActiveMorph final { void* target=nullptr; float weight=0; };
    static_assert(sizeof(MorphLod)==16 && sizeof(ActiveMorph)==8);
    struct Profile final
    {
        float depth=6,halfWidth=7.5f,height=143,radiusX=14,radiusZ=18,rigidFactor=0.75f;
        float maxStretch=1.25f;
        float boneDepthScale=1.50f;
    };
    struct RawLod final
    {
        unsigned int stride=0,full=0,uvs=0,paintedVertices=0;
        std::vector<unsigned char> original,weighted,targets;
    };
    struct Baseline final
    {
        std::string text;
        Name name;
        Name setName,targetName,bakedName;
        unsigned int bones=0;
        std::vector<std::vector<Vertex>> lods;
        std::vector<std::vector<unsigned int>> indices;
        std::vector<std::vector<Delta>> morphs;
        std::vector<RawLod> rawLods;
        bool mismatchReported=false;
        bool materialReported=false;
        bool bakeReported=false;
        Profile settings;
        float effectiveDepth=0,measuredStretch=1;
        float requestedStretch=1;
    };
    std::vector<Baseline> baselines;
    std::uintptr_t moduleBase=0;
    fable::core::Diagnostics diagnostics;
    Profile profile;
    std::atomic_bool named{false};
    std::atomic<float> liveBoneDepth{1.50f};
    std::atomic<float> liveHips{0.45f},liveButtocks{0.35f};
    std::atomic<float> liveShoulderNarrowing{0.60f};
    std::atomic<float> liveWaistNarrowing{0.25f};
    std::atomic<float> liveTorsoNarrowing{1.35f};
    bool shapeHotkeys=false,profileLogged=false;
    std::atomic_bool sizeKeysEnabled{false};
    struct KeyRepeat final { unsigned long long nextAt=0;int direction=0; };
    KeyRepeat sizeRepeat;
    KeyRepeat hipsRepeat,buttocksRepeat;
    KeyRepeat shouldersRepeat;
    KeyRepeat waistRepeat;
    KeyRepeat torsoRepeat;
    bool bookmarkDown=false;
    unsigned int reports=0;
    int HeldSizeStep(KeyRepeat& state,int direction,unsigned long long now)
    {
        if (!direction) { state={};return 0; }
        if (direction!=state.direction)
        { state.direction=direction;state.nextAt=now+350;return direction; }
        if (now<state.nextAt) return 0;
        state.nextAt=now+100; // No frame-rate dependence or catch-up burst.
        return direction;
    }
    float SteppedSize(float previous,int direction,float minimum,float maximum)
    {
        return std::clamp(std::round((previous+direction*0.05f)*100)/100,minimum,maximum);
    }
    bool AdjustSize(std::atomic<float>& value,KeyRepeat& repeat,int increaseKey,int decreaseKey,
        float minimum,float maximum,const char* region,unsigned long long now)
    {
        const bool increase=(GetAsyncKeyState(increaseKey)&0x8000)!=0;
        const bool decrease=(GetAsyncKeyState(decreaseKey)&0x8000)!=0;
        const int direction=HeldSizeStep(repeat,increase==decrease?0:increase?1:-1,now);
        if (!direction) return false;
        const float previous=value.load();
        const float next=SteppedSize(previous,direction,minimum,maximum);
        if (std::fabs(next-previous)<0.0001f) return false;
        value.store(next);
        char detail[160]={};
        std::snprintf(detail,sizeof(detail),"region=%s value=%.2f step=.05 range=%.1f..%.1f key_vk=0x%02X session_only=1",
            region,next,minimum,maximum,direction>0?increaseKey:decreaseKey);
        diagnostics.Event("MultiplayerTheresaBodySizeChanged",detail);
        return true;
    }
    void LogBodyProfile(const char* reason)
    {
        using fable::game::hero_pawn::appearance::native::TheresaGarmentShape;
        const auto lower=TheresaGarmentShape::LowerBodyScales();
        const auto shoulder=TheresaGarmentShape::ShoulderScales();
        char detail[768]={};
        std::snprintf(detail,sizeof(detail),
            "version=4 reason=%s pid=%lu chest_depth_requested=%.2f hips=%.2f buttocks=%.2f "
            "shoulder_narrowing=%.2f clavicle_length_scale=%.3f upper_arm_scale=%.3f "
            "pelvis_width=%.3f pelvis_depth=%.3f hip_spacing=%.3f thigh_width=%.3f thigh_depth=%.3f waist_narrowing=%.2f waist_cross_scale=%.3f torso_narrowing=%.2f torso_cross_scale=%.3f hotkeys_enabled=%u config=theresa-garment-shape.ini",
            reason,GetCurrentProcessId(),liveBoneDepth.load(),liveHips.load(),liveButtocks.load(),
            liveShoulderNarrowing.load(),shoulder.clavicleLength,shoulder.upperArmScale,lower.pelvisWidth,lower.pelvisDepth,
            lower.hipSpacing,lower.thighWidth,lower.thighDepth,liveWaistNarrowing.load(),TheresaGarmentShape::WaistScale(),
            liveTorsoNarrowing.load(),TheresaGarmentShape::TorsoScale(),shapeHotkeys?1u:0u);
        diagnostics.Event("MultiplayerTheresaBodyProfile",detail);
    }
    float Push(Vec position,float weight,float depth,const Profile& shape);

    bool PaintWeights(Baseline& base)
    {
        if (base.rawLods.size()!=base.lods.size()) return false;
        unsigned int changed=0;
        for (std::size_t lod=0;lod<base.rawLods.size();++lod)
        {
            auto& raw=base.rawLods[lod];
            if (!raw.weighted.empty()) { changed+=raw.paintedVertices;continue; }
            raw.weighted=raw.original;
            for (std::size_t i=0;i<base.lods[lod].size();++i)
            {
                const auto chest=raw.targets[i*2],neutral=raw.targets[i*2+1];
                if (chest==255 || neutral==255 || chest==neutral) continue;
                auto* vertex=raw.weighted.data()+i*raw.stride;
                unsigned int pool=0,count=0;
                std::array<std::pair<unsigned char,unsigned int>,4> influences{};
                for (int j=0;j<4;++j)
                {
                    const auto bone=vertex[8+j];const auto weight=vertex[12+j];
                    if (bone==chest || bone==neutral) pool+=weight;
                    else if (weight) influences[count++]={bone,weight};
                }
                if (!pool) continue;
                const float mask=Push(base.lods[lod][i].position,1,1,base.settings);
                const auto front=static_cast<unsigned int>(std::clamp(std::lround(pool*mask),0l,static_cast<long>(pool)));
                if (count+(front?1u:0u)+(front<pool?1u:0u)>4) continue;
                if (front) influences[count++]={chest,front};
                if (front<pool) influences[count++]={neutral,pool-front};
                unsigned char painted[8]={};
                for (unsigned int j=0;j<count;++j)
                { painted[j]=influences[j].first;painted[j+4]=static_cast<unsigned char>(influences[j].second); }
                if (std::memcmp(vertex+8,painted,8))
                { std::memcpy(vertex+8,painted,8);++raw.paintedVertices; }
            }
            changed+=raw.paintedVertices;
        }
        return changed>0;
    }

    struct BufferView final
    {
        unsigned char* data=nullptr;
        unsigned int count=0,stride=0,uvBytes=0;
        bool packed=false;
        Vec origin,extent;
    };
    bool ViewBuffer(void* gpu,BufferView& view)
    {
        const unsigned int uv=Field<unsigned int>(gpu,0x3C);
        const unsigned int full=Field<unsigned int>(gpu,0x1C),packed=Field<unsigned int>(gpu,0x24);
        view={};
        if (uv<1 || uv>4 || full>1 || packed>1) return false;
        view.data=Field<unsigned char*>(gpu,0x30);
        view.count=Field<unsigned int>(gpu,0x38);
        view.stride=Field<unsigned int>(gpu,0x34);
        view.uvBytes=uv*(full?8u:4u);
        view.packed=packed!=0;
        view.origin=Field<Vec>(gpu,0x40);view.extent=Field<Vec>(gpu,0x4C);
        return view.data && view.count>0 && view.count<200000 && view.stride==(packed?20u:28u)+view.uvBytes;
    }
    [[maybe_unused]] bool NativeBufferView(void* gpu,BufferView& view)
    {
        if (!ViewBuffer(gpu,view)) return false;
        void* storage=Field<void*>(gpu,0x2C);
        if (!storage) return false;
        // Cached GPU+30/+38 alone can outlive a discarded CPU resource array.
        // Ask the owning native array, as serializer00724910 does, before any
        // read/copy. A cooked mesh with discarded data keeps stock geometry.
        using GetData=void*(__thiscall*)(void*);
        using Count=unsigned int(__thiscall*)(void*);
        return reinterpret_cast<GetData>(Field<void**>(storage,0)[3])(storage)==view.data &&
            reinterpret_cast<Count>(Field<void**>(storage,0)[4])(storage)==view.count;
    }
    Vec BufferPosition(const BufferView& view,unsigned int index)
    {
        auto* record=view.data+index*view.stride;
        if (!view.packed) return Field<Vec>(record,16);
        const unsigned int packed=Field<unsigned int>(record,16);
        const int x=static_cast<int>(packed<<21)>>21;
        const int y=static_cast<int>(packed<<10)>>21;
        const int z=static_cast<int>(packed)>>22;
        return {view.origin.x+view.extent.x*x/1023.0f,view.origin.y+view.extent.y*y/1023.0f,
            view.origin.z+view.extent.z*z/511.0f};
    }
    unsigned int PackPosition(Vec value,Vec origin,Vec extent)
    {
        // Native0066BF80: signed 11/11/10, truncation toward zero.
        const auto pack=[](float v,float o,float e,int limit)
        { return std::clamp(static_cast<int>((v-o)/e*limit),-limit,limit); };
        return (static_cast<unsigned int>(pack(value.x,origin.x,extent.x,1023))&0x7FFu) |
            ((static_cast<unsigned int>(pack(value.y,origin.y,extent.y,1023))&0x7FFu)<<11) |
            ((static_cast<unsigned int>(pack(value.z,origin.z,extent.z,511))&0x3FFu)<<22);
    }
    void CopyUnpackedStream(const BufferView& source,unsigned char* data)
    {
        const auto stride=28+source.uvBytes;
        for (unsigned int i=0;i<source.count;++i)
        {
            auto* out=data+i*stride;
            auto* in=source.data+i*source.stride;
            std::memcpy(out,in,16);
            Field<Vec>(out,16)=BufferPosition(source,i);
            std::memcpy(out+28,in+(source.packed?20:28),source.uvBytes);
        }
    }
    int SerializeCopyBuffer(void* archive,void* gpu)
    {
        // Native GPU serialization omits PC streams when saving ordinary
        // archives. A duplicate must explicitly serialize them on BOTH sides.
        // Scope the raw-stream condition to this helper, not UObject/property
        // serialization, and restore it before any other archive work.
        const auto saved=Field<unsigned int>(archive,0x68);
        int result=0;
        __try
        {
            Field<unsigned int>(archive,0x68)=1;
            result=originalSerializeBuffer(archive,gpu);
        }
        __finally { Field<unsigned int>(archive,0x68)=saved; }
        return result;
    }
    int SerializeShadow(void* archive,void* shadow,void* storage)
    {
        int result=0;
        __try { result=SerializeCopyBuffer(archive,shadow); }
        __finally
        {
            using Destroy=void(__thiscall*)(void*,unsigned int);
            reinterpret_cast<Destroy>(Field<void**>(storage,0)[0])(storage,1);
        }
        return result;
    }
    int __cdecl SerializeGarmentBuffer(void* archive,void* gpu)
    {
        if (!copyingGarment) return originalSerializeBuffer(archive,gpu);
        if (copyingBase && copyingSource && !Field<int>(archive,0x10) && Field<int>(archive,0x14))
        {
            auto** lods=Field<void**>(copyingSource,0xE0);
            for (std::size_t lod=0;lod<copyingBase->rawLods.size();++lod)
                if (gpu==static_cast<unsigned char*>(lods[lod])+0x54)
                {
                    const auto& raw=copyingBase->rawLods[lod];
                    std::array<unsigned char,0x58> shadow{};
                    std::memcpy(shadow.data(),gpu,shadow.size());
                    Field<void*>(shadow.data(),0x2C)=nullptr;
                    Field<unsigned int>(shadow.data(),0x1C)=raw.full;
                    Field<unsigned int>(shadow.data(),0x24)=0;
                    Field<unsigned int>(shadow.data(),0x3C)=raw.uvs;
                    using Allocate=void(__thiscall*)(void*);
                    reinterpret_cast<Allocate>(moduleBase+AllocateVertexDataRva)(shadow.data());
                    void* storage=Field<void*>(shadow.data(),0x2C);
                    using ResizeData=void(__thiscall*)(void*,int);
                    const auto count=static_cast<unsigned int>(copyingBase->lods[lod].size());
                    reinterpret_cast<ResizeData>(Field<void**>(storage,0)[1])(storage,static_cast<int>(count));
                    using GetData=void*(__thiscall*)(void*);
                    void* data=reinterpret_cast<GetData>(Field<void**>(storage,0)[3])(storage);
                    std::memcpy(data,raw.weighted.data(),raw.weighted.size());
                    Field<void*>(shadow.data(),0x30)=data;
                    Field<unsigned int>(shadow.data(),0x34)=raw.stride;
                    Field<unsigned int>(shadow.data(),0x38)=count;
                    return SerializeShadow(archive,shadow.data(),storage);
                }
        }
        if (copyingBase && Field<int>(archive,0x10))
        {
            // 00728590: chunks at LOD+C; 0071ED50: stride34/max influences30.
            auto* lod=static_cast<unsigned char*>(gpu)-0x54;
            auto* chunks=Field<unsigned char*>(lod,0xC);
            const int count=Field<int>(lod,0x10);
            if (chunks && count>0 && count<=32)
                for (int i=0;i<count;++i) Field<unsigned int>(chunks+i*0x34,0x30)=4;
        }
        // Duplicate archives serialize uncompressed positions. A runtime PC
        // asset may already be packed: serialize a private unpacked shadow,
        // never mutate or free the source resource array.
        BufferView source;
        if (!Field<int>(archive,0x10) && Field<int>(archive,0x14) && ViewBuffer(gpu,source) && source.packed)
        {
            std::array<unsigned char,0x58> shadow{};
            std::memcpy(shadow.data(),gpu,shadow.size());
            Field<void*>(shadow.data(),0x2C)=nullptr;
            Field<unsigned int>(shadow.data(),0x24)=0;
            using Allocate=void(__thiscall*)(void*);
            reinterpret_cast<Allocate>(moduleBase+AllocateVertexDataRva)(shadow.data());
            void* storage=Field<void*>(shadow.data(),0x2C);
            using ResizeData=void(__thiscall*)(void*,int);
            reinterpret_cast<ResizeData>(Field<void**>(storage,0)[1])(storage,static_cast<int>(source.count));
            using GetData=void*(__thiscall*)(void*);
            auto* data=static_cast<unsigned char*>(reinterpret_cast<GetData>(Field<void**>(storage,0)[3])(storage));
            const auto stride=28+source.uvBytes;
            Field<unsigned char*>(shadow.data(),0x30)=data;
            Field<unsigned int>(shadow.data(),0x34)=stride;
            Field<unsigned int>(shadow.data(),0x38)=source.count;
            CopyUnpackedStream(source,data);
            return SerializeShadow(archive,shadow.data(),storage);
        }
        return SerializeCopyBuffer(archive,gpu);
    }

    bool Same(Name a,Name b) noexcept { return a.index==b.index && a.number==b.number; }
    bool MakeName(const char* text,Name& name)
    {
        using Construct=void*(__thiscall*)(Name*,const char*,int);
        reinterpret_cast<Construct>(moduleBase+FNameConstructRva)(&name,text,1);
        return name.index!=0 || name.number!=0;
    }
    template<class T> bool Resize(Array<T>& array,int count)
    {
        if (count<0 || count>200000 || array.count<0 || array.capacity<array.count) return false;
        if (array.capacity<count)
        {
            using Reallocate=void*(__cdecl*)(void*,unsigned int,unsigned int);
            auto* data=reinterpret_cast<T*>(reinterpret_cast<Reallocate>(moduleBase+ArrayReallocateRva)(
                array.data,static_cast<unsigned int>(count*sizeof(T)),8));
            if (!data) return false;
            array.data=data;
            array.capacity=count;
        }
        array.count=count;
        return true;
    }
    Vec Normalize(Vec v)
    {
        const float size=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
        if (size<0.00001f) return {0,-1,0};
        return {v.x/size,v.y/size,v.z/size};
    }
    float Push(Vec position,float weight,float depth,const Profile& shape)
    {
        // Owner's in-game inspection establishes +model Y as the front.
        // The previous -Y assumption pushed the garment's back out instead.
        const float front=std::clamp((position.y-1)/6,0.0f,1.0f);
        const float z=(position.z-shape.height)/shape.radiusZ;
        // Two compact, smooth lobes. The center seam, sleeves, back and waist
        // taper naturally; no displacement survives outside the support.
        const auto lobe=[&](float center)
        {
            const float x=(position.x-center)/shape.radiusX;
            const float t=(std::max)(0.0f,1-x*x-z*z);
            return t*t*t;
        };
        return depth*weight*front*front*(3-2*front)*(std::max)(lobe(-shape.halfWidth),lobe(shape.halfWidth));
    }
    Delta Deform(const Vertex& vertex,unsigned int index,float depth,const Profile& shape)
    {
        Delta delta;
        delta.index=index;
        const Vec p=vertex.position;
        delta.position.y=Push(p,vertex.torso,depth,shape);
        // Inverse-transpose Jacobian transforms the authored normal, keeping
        // UV seams/hard edges; the native morph pipeline blends tangent Z.
        constexpr float epsilon=0.05f;
        const float dx=(Push({p.x+epsilon,p.y,p.z},vertex.torso,depth,shape)-Push({p.x-epsilon,p.y,p.z},vertex.torso,depth,shape))/(2*epsilon);
        const float dy=(Push({p.x,p.y+epsilon,p.z},vertex.torso,depth,shape)-Push({p.x,p.y-epsilon,p.z},vertex.torso,depth,shape))/(2*epsilon);
        const float dz=(Push({p.x,p.y,p.z+epsilon},vertex.torso,depth,shape)-Push({p.x,p.y,p.z-epsilon},vertex.torso,depth,shape))/(2*epsilon);
        const Vec old=Normalize(vertex.normal);
        const float divisor=(std::max)(0.2f,1+dy);
        const Vec normal=Normalize({old.x-dx*old.y/divisor,old.y/divisor,old.z-dz*old.y/divisor});
        const float differences[]={normal.x-old.x,normal.y-old.y,normal.z-old.z};
        for (int axis=0;axis<3;++axis)
            delta.normal[axis]=static_cast<unsigned char>(std::clamp(std::lround((differences[axis]+1)*127.5f),0l,255l));
        delta.normal[3]=127;
        return delta;
    }
    float MeasureStretch(const Baseline& base,float depth)
    {
        float maximum=1;
        for (std::size_t lod=0;lod<base.lods.size();++lod)
        {
            const auto& vertices=base.lods[lod];
            std::vector<float> pushes;
            for (const auto& vertex:vertices) pushes.push_back(Push(vertex.position,vertex.torso,depth,base.settings));
            const auto& indices=base.indices[lod];
            for (std::size_t tri=0;tri<indices.size();tri+=3)
                for (int edge=0;edge<3;++edge)
                {
                    const auto a=indices[tri+edge],b=indices[tri+(edge+1)%3];
                    const Vec p=vertices[a].position,q=vertices[b].position;
                    const float x=q.x-p.x,y=q.y-p.y,z=q.z-p.z;
                    const float old=x*x+y*y+z*z;
                    if (old<0.01f) continue; // Sub-millimeter seam/degenerate edges.
                    const float dy=y+pushes[b]-pushes[a];
                    const float now=x*x+dy*dy+z*z;
                    const float ratio=now>0.000001f?(std::max)(now/old,old/now):1000000;
                    maximum=(std::max)(maximum,ratio);
                }
        }
        return std::sqrt(maximum);
    }
    bool PrepareMorphs(Baseline& base)
    {
        if (!base.morphs.empty()) return true;
        const bool rigid=base.text.find("ARMOUR")!=std::string::npos || base.text.find("ARMOR")!=std::string::npos ||
            base.text.find("PLATINUM")!=std::string::npos || base.text.find("MINION")!=std::string::npos;
        const float requested=base.settings.depth*(rigid?base.settings.rigidFactor:1);
        float depth=requested;
        base.requestedStretch=MeasureStretch(base,depth);
        if (base.requestedStretch>base.settings.maxStretch)
        {
            float lo=0,hi=depth;
            for (int iteration=0;iteration<12;++iteration)
            {
                const float middle=(lo+hi)/2;
                if (MeasureStretch(base,middle)<=base.settings.maxStretch) lo=middle;
                else hi=middle;
            }
            depth=lo;
        }
        std::vector<std::vector<Delta>> computed;
        for (const auto& source:base.lods)
        {
            std::vector<Delta> deltas;
            for (unsigned int i=0;i<source.size();++i)
            {
                const auto delta=Deform(source[i],i,depth,base.settings);
                if (std::fabs(delta.position.y)>0.0001f) deltas.push_back(delta);
            }
            if (deltas.empty()) return false;
            computed.push_back(std::move(deltas));
        }
        base.effectiveDepth=depth;
        base.measuredStretch=MeasureStretch(base,depth);
        base.morphs=std::move(computed);
        return true;
    }
    template<class T> bool Read(std::ifstream& input,T& value)
    { return static_cast<bool>(input.read(reinterpret_cast<char*>(&value),sizeof(value))); }

    float ReadSetting(const std::wstring& path,const wchar_t* key,float fallback,float lo,float hi,const wchar_t* section=L"Chest")
    {
        wchar_t text[64]={};
        if (!GetPrivateProfileStringW(section,key,L"",text,64,path.c_str())) return fallback;
        wchar_t* end=nullptr;
        const float value=std::wcstof(text,&end);
        return end!=text && *end==0 && std::isfinite(value)?std::clamp(value,lo,hi):fallback;
    }
    void LoadShapeSettings(const std::wstring& config)
    {
        profile.depth=ReadSetting(config,L"Depth",6,0,12);
        profile.halfWidth=ReadSetting(config,L"HalfWidth",7.5f,3,13);
        profile.height=ReadSetting(config,L"Height",143,125,160);
        profile.radiusX=ReadSetting(config,L"RadiusX",14,5,18);
        profile.radiusZ=ReadSetting(config,L"RadiusZ",18,6,22);
        profile.rigidFactor=ReadSetting(config,L"RigidFactor",0.75f,0.25f,1);
        profile.maxStretch=ReadSetting(config,L"MaxStretch",1.25f,1.01f,1.5f);
        profile.boneDepthScale=ReadSetting(config,L"BoneDepthScale",1.50f,1,2);
        liveBoneDepth.store(profile.boneDepthScale);
        liveHips.store(ReadSetting(config,L"Hips",0.45f,0,1.5f,L"Body"));
        liveButtocks.store(ReadSetting(config,L"Buttocks",0.35f,0,1.5f,L"Body"));
        liveShoulderNarrowing.store(ReadSetting(config,L"ShoulderNarrowing",0.60f,0,1.5f,L"Body"));
        liveWaistNarrowing.store(ReadSetting(config,L"WaistNarrowing",0.25f,0,1.5f,L"Body"));
        liveTorsoNarrowing.store(ReadSetting(config,L"TorsoNarrowing",1.35f,0,1.5f,L"Body"));
        shapeHotkeys=ReadSetting(config,L"EnableShapeHotkeys",0,0,1,L"Controls")>=1;
    }
    bool LoadCache()
    {
        HMODULE self=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LoadCache),&self)) return false;
        wchar_t path[MAX_PATH]={};
        const DWORD length=GetModuleFileNameW(self,path,MAX_PATH);
        if (!length || length>=MAX_PATH) return false;
        const std::wstring file(path);
        const auto slash=file.find_last_of(L"\\/");
        if (slash==std::wstring::npos) return false;
        const auto folder=file.substr(0,slash+1);
        const auto config=folder+L"theresa-garment-shape.ini";
        LoadShapeSettings(config);
        std::ifstream input(folder+L"theresa-garment-geometry.bin",std::ios::binary);
        char magic[8]={};
        unsigned int count=0;
        if (!input.read(magic,8) || std::memcmp(magic,"THGEOM03",8) || !Read(input,count) || !count || count>128) return false;
        std::vector<Baseline> loaded;
        for (unsigned int i=0;i<count;++i)
        {
            Baseline base;
            unsigned int lengthName=0,lodCount=0;
            if (!Read(input,lengthName) || lengthName<10 || lengthName>120) return false;
            base.text.resize(lengthName);
            if (!input.read(base.text.data(),lengthName) || base.text.find("MESH_HERO_")!=0 ||
                !Read(input,base.bones) || !base.bones || base.bones>128 || !Read(input,lodCount) || !lodCount || lodCount>8) return false;
            const std::wstring section(base.text.begin(),base.text.end());
            auto& shape=base.settings;
            shape.depth=ReadSetting(config,L"Depth",profile.depth,0,12,section.c_str());
            shape.halfWidth=ReadSetting(config,L"HalfWidth",profile.halfWidth,3,13,section.c_str());
            shape.height=ReadSetting(config,L"Height",profile.height,125,160,section.c_str());
            shape.radiusX=ReadSetting(config,L"RadiusX",profile.radiusX,5,18,section.c_str());
            shape.radiusZ=ReadSetting(config,L"RadiusZ",profile.radiusZ,6,22,section.c_str());
            shape.rigidFactor=ReadSetting(config,L"RigidFactor",profile.rigidFactor,0.25f,1,section.c_str());
            shape.maxStretch=ReadSetting(config,L"MaxStretch",profile.maxStretch,1.01f,1.5f,section.c_str());
            for (unsigned int lod=0;lod<lodCount;++lod)
            {
                unsigned int vertices=0;
                if (!Read(input,vertices) || !vertices || vertices>65535) return false;
                std::vector<Vertex> values(vertices);
                if (!input.read(reinterpret_cast<char*>(values.data()),vertices*sizeof(Vertex))) return false;
                for (const auto& value:values)
                {
                    const float fields[]={value.position.x,value.position.y,value.position.z,value.normal.x,value.normal.y,value.normal.z,value.torso};
                    for (float field:fields) if (!std::isfinite(field) || std::fabs(field)>10000) return false;
                    if (value.torso<0 || value.torso>1) return false;
                }
                unsigned int indexCount=0;
                if (!Read(input,indexCount) || !indexCount || indexCount>600000 || indexCount%3) return false;
                std::vector<unsigned int> indices(indexCount);
                if (!input.read(reinterpret_cast<char*>(indices.data()),indexCount*sizeof(unsigned int))) return false;
                for (auto index:indices) if (index>=vertices) return false;
                RawLod raw;
                unsigned int bytes=0;
                if (!Read(input,raw.stride) || !Read(input,raw.full) || !Read(input,raw.uvs) || !Read(input,bytes) ||
                    raw.full>1 || raw.uvs<1 || raw.uvs>4 || raw.stride!=28+raw.uvs*(raw.full?8:4) || bytes!=raw.stride*vertices) return false;
                raw.original.resize(bytes);raw.targets.resize(vertices*2);
                if (!input.read(reinterpret_cast<char*>(raw.original.data()),bytes) ||
                    !input.read(reinterpret_cast<char*>(raw.targets.data()),vertices*2)) return false;
                for (unsigned int v=0;v<vertices;++v)
                {
                    auto* record=raw.original.data()+v*raw.stride;
                    if (std::memcmp(record+16,&values[v].position,12)) return false;
                    if (record[12]+record[13]+record[14]+record[15]!=255) return false;
                    for (unsigned int j=0;j<2;++j)
                        if (raw.targets[v*2+j]!=255 && raw.targets[v*2+j]>=base.bones) return false;
                }
                base.rawLods.push_back(std::move(raw));
                base.indices.push_back(std::move(indices));
                base.lods.push_back(std::move(values));
            }
            loaded.push_back(std::move(base));
        }
        if (input.peek()!=std::char_traits<char>::eof()) return false;
        baselines=std::move(loaded);
        return true;
    }

    bool ValidateHelpers()
    {
        const auto bytes=[&](std::uintptr_t rva,const unsigned char* expected,std::size_t size)
        { return !std::memcmp(reinterpret_cast<void*>(moduleBase+rva),expected,size); };
        const auto getter=[&](std::uintptr_t rva,std::uintptr_t global)
        {
            unsigned char signature[]={0xA1,0,0,0,0,0x85,0xC0};
            const auto address=static_cast<unsigned int>(moduleBase+global);
            std::memcpy(signature+1,&address,4);
            return bytes(rva,signature,sizeof(signature));
        };
        const auto seh=[&](std::uintptr_t rva,std::uintptr_t handler,bool aligned)
        {
            unsigned char signature[]={0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x6A,0xFF,0x68,0,0,0,0};
            const auto address=static_cast<unsigned int>(moduleBase+handler);
            std::memcpy(signature+9,&address,4);
            return aligned?bytes(rva,signature,sizeof(signature)):bytes(rva,signature+6,7);
        };
        unsigned char alloc[]={0x8B,0x0D,0,0,0,0,0x85,0xC9};
        const auto allocator=static_cast<unsigned int>(moduleBase+AllocatorGlobalRva);
        std::memcpy(alloc+2,&allocator,4);
        const unsigned char name[]={0x8B,0x44,0x24,0x08,0x56,0x50};
        const unsigned char hidden[]={0x8B,0x81,0x28,0x01,0,0};
        const unsigned char preEdit[]={0x8B,0x44,0x24,0x04,0x56,0x50,0x8B,0xF1};
        const unsigned char initResources[]={0x51,0x53,0x55,0x33,0xED,0x8B,0xD9};
        const unsigned char setMesh[]={0x55,0x8B,0xEC,0x83,0xE4,0xF0};
        const unsigned char allocateData[]={0x56,0x8B,0xF1,0x8B,0x4E,0x2C};
        return getter(MorphClassRva,0x0315EEA0) && getter(MorphSetClassRva,0x0315EEA4) &&
            seh(ConstructObjectRva,ConstructHandlerRva,false) && seh(InitMorphTargetsRva,InitMorphHandlerRva,true) &&
            seh(UpdateActiveMorphsRva,ActiveMorphHandlerRva,true) && bytes(ArrayReallocateRva,alloc,sizeof(alloc)) &&
            bytes(FNameConstructRva,name,sizeof(name)) && bytes(SetHiddenRva,hidden,sizeof(hidden)) &&
            seh(DuplicateMeshRva,0x0239D289,false) &&
            bytes(MeshPreEditRva,preEdit,sizeof(preEdit)) && bytes(MeshInitResourcesRva,initResources,sizeof(initResources)) &&
            bytes(SetSkeletalMeshRva,setMesh,sizeof(setMesh)) && bytes(AllocateVertexDataRva,allocateData,sizeof(allocateData));
    }

    void* Create(void* type,void* outer,Name name)
    {
        using Construct=void*(__cdecl*)(void*,void*,unsigned int,unsigned int,unsigned int,unsigned int,void*,void*,void*,void*);
        return reinterpret_cast<Construct>(moduleBase+ConstructObjectRva)(type,outer,name.index,name.number,0,0,nullptr,
            *reinterpret_cast<void**>(moduleBase+WarnGlobalRva),nullptr,nullptr);
    }
    bool SupportsMorphMaterials(void* component,Baseline& base)
    {
        // UMaterialInterface::GetMaterial (vf94), traced in 00308370.
        // Runtime cannot compile a missing usage permutation: 002E4C20
        // rejects it outside the editor. Never force a usage bit on a cooked
        // material or replace the garment's texture with a default material.
        void* asset=Field<void*>(component,0x1E4);
        const int count=Field<int>(asset,0x5C);
        auto** authored=Field<void**>(asset,0x58);
        const int bound=Field<int>(component,0x1DC);
        auto** overrides=Field<void**>(component,0x1D8);
        if (count<1 || count>64 || !authored || bound<0 || bound>64) return false;
        for (int part=0;part<count;++part)
        {
            void* material=bound>part && overrides && overrides[part]?overrides[part]:authored[part];
            if (!material) return false;
            using GetMaterial=void*(__thiscall*)(void*);
            void* parent=reinterpret_cast<GetMaterial>(Field<void**>(material,0)[94])(material);
            if (!parent) return false;
            // Usage13=bUsedWithMorphTargets: 002E20A0 jump table;
            // 002E0C80 reads +644 bit3. Special engine materials accept all.
            const bool supported=(Field<unsigned int>(parent,0x644)&8u)!=0 ||
                (Field<unsigned int>(parent,0x640)&0x40000u)!=0;
            if (!supported)
            {
                if (!base.materialReported)
                {
                    base.materialReported=true;
                    char detail[256]={};
                    std::snprintf(detail,sizeof(detail),"mesh=%s section=%d material=%p base=%p morph_usage=0 geometry=stock textures=preserved; cooked shader permutation required",
                        base.text.c_str(),part,material,parent);
                    diagnostics.Event("MultiplayerTheresaGarmentMorphMaterialRejected",detail);
                }
                return false;
            }
        }
        return true;
    }
    bool PruneGarmentMorphs(Array<ActiveMorph>& manual,void* keep)
    {
        if (manual.count<0 || manual.count>128 || (manual.count && !manual.data)) return false;
        int retained=0;
        for (int i=0;i<manual.count;++i)
        {
            void* target=manual.data[i].target;
            bool ours=false;
            if (target && target!=keep)
                for (const auto& base:baselines)
                    if (Same(Field<Name>(target,0x2C),base.targetName)) { ours=true; break; }
            if (!ours) manual.data[retained++]=manual.data[i];
        }
        if (retained==manual.count) return false;
        manual.count=retained;
        return true;
    }
    void RetireOtherGarmentMorphs(void* component,void* keep)
    {
        if (!PruneGarmentMorphs(Field<Array<ActiveMorph>>(component,0x2DC),keep)) return;
        using Refresh=void(__thiscall*)(void*);
        reinterpret_cast<Refresh>(moduleBase+UpdateActiveMorphsRva)(component);
        // Retire the old component-private GPU morph resources through the
        // native reattachment path; keep UObjects referenced until native GC.
        using Hide=void(__thiscall*)(void*,unsigned int);
        auto hide=reinterpret_cast<Hide>(moduleBase+SetHiddenRva);
        const unsigned int hidden=(Field<unsigned int>(component,0x128)>>2)&1u;
        hide(component,hidden^1u);
        hide(component,hidden);
        diagnostics.Event("MultiplayerTheresaGarmentMorphRetired","previous garment deformation removed; unrelated native morphs preserved");
    }
    [[maybe_unused]] bool BakeBuffer(void* gpu,const std::vector<Vertex>& vertices,float depth,const Profile& shape)
    {
        BufferView view;
        if (!ViewBuffer(gpu,view) || view.count!=vertices.size()) return false;
        std::vector<Vec> positions;
        Vec lo{10000,10000,10000},hi{-10000,-10000,-10000};
        for (unsigned int i=0;i<view.count;++i)
        {
            Vec p=BufferPosition(view,i);
            const Vec authored=vertices[i].position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                std::fabs(p.x-authored.x)>0.3f || std::fabs(p.y-authored.y)>0.3f || std::fabs(p.z-authored.z)>0.3f) return false;
            p.y+=Push(authored,vertices[i].torso,depth,shape);
            positions.push_back(p);
            lo={(std::min)(lo.x,p.x),(std::min)(lo.y,p.y),(std::min)(lo.z,p.z)};
            hi={(std::max)(hi.x,p.x),(std::max)(hi.y,p.y),(std::max)(hi.z,p.z)};
        }
        const Vec origin{(lo.x+hi.x)/2,(lo.y+hi.y)/2,(lo.z+hi.z)/2};
        const Vec extent{std::ceil((hi.x-lo.x)/2+1),std::ceil((hi.y-lo.y)/2+1),std::ceil((hi.z-lo.z)/2+1)};
        for (unsigned int i=0;i<view.count;++i)
        {
            auto* record=view.data+i*view.stride;
            if (view.packed) Field<unsigned int>(record,16)=PackPosition(positions[i],origin,extent);
            else Field<Vec>(record,16)=positions[i];
            const auto delta=Deform(vertices[i],i,depth,shape);
            if (delta.position.y<=0.0001f) continue;
            const auto unpack=[&](int offset)
            { return Vec{record[offset]/127.5f-1,record[offset+1]/127.5f-1,record[offset+2]/127.5f-1}; };
            Vec normal=unpack(4);
            normal=Normalize({normal.x+delta.normal[0]/127.5f-1,
                normal.y+delta.normal[1]/127.5f-1,normal.z+delta.normal[2]/127.5f-1});
            Vec tangent=unpack(0);
            const float dot=tangent.x*normal.x+tangent.y*normal.y+tangent.z*normal.z;
            tangent=Normalize({tangent.x-dot*normal.x,tangent.y-dot*normal.y,tangent.z-dot*normal.z});
            const float components[]={tangent.x,tangent.y,tangent.z,normal.x,normal.y,normal.z};
            for (int axis=0;axis<6;++axis)
                record[axis<3?axis:axis+1]=static_cast<unsigned char>(std::clamp(std::lround((components[axis]+1)*127.5f),0l,255l));
            // Tangent handedness bytes3/7, bone indices/weights8..15 and ALL
            // texture-coordinate channels after the position stay untouched.
        }
        if (view.packed) { Field<Vec>(gpu,0x40)=origin;Field<Vec>(gpu,0x4C)=extent; }
        return true;
    }
    void* DuplicatePrivate(void* asset,void* outer,const wchar_t* name,Baseline& base)
    {
        using Duplicate=void*(__cdecl*)(void*,void*,void*,const wchar_t*,unsigned int,unsigned int,void*,void*);
        void* copy=nullptr;
        const bool previous=copyingGarment;
        auto* previousBase=copyingBase;
        void* previousSource=copyingSource;
        __try
        {
            copyingGarment=true;
            copyingBase=&base;copyingSource=asset;
            copy=reinterpret_cast<Duplicate>(moduleBase+DuplicateMeshRva)(asset,asset,outer,name,0xFFFFFFFFu,0xFFFFFFFFu,nullptr,nullptr);
        }
        __finally { copyingGarment=previous;copyingBase=previousBase;copyingSource=previousSource; }
        return copy;
    }
    bool BakePrivateMesh(void* component,Baseline& base)
    {
        const auto reject=[&](const char* reason)
        {
            if (!base.bakeReported)
            {
                base.bakeReported=true;
                char detail[256]={};
                std::snprintf(detail,sizeof(detail),"mesh=%s reason=%s; stock geometry retained; morph shader disabled",base.text.c_str(),reason);
                diagnostics.Event("MultiplayerTheresaGarmentPrivateMeshRejected",detail);
            }
            return false;
        };
        void* source=Field<void*>(component,0x1E4);
        auto& sets=Field<Array<void*>>(component,0x2C4);
        if (sets.count<0 || sets.count>128 || (sets.count && !sets.data)) return false;
        using SetMesh=void(__thiscall*)(void*,void*,int);
        for (int i=0;i<sets.count;++i)
            if (sets.data[i] && Same(Field<Name>(sets.data[i],0x2C),base.bakedName))
            {
                void* copy=Field<void*>(sets.data[i],0x48);
                if (!copy || Field<void*>(copy,0x28)!=sets.data[i]) return reject("previous clone incomplete");
                if (copy!=source) reinterpret_cast<SetMesh>(moduleBase+SetSkeletalMeshRva)(component,copy,1);
                return true;
            }
        if (!originalSerializeBuffer || (Field<unsigned int>(source,0x16C)&8u)) return reject("no serializer or CPU-skinned source");
        auto** sourceLods=Field<void**>(source,0xE0);
        if (!PaintWeights(base)) return reject("no eligible chest weights in cached palettes");
        RetireOtherGarmentMorphs(component,nullptr);
        using GetClass=void*(__cdecl*)();
        void* holder=Create(reinterpret_cast<GetClass>(moduleBase+MorphSetClassRva)(),component,base.bakedName);
        if (!holder || !Resize(sets,sets.count+1)) return reject("native holder allocation failed");
        Field<void*>(holder,0x48)=source;
        sets.data[sets.count-1]=holder; // Reflected GC reference, no manual/root pointer registry.
        const std::wstring name(base.text.begin(),base.text.end());
        void* copy=DuplicatePrivate(source,holder,name.c_str(),base);
        if (!copy || copy==source) return reject("native duplication failed");
        Field<void*>(holder,0x48)=copy; // Keep alive across resource-fence waits.
        auto** lods=Field<void**>(copy,0xE0);
        bool valid=Field<int>(copy,0xE4)==static_cast<int>(base.lods.size()) && lods && lods!=sourceLods &&
            Field<unsigned int>(copy,0x98)==base.bones && Field<int>(copy,0x5C)==Field<int>(source,0x5C);
        if (valid)
            for (int part=0;part<Field<int>(source,0x5C);++part)
                if (Field<void**>(copy,0x58)[part]!=Field<void**>(source,0x58)[part]) valid=false;
        if (valid)
            for (std::size_t lod=0;lod<base.lods.size();++lod)
            {
                void* a=lods[lod]?static_cast<unsigned char*>(lods[lod])+0x54:nullptr;
                void* b=static_cast<unsigned char*>(sourceLods[lod])+0x54;
                if (!lods[lod] || lods[lod]==sourceLods[lod] ||
                    !Field<void*>(a,0x2C) || Field<void*>(a,0x2C)==Field<void*>(b,0x2C) ||
                    Field<unsigned int>(a,0x38)!=base.lods[lod].size()) valid=false;
            }
        if (!valid) { Field<void*>(holder,0x48)=source;return reject("clone geometry/material isolation mismatch"); }
        // Weights were injected while reading the private duplicate, BEFORE
        // native PostLoad uploads its ordinary GPU resources. No live RHI edit.
        reinterpret_cast<SetMesh>(moduleBase+SetSkeletalMeshRva)(component,copy,1);
        char detail[256]={};
        unsigned int painted=0;for (const auto& raw:base.rawLods) painted+=raw.paintedVertices;
        std::snprintf(detail,sizeof(detail),"mesh=%s component=%p original=%p private=%p weights_changed=%u bone_depth=%.2f shader=ordinary materials=original UV=preserved source=disk_cache appearance=unverified",
            base.text.c_str(),component,source,copy,painted,profile.boneDepthScale);
        diagnostics.Event("MultiplayerTheresaGarmentPrivateMeshAttached",detail);
        return true;
    }
    void ApplyImpl(void* component)
    {
        if (!moduleBase || !component || baselines.empty() || profile.depth<=0) return;
        void* asset=Field<void*>(component,0x1E4);
        if (!asset || !Field<void*>(component,0x29C)) return;
        if (!named)
        {
            for (auto& base:baselines)
                if (!MakeName(base.text.c_str(),base.name) ||
                    !MakeName(("TheresaGarmentV3_"+base.text).c_str(),base.setName) ||
                    !MakeName(("TheresaChestV3_"+base.text).c_str(),base.targetName) ||
                    !MakeName(("TheresaWeightsV1_"+base.text).c_str(),base.bakedName)) return;
            named=true;
        }
        const Name name=Field<Name>(asset,0x2C);
        Baseline* base=nullptr;
        for (auto& candidate:baselines) if (Same(candidate.name,name)) { base=&candidate; break; }
        if (!base || base->text.find("LADY_GREY")!=std::string::npos || base->text.find("DRESS_")!=std::string::npos ||
            base->text.find("WHISPER")!=std::string::npos)
        { RetireOtherGarmentMorphs(component,nullptr); return; } // Already authored for female bodies.
        if (base->settings.depth<=0) { RetireOtherGarmentMorphs(component,nullptr); return; }
        bool matches=Field<unsigned int>(asset,0x98)==base->bones && Field<int>(asset,0xE4)==static_cast<int>(base->lods.size());
        auto** lods=Field<void**>(asset,0xE0);
        if (!lods) matches=false;
        if (matches)
            for (std::size_t lod=0;lod<base->lods.size();++lod)
                if (!lods[lod] || Field<unsigned int>(lods[lod],0x40)!=base->lods[lod].size()) matches=false;
        if (!matches)
        {
            RetireOtherGarmentMorphs(component,nullptr);
            if (!base->mismatchReported)
            {
                base->mismatchReported=true;
                diagnostics.Event("MultiplayerTheresaGarmentMorphLayoutRejected",base->text.c_str());
            }
            return;
        }
        if (!PrepareMorphs(*base)) return;
        RetireOtherGarmentMorphs(component,nullptr);
        if (BakePrivateMesh(component,*base)) return;
        // Never silently re-enable the rejected morph-shader route.
        (void)SupportsMorphMaterials(component,*base);

    }
}

namespace fable::game::hero_pawn::appearance::native
{
    bool TheresaGarmentShape::Initialize(HMODULE module,const core::Diagnostics& log) noexcept
    {
        Shutdown();
        diagnostics=log;
        moduleBase=reinterpret_cast<std::uintptr_t>(module);
        try
        {
            if (moduleBase && ValidateHelpers() && LoadCache())
            {
                liveBoneDepth.store(profile.boneDepthScale);
                unsigned char signature[]={0x6A,0xFF,0x68,0,0,0,0};
                const auto handler=static_cast<unsigned int>(moduleBase+0x023E0168);
                std::memcpy(signature+3,&handler,4);
                if (vertexSerializeHook.Install(reinterpret_cast<void*>(moduleBase+SerializeVertexBufferRva),
                    signature,sizeof(signature),reinterpret_cast<void*>(&SerializeGarmentBuffer),sizeof(signature)))
                {
                    originalSerializeBuffer=reinterpret_cast<SerializeBuffer>(vertexSerializeHook.Original());
                    diagnostics.Event("MultiplayerTheresaGarmentMorphReady","v5 cached source streams; private painted chest weights; native bone depth; ordinary shader; original UV/materials");
                    return true;
                }
            }
        }
        catch (...) {}
        moduleBase=0;
        baselines.clear();
        diagnostics.Event("MultiplayerTheresaGarmentMorphUnavailable","native signature mismatch or missing/invalid geometry cache");
        return false;
    }
    void TheresaGarmentShape::Shutdown() noexcept
    {
        if (!vertexSerializeHook.Shutdown())
        { diagnostics.Event("MultiplayerTheresaGarmentShutdownDeferred","vertex serializer is owned by another patch");return; }
        originalSerializeBuffer=nullptr;
        copyingGarment=false;
        copyingBase=nullptr;copyingSource=nullptr;
        sizeKeysEnabled.store(false);
        profileLogged=false;shapeHotkeys=false;
        sizeRepeat={};
        hipsRepeat={};buttocksRepeat={};
        shouldersRepeat={};waistRepeat={};torsoRepeat={};bookmarkDown=false;
        // Native objects/resources remain component-owned and GC-referenced.
        moduleBase=0;
        baselines.clear();
        named=false;
        profile={};reports=0;
    }
    void TheresaGarmentShape::Apply(void* component) noexcept
    {
        __try { ApplyImpl(component); }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            if (reports++<64) diagnostics.Event("MultiplayerTheresaGarmentMorphRejected","unexpected native component layout; source asset unchanged");
        }
    }
    float TheresaGarmentShape::BoneDepthScale(void* master) noexcept
    {
        if (!moduleBase || !named || !master || profile.depth<=0) return 1;
        __try
        {
            void* pawn=Field<void*>(master,0x4C);
            if (!pawn) return 1;
            const int count=Field<int>(pawn,0x4FC);
            auto** meshes=Field<void**>(pawn,0x4F8);
            if (count<0 || count>128 || (count && !meshes)) return 1;
            for (int i=0;i<count;++i)
            {
                void* mesh=meshes[i];
                if (!mesh || Field<void*>(mesh,0x29C)!=master || (Field<unsigned int>(mesh,0x128)&4u)) continue;
                void* asset=Field<void*>(mesh,0x1E4);
                void* holder=asset?Field<void*>(asset,0x28):nullptr;
                if (!holder) continue;
                for (const auto& base:baselines)
                    if (Same(Field<Name>(holder,0x2C),base.bakedName) && Field<void*>(holder,0x48)==asset)
                        return liveBoneDepth.load();
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
    TheresaGarmentShape::LowerBodyProfile TheresaGarmentShape::LowerBodyScales() noexcept
    {
        const float hips=std::clamp(liveHips.load(),0.0f,1.5f);
        const float buttocks=std::clamp(liveButtocks.load(),0.0f,1.5f);
        // Authored pelvis Y is depth, Z is width. Thigh X is longitudinal:
        // leave it unchanged so the native calf translation/leg length stays put.
        return {1+0.18f*hips,1+0.22f*buttocks,1+0.12f*hips,1+0.12f*hips,1+0.12f*buttocks};
    }
    bool TheresaGarmentShape::CaptureSizeKey(unsigned int message,unsigned int key) noexcept
    {
        return sizeKeysEnabled.load() && (key==VK_F1 || key==VK_F2 || key==VK_F3 || key==VK_F4 ||
            key==VK_F6 || key==VK_F7 || key==VK_F9 || key==VK_F10 || key==VK_F11 || key==VK_OEM_4 || key==VK_OEM_6) &&
            (message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP);
    }
    bool TheresaGarmentShape::TickSizeHotkeys() noexcept
    {
        if (!moduleBase) return false;
        if (!profileLogged) { LogBodyProfile("initial_from_ini");profileLogged=true; }
        sizeKeysEnabled.store(shapeHotkeys);
        if (!shapeHotkeys)
        { sizeRepeat={};hipsRepeat={};buttocksRepeat={};shouldersRepeat={};waistRepeat={};torsoRepeat={};bookmarkDown=false;return false; }
        DWORD process=0;
        const HWND foreground=GetForegroundWindow();
        if (foreground) GetWindowThreadProcessId(foreground,&process);
        if (process!=GetCurrentProcessId())
        { sizeRepeat={};hipsRepeat={};buttocksRepeat={};shouldersRepeat={};waistRepeat={};torsoRepeat={};bookmarkDown=false;return false; }
        const auto now=GetTickCount64();
        const bool chest=AdjustSize(liveBoneDepth,sizeRepeat,VK_F1,VK_F2,1,2,"chest",now);
        const bool hips=AdjustSize(liveHips,hipsRepeat,VK_F3,VK_F4,0,1.5f,"hips",now);
        const bool buttocks=AdjustSize(liveButtocks,buttocksRepeat,VK_F6,VK_F7,0,1.5f,"buttocks",now);
        const bool shoulders=AdjustSize(liveShoulderNarrowing,shouldersRepeat,VK_F9,VK_F10,0,1.5f,"shoulder_narrowing",now);
        const bool shift=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
        if (shift) waistRepeat={};else torsoRepeat={};
        const bool waist=!shift && AdjustSize(liveWaistNarrowing,waistRepeat,VK_OEM_4,VK_OEM_6,0,1.5f,"waist_narrowing",now);
        const bool torso=shift && AdjustSize(liveTorsoNarrowing,torsoRepeat,VK_OEM_4,VK_OEM_6,0,1.5f,"torso_narrowing",now);
        const bool bookmark=(GetAsyncKeyState(VK_F11)&0x8000)!=0;
        if (bookmark && !bookmarkDown) LogBodyProfile("bookmark_F11");
        else if (chest || hips || buttocks || shoulders || waist || torso) LogBodyProfile("changed");
        bookmarkDown=bookmark;
        return chest || hips || buttocks || shoulders || waist || torso;
    }
    float TheresaGarmentShape::TorsoScale() noexcept
    {
        return 1.0f-0.18f*std::clamp(liveTorsoNarrowing.load(),0.0f,1.5f);
    }
    float TheresaGarmentShape::WaistScale() noexcept
    {
        // Spine1 local X is longitudinal. Narrow only its cross section;
        // chest, clavicle, pelvis and limb factors stay independent.
        return 1.0f-0.20f*std::clamp(liveWaistNarrowing.load(),0.0f,1.5f);
    }
    TheresaGarmentShape::ShoulderProfile TheresaGarmentShape::ShoulderScales() noexcept
    {
        // Native006DED70 does not inherit mesh scale. An inverse UpperArm
        // factor lengthens its ForeArm child translation instead of cancelling
        // the clavicle scale. Only shorten clavicle X; arms stay neutral.
        return {0.86f-0.12f*std::clamp(liveShoulderNarrowing.load(),0.0f,1.5f),1.0f};
    }
}

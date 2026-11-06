// Offline geometry verifier: includes the production deformation so no game,
// injection, native object construction or engine helper is ever executed.
#include "../Client/Game/HeroPawn/Appearance/Native/TheresaGarmentShape.cpp"

namespace
{
    void* __fastcall MockMaterial(void* object,void*) { return object; }
    bool VerifyMaterialGate()
    {
        std::array<unsigned char,0x700> supported{},unsupported{},special{},mesh{},asset{};
        void* table[95]{};
        table[94]=reinterpret_cast<void*>(&MockMaterial);
        for (void* material:{static_cast<void*>(supported.data()),static_cast<void*>(unsupported.data()),static_cast<void*>(special.data())})
            Field<void**>(material,0)=table;
        Field<unsigned int>(supported.data(),0x644)=8;
        Field<unsigned int>(special.data(),0x640)=0x40000;
        // Skeletal/APEX support alone must not pass the morph-specific gate.
        Field<unsigned int>(unsupported.data(),0x640)=0x80000;
        Field<unsigned int>(unsupported.data(),0x644)=0x80;
        void* authored[]={supported.data(),unsupported.data()};
        void* overrides[]={nullptr,supported.data()};
        Field<void*>(mesh.data(),0x1E4)=asset.data();
        Field<void**>(asset.data(),0x58)=authored;
        Field<int>(asset.data(),0x5C)=2;
        Baseline baseline;
        if (SupportsMorphMaterials(mesh.data(),baseline)) return false;
        Field<void**>(mesh.data(),0x1D8)=overrides;
        Field<int>(mesh.data(),0x1DC)=2;
        if (!SupportsMorphMaterials(mesh.data(),baseline)) return false;
        overrides[1]=special.data();
        if (!SupportsMorphMaterials(mesh.data(),baseline)) return false;
        overrides[0]=unsupported.data();
        return !SupportsMorphMaterials(mesh.data(),baseline);
    }
    bool VerifyOutfitRetirement()
    {
        std::array<unsigned char,0x60> oldTarget{},currentTarget{},nativeTarget{};
        Baseline first,second;
        first.targetName={1001,0};second.targetName={1002,0};
        Field<Name>(oldTarget.data(),0x2C)=first.targetName;
        Field<Name>(currentTarget.data(),0x2C)=second.targetName;
        Field<Name>(nativeTarget.data(),0x2C)={99,0};
        baselines.push_back(first);baselines.push_back(second);
        ActiveMorph storage[]={{oldTarget.data(),1},{nativeTarget.data(),0.4f},{currentTarget.data(),1}};
        Array<ActiveMorph> manual{storage,3,3};
        const bool pruned=PruneGarmentMorphs(manual,currentTarget.data());
        const bool kept=manual.count==2 && storage[0].target==nativeTarget.data() && storage[0].weight==0.4f &&
            storage[1].target==currentTarget.data() && !PruneGarmentMorphs(manual,currentTarget.data());
        const bool removed=PruneGarmentMorphs(manual,nullptr) && manual.count==1 && storage[0].target==nativeTarget.data();
        baselines.clear();
        return pruned && kept && removed;
    }
    int __cdecl MockSerialize(void* archive,void*)
    { return Field<unsigned int>(archive,0x68)==1?42:17; }
    void* __fastcall MockVertexData(void* storage,void*) { return Field<void*>(storage,4); }
    unsigned int __fastcall MockVertexCount(void* storage,void*) { return Field<unsigned int>(storage,8); }
    bool VerifyDiscardedStream()
    {
        std::array<unsigned char,0x58> gpu{};
        std::array<unsigned char,32> bytes{},storage{};
        void* table[5]{};
        table[3]=reinterpret_cast<void*>(&MockVertexData);table[4]=reinterpret_cast<void*>(&MockVertexCount);
        Field<void**>(storage.data(),0)=table;
        Field<void*>(storage.data(),4)=bytes.data();Field<unsigned int>(storage.data(),8)=1;
        Field<void*>(gpu.data(),0x2C)=storage.data();Field<void*>(gpu.data(),0x30)=bytes.data();
        Field<unsigned int>(gpu.data(),0x34)=32;Field<unsigned int>(gpu.data(),0x38)=1;Field<unsigned int>(gpu.data(),0x3C)=1;
        BufferView view;
        if (!NativeBufferView(gpu.data(),view)) return false;
        Field<unsigned int>(storage.data(),8)=0;
        if (NativeBufferView(gpu.data(),view)) return false;
        Field<unsigned int>(storage.data(),8)=1;Field<void*>(storage.data(),4)=nullptr;
        return !NativeBufferView(gpu.data(),view);
    }
    bool VerifyArchiveScope()
    {
        std::array<unsigned char,0x90> archive{},gpu{};
        originalSerializeBuffer=&MockSerialize;
        Field<unsigned int>(archive.data(),0x68)=7;
        Field<int>(archive.data(),0x10)=1;
        const bool normal=SerializeGarmentBuffer(archive.data(),gpu.data())==17;
        copyingGarment=true;
        const bool copied=SerializeGarmentBuffer(archive.data(),gpu.data())==42 && Field<unsigned int>(archive.data(),0x68)==7;
        copyingGarment=false;originalSerializeBuffer=nullptr;
        return normal && copied;
    }
    bool VerifyBakedStreams(const Baseline& base,const std::vector<Vertex>& vertices)
    {
        Vec lo{10000,10000,10000},hi{-10000,-10000,-10000};
        for (const auto& v:vertices)
        {
            lo={(std::min)(lo.x,v.position.x),(std::min)(lo.y,v.position.y),(std::min)(lo.z,v.position.z)};
            hi={(std::max)(hi.x,v.position.x),(std::max)(hi.y,v.position.y),(std::max)(hi.z,v.position.z)};
        }
        const Vec origin{(lo.x+hi.x)/2,(lo.y+hi.y)/2,(lo.z+hi.z)/2};
        const Vec extent{std::ceil((hi.x-lo.x)/2+1),std::ceil((hi.y-lo.y)/2+1),std::ceil((hi.z-lo.z)/2+1)};
        for (unsigned int full=0;full<2;++full)
            for (unsigned int packed=0;packed<2;++packed)
                for (unsigned int channels=1;channels<=4;++channels)
                {
                    const unsigned int uvBytes=channels*(full?8u:4u),positionEnd=packed?20u:28u,stride=positionEnd+uvBytes;
                    std::vector<unsigned char> data(vertices.size()*stride);
                    std::array<unsigned char,0x58> gpu{};
                    Field<unsigned int>(gpu.data(),0x1C)=full;
                    Field<unsigned int>(gpu.data(),0x24)=packed;
                    Field<unsigned char*>(gpu.data(),0x30)=data.data();
                    Field<unsigned int>(gpu.data(),0x34)=stride;
                    Field<unsigned int>(gpu.data(),0x38)=static_cast<unsigned int>(vertices.size());
                    Field<unsigned int>(gpu.data(),0x3C)=channels;
                    Field<Vec>(gpu.data(),0x40)=origin;Field<Vec>(gpu.data(),0x4C)=extent;
                    for (unsigned int i=0;i<vertices.size();++i)
                    {
                        auto* record=data.data()+i*stride;
                        const Vec normal=Normalize(vertices[i].normal);
                        const Vec tangent=Normalize({normal.z,0,-normal.x});
                        const float basis[]={tangent.x,tangent.y,tangent.z,normal.x,normal.y,normal.z};
                        for (int axis=0;axis<6;++axis)
                            record[axis<3?axis:axis+1]=static_cast<unsigned char>(std::clamp(std::lround((basis[axis]+1)*127.5f),0l,255l));
                        record[3]=37;record[7]=219;
                        for (unsigned int k=8;k<16;++k) record[k]=static_cast<unsigned char>((i+k*13)%256);
                        if (packed) Field<unsigned int>(record,16)=PackPosition(vertices[i].position,origin,extent);
                        else Field<Vec>(record,16)=vertices[i].position;
                        for (unsigned int k=0;k<uvBytes;++k) record[positionEnd+k]=static_cast<unsigned char>((i*31+k*7)%256);
                    }
                    const auto before=data;
                    BufferView originalView;
                    if (!ViewBuffer(gpu.data(),originalView)) return false;
                    std::vector<unsigned char> unpacked(vertices.size()*(28+uvBytes));
                    CopyUnpackedStream(originalView,unpacked.data());
                    for (unsigned int i=0;i<vertices.size();++i)
                    {
                        const auto* original=before.data()+i*stride;
                        const auto* converted=unpacked.data()+i*(28+uvBytes);
                        const Vec p=Field<Vec>(const_cast<unsigned char*>(converted),16),q=vertices[i].position;
                        if (std::memcmp(original,converted,16) || std::memcmp(original+positionEnd,converted+28,uvBytes) ||
                            std::fabs(p.x-q.x)>0.3f || std::fabs(p.y-q.y)>0.3f || std::fabs(p.z-q.z)>0.3f) return false;
                    }
                    if (!BakeBuffer(gpu.data(),vertices,base.effectiveDepth,base.settings)) return false;
                    BufferView after;
                    if (!ViewBuffer(gpu.data(),after)) return false;
                    float maximumPush=0;
                    for (unsigned int i=0;i<vertices.size();++i)
                    {
                        auto* record=data.data()+i*stride;
                        auto* original=before.data()+i*stride;
                        if (record[3]!=original[3] || record[7]!=original[7] ||
                            std::memcmp(record+8,original+8,8) ||
                            std::memcmp(record+positionEnd,original+positionEnd,uvBytes)) return false;
                        const float expected=Push(vertices[i].position,vertices[i].torso,base.effectiveDepth,base.settings);
                        maximumPush=(std::max)(maximumPush,expected);
                        const Vec p=BufferPosition(after,i),q=vertices[i].position;
                        const float tolerance=packed?0.45f:0.0001f;
                        if (std::fabs(p.x-q.x)>tolerance || std::fabs(p.z-q.z)>tolerance ||
                            std::fabs(p.y-q.y-expected)>tolerance) return false;
                    }
                    if (maximumPush<0.5f) return false;
                }
        return true;
    }
    bool VerifyWeightPaint(Baseline& base)
    {
        if (!PaintWeights(base)) return false;
        bool left=false,right=false;
        for (std::size_t lod=0;lod<base.rawLods.size();++lod)
        {
            const auto& raw=base.rawLods[lod];
            const auto firstPaint=raw.weighted;
            if (!PaintWeights(base) || raw.weighted!=firstPaint) return false;
            for (std::size_t i=0;i<base.lods[lod].size();++i)
            {
                const auto* before=raw.original.data()+i*raw.stride;
                const auto* after=raw.weighted.data()+i*raw.stride;
                if (std::memcmp(before,after,8) || std::memcmp(before+16,after+16,raw.stride-16)) return false;
                std::array<unsigned int,256> oldWeights{},newWeights{};
                unsigned int total=0;
                for (int j=0;j<4;++j)
                { oldWeights[before[8+j]]+=before[12+j];newWeights[after[8+j]]+=after[12+j];total+=after[12+j]; }
                if (total!=255) return false;
                const auto chest=raw.targets[i*2],neutral=raw.targets[i*2+1];
                if (chest==255 || neutral==255)
                { if (std::memcmp(before+8,after+8,8)) return false;continue; }
                for (unsigned int bone=0;bone<256;++bone)
                    if (bone!=chest && bone!=neutral && oldWeights[bone]!=newWeights[bone]) return false;
                if (oldWeights[chest]+oldWeights[neutral]!=newWeights[chest]+newWeights[neutral]) return false;
                const auto& p=base.lods[lod][i].position;
                if (p.y<=1 && newWeights[chest]) return false;
                if (p.y>1 && newWeights[chest]>10)
                { if (p.x<-2) left=true;if (p.x>2) right=true; }
            }
        }
        return left && right;
    }
}

int main()
{
    if (!VerifyMaterialGate()) return 9;
    if (!VerifyOutfitRetirement()) return 10;
    if (!VerifyArchiveScope()) return 11;
    if (!VerifyDiscardedStream()) return 13;
    if (!LoadCache() || baselines.size()!=25) return 1;
    const Profile shape;
    // Geometric requirements, not a second implementation of the curve.
    if (Push({7.5f,-10,143},1,7,shape)!=0 || Push({7.5f,10,143},0,7,shape)!=0 ||
        Push({60,10,143},1,7,shape)!=0 || Push({7.5f,10,110},1,7,shape)!=0) return 2;
    const float left=Push({-7.5f,10,143},1,7,shape);
    const float right=Push({7.5f,10,143},1,7,shape);
    if (std::fabs(left-right)>0.00001f || right<6.9f ||
        Push({0,10,143},1,7,shape)>=right) return 3;
    unsigned int garments=0,moved=0,total=0;
    for (auto& base:baselines)
    {
        if (base.text.find("LADY_GREY")!=std::string::npos || base.text.find("DRESS_")!=std::string::npos ||
            base.text.find("WHISPER")!=std::string::npos) continue;
        if (!PrepareMorphs(base) || base.measuredStretch>base.settings.maxStretch+0.0001f) return 8;
        if (!VerifyWeightPaint(base)) { std::printf("Weight paint FAILED: %s\n",base.text.c_str());return 14; }
        unsigned int painted=0;for (const auto& raw:base.rawLods) painted+=raw.paintedVertices;
        std::printf("%s weights_changed=%u native_bone_depth=%.2f UV/positions/tangents=unchanged\n",
            base.text.c_str(),painted,profile.boneDepthScale);
        unsigned int changed=0;
        for (const auto& lod:base.lods)
        {
            if (!VerifyBakedStreams(base,lod)) { std::printf("Private stream preservation FAILED: %s\n",base.text.c_str());return 12; }
            for (unsigned int i=0;i<lod.size();++i)
            {
                const auto& source=lod[i];
                const auto delta=Deform(source,i,base.effectiveDepth,base.settings);
                if (delta.index!=i || !std::isfinite(delta.position.y) || delta.position.y<0 ||
                    delta.position.y>shape.depth+0.0001f || delta.position.x!=0 || delta.position.z!=0) return 4;
                if ((source.torso==0 || source.position.y<=1 || std::fabs(source.position.x)>shape.halfWidth+shape.radiusX ||
                    std::fabs(source.position.z-shape.height)>=shape.radiusZ) && delta.position.y!=0) return 5;
                if (std::fabs(delta.position.y)>0.0001f) ++changed;
                ++total;
            }
        }
        if (!changed) { std::printf("No chest deformation for %s\n",base.text.c_str()); return 6; }
        moved+=changed;
        ++garments;
    }
    if (garments!=22) return 7;
    std::printf("OFFLINE PASS: %u baselines/%u eligible; chest weight paint normalized255, unrelated influences preserved, bilateral front masks, back excluded, no accumulation; positions/UV/tangents unchanged. Historical geometry format tests also passed (%u/%u); engine duplication/render/animation NOT tested.\n",
        static_cast<unsigned int>(baselines.size()),garments,moved,total);
    return 0;
}

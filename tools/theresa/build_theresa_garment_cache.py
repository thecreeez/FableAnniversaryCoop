"""Extract read-only garment baselines; never modify a retail package.

The client computes its own per-component morph deltas from this cache.
CPU vertex order is also GPU morph SourceIdx order (validated chunk bases).
"""
import hashlib
import json
import math
import struct
from pathlib import Path
import compare_skeletons as c


def garment_lods(data, item, bone_count):
    result=[]
    lo,hi=item['offset'],item['offset']+item['size']
    # FSkelMeshSection: material/chunk u16, first index/triangles u32, sorting u8.
    for start in range(lo,hi-60):
        n=c.u32(data,start)
        if not 1<=n<=32: continue
        try:
            sections=[struct.unpack_from('<HHIIB',data,start+4+13*i) for i in range(n)]
            if sections[0][2]!=0 or any(s[0]>=32 or s[1]>=32 or s[3]>100000 for s in sections):continue
            if any(b[2]!=a[2]+3*a[3] for a,b in zip(sections,sections[1:])):continue
            p=start+4+13*n
            revision,width,stride,count=struct.unpack_from('<IBII',data,p)
            if (revision,width,stride)!=(1,2,2) or count!=sum(s[3]*3 for s in sections) or not 30<count<200000:continue
            p+=13
            indices=struct.unpack_from('<'+'H'*count,data,p);p+=2*count
            nb=c.u32(data,p);p+=4
            if not 1<=nb<=bone_count:continue
            p+=nb*2
            chunks=c.u32(data,p);p+=4
            if not 1<=chunks<=32 or max(s[1] for s in sections)>=chunks:continue
            vertices=[]
            for _ in range(chunks):
                first,nr=struct.unpack_from('<II',data,p);p+=8
                assert first==len(vertices) and nr<65536
                pending=[]
                for soft,count_vertices in [(False,nr),(True,None)]:
                    if soft:count_vertices=c.u32(data,p);p+=4
                    assert count_vertices<65536
                    for _ in range(count_vertices):
                        pos=struct.unpack_from('<3f',data,p)
                        normal=tuple((v/127.5)-1 for v in data[p+20:p+23])
                        uv=struct.unpack_from('<8f',data,p+24)
                        assert all(math.isfinite(v) and abs(v)<10000 for v in pos)
                        if soft:weights=list(zip(data[p+60:p+64],data[p+64:p+68]));p+=68
                        else:weights=[(data[p+60],255)];p+=61
                        assert sum(w for _,w in weights)==255
                        pending.append(dict(pos=pos,normal=normal,uv=uv,weights=weights))
                palette_count=c.u32(data,p);p+=4
                assert 1<=palette_count<=bone_count
                palette=struct.unpack_from('<'+'H'*palette_count,data,p);p+=2*palette_count
                assert max(palette)<bone_count
                ar,ass,maximum=struct.unpack_from('<III',data,p);p+=12
                assert ar==nr and ar+ass==len(pending) and 1<=maximum<=4
                for v in pending:
                    v['palette']=palette
                    v['weights']=[(palette[b],w) for b,w in v['weights'] if w]
                vertices.extend(pending)
            assert vertices and max(indices)<len(vertices) and p<=hi
            result.append(dict(start=start,end=p,vertices=vertices,indices=indices))
        except (AssertionError,IndexError,struct.error):continue
    assert 1<=len(result)<=8,(item['name'],'unexpected validated LOD count',len(result))
    for a,b in zip(result,result[1:]):assert a['end']<=b['start']
    return result


def verify_gpu_order(data,item,lod):
    """Independent serialized GPU stream confirms native morph SourceIdx."""
    vertices=lod['vertices'];hi=item['offset']+item['size']
    needle=struct.pack('<3f',*vertices[0]['pos'])
    position=data.find(needle,lod['end'],hi)
    matches=[]
    while position>=0:
        # UE3 FPackedNormal x2, bones/weights x4, float3 position, half UV.
        for stride in range(32,65,4): # Multiple UV channels, half/full precision.
            if position+(len(vertices)-1)*stride+12<=hi and all(
                data[position+i*stride:position+i*stride+12]==struct.pack('<3f',*v['pos'])
                for i,v in enumerate(vertices)):
                matches.append((position,stride))
        position=data.find(needle,position+1,hi)
    assert len(matches)==1,(item['name'],'no unique GPU position stream',matches)
    position,stride=matches[0]
    for i,v in enumerate(vertices):
        normal=bytes(round((value+1)*127.5) for value in v['normal'])
        assert data[position+i*stride-12:position+i*stride-9]==normal,(item['name'],'GPU tangent Z order mismatch',i)
    formats=[]
    for kind,width in [('e',2),('f',4)]:
        count=(stride-28)//(width*2)
        if not 1<=count<=4 or 28+count*width*2!=stride:continue
        valid=True
        for i,v in enumerate(vertices):
            actual=struct.unpack_from('<'+kind*count*2,data,position+i*stride+12)
            for a,b in zip(actual,v['uv']):
                if not math.isfinite(a) or not math.isfinite(b) or abs(a-b)>max(0.0007,abs(b)*0.001):
                    valid=False;break
            if not valid:break
        if valid:formats.append(dict(component=kind,channels=count))
    assert formats,(item['name'],'CPU/GPU UV vertex order mismatch')
    return position,stride,formats


def main():
    source=c.ROOT/'test-install/WellingtonGame/CookedPC/Startup.upk'
    before=hashlib.sha256(source.read_bytes()).hexdigest()
    data=c.package_bytes(source); names=c.name_table(data); exports=c.export_table(data,names)
    meshes=[e for e in exports if e['class_name']=='SkeletalMesh' and
        e['name'].startswith('MESH_HERO_') and ('SHIRT' in e['name'] or 'FULL_OUTFIT' in e['name']) and 'FOLDED' not in e['name']]
    output=bytearray(b'THGEOM03'+struct.pack('<I',len(meshes)))
    report=[]
    for item in meshes:
        skeleton=c.skeleton_in_export(data,names,item,minimum_bones=1)
        bones=skeleton['bones']; lods=garment_lods(data,item,len(bones))
        torso={b['index'] for b in bones if b['name'] in ('Bip01_Spine1S0','Bip01_Spine2S0','Bip01_Spine3S0')}
        assert len(torso)==3
        chest=next(b['index'] for b in bones if b['name']=='Bip01_Spine2S0')
        neutral=next(b['index'] for b in bones if b['name']=='Bip01_Spine3S0')
        name=item['name'].encode('ascii')
        output+=struct.pack('<I',len(name))+name+struct.pack('<II',len(bones),len(lods))
        rows=[]
        for lod in lods:
            gpu,stride,uv_formats=verify_gpu_order(data,item,lod)
            output+=struct.pack('<I',len(lod['vertices']))
            covered=0
            for v in lod['vertices']:
                weight=sum(w for b,w in v['weights'] if b in torso)/255
                output+=struct.pack('<7f',*v['pos'],*v['normal'],weight)
                covered+=weight>0
            output+=struct.pack('<I',len(lod['indices']))
            output+=struct.pack('<'+'I'*len(lod['indices']),*lod['indices'])
            assert len(uv_formats)==1,(item['name'],uv_formats)
            fmt=uv_formats[0]
            raw=data[gpu-16:gpu-16+stride*len(lod['vertices'])]
            targets=bytearray()
            for i,v in enumerate(lod['vertices']):
                record=raw[i*stride:(i+1)*stride]
                actual={}
                for local,weight in zip(record[8:12],record[12:16]):
                    if weight:actual[v['palette'][local]]=actual.get(v['palette'][local],0)+weight
                expected={}
                for bone,weight in v['weights']:expected[bone]=expected.get(bone,0)+weight
                assert actual==expected,(item['name'],'CPU/GPU weight mismatch',i,actual,expected)
                targets+=bytes([v['palette'].index(b) if b in v['palette'] else 255 for b in (chest,neutral)])
            output+=struct.pack('<4I',stride,int(fmt['component']=='f'),fmt['channels'],len(raw))+raw+targets
            assert covered>0
            rows.append(dict(vertices=len(lod['vertices']),torso_vertices=covered,gpu_position_offset=gpu,gpu_stride=stride,gpu_uv_formats=uv_formats,weight_palettes_verified=True,paired_targets=sum(targets[i]!=255 and targets[i+1]!=255 for i in range(0,len(targets),2))))
        report.append(dict(name=item['name'],bone_count=len(bones),lods=rows))
        print(item['name'],[(r['vertices'],r['torso_vertices']) for r in rows],flush=True)
    assert len(meshes)>=15
    destination=c.ROOT/'src/bin/Release/theresa-garment-geometry.bin'
    destination.write_bytes(output)
    config=c.ROOT/'src/config/theresa-garment-shape.ini'
    if config.exists() and not destination.with_name(config.name).exists():
        destination.with_name(config.name).write_bytes(config.read_bytes())
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    manifest=dict(source_sha256=before,cache_sha256=hashlib.sha256(output).hexdigest(),meshes=report)
    (c.ROOT/'local/theresa-garment-cache-manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print('Cache',len(output),'bytes;',len(meshes),'garments;',destination)


if __name__=='__main__':main()

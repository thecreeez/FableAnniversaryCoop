"""Offline Fable Sister triangle/CPU vertex inspection. Never writes game data."""
import sys, struct, json, math
from pathlib import Path
from collections import defaultdict
import compare_skeletons as c

def lods(data, item):
    lo, hi=item['offset'],item['offset']+item['size']
    results=[]
    needle=struct.pack('<I',4)
    cursor=lo
    while True:
        start=data.find(needle,cursor,hi)
        if start<0: break
        cursor=start+1
        try:
            sections=[struct.unpack_from('<HHIIB',data,start+4+13*i) for i in range(4)]
            if [s[0] for s in sections]!=list(range(4)): continue
            if [s[1] for s in sections]!=list(range(4)): continue
            if sections[0][2]!=0: continue
            if any(b[2]!=a[2]+3*a[3] for a,b in zip(sections,sections[1:])): continue
            p=start+56
            revision,width,stride,count=struct.unpack_from('<IBII',data,p)
            if revision!=1 or width!=2 or stride!=2: continue
            if count!=sum(s[3]*3 for s in sections) or not 100<count<100000: continue
            index_offset=p+13
            indices=struct.unpack_from('<'+'H'*count,data,index_offset)
            p=index_offset+2*count
            bones=c.u32(data,p); p+=4+bones*2
            if not 1<=bones<=80 or c.u32(data,p)!=4: continue
            p+=4
            vertices=[]; chunks=[]
            for ci in range(4):
                first,nr=struct.unpack_from('<II',data,p);p+=8
                assert first==len(vertices) and nr<20000
                rigid=[];soft=[]
                for i in range(nr):
                    pos=struct.unpack_from('<3f',data,p)
                    uv=struct.unpack_from('<2f',data,p+24)
                    bi=data[p+60]
                    assert all(math.isfinite(v) and abs(v)<10000 for v in pos+uv)
                    rigid.append(dict(pos=pos,uv=uv,weights=[(bi,255)]));p+=61
                ns=c.u32(data,p);p+=4
                assert ns<20000
                for i in range(ns):
                    pos=struct.unpack_from('<3f',data,p)
                    uv=struct.unpack_from('<2f',data,p+24)
                    bis=data[p+60:p+64]; ws=data[p+64:p+68]
                    assert all(math.isfinite(v) and abs(v)<10000 for v in pos+uv)
                    assert sum(ws)==255
                    soft.append(dict(pos=pos,uv=uv,weights=list(zip(bis,ws))));p+=68
                nb=c.u32(data,p);p+=4
                assert 1<=nb<=80
                palette=struct.unpack_from('<'+'H'*nb,data,p);p+=nb*2
                ar,ass,maximum=struct.unpack_from('<III',data,p);p+=12
                assert ar==nr and ass==ns and 1<=maximum<=4
                for v in rigid+soft:
                    v['weights']=[(palette[b],w) for b,w in v['weights'] if w]
                vertices+=rigid+soft
                chunks.append(dict(first=first,count=nr+ns,palette=palette))
            assert max(indices)<len(vertices)
            results.append(dict(section_offset=start,index_offset=index_offset,indices=indices,
                sections=sections,vertices=vertices,cpu_end=p,chunks=chunks))
        except (AssertionError,struct.error,IndexError):
            continue
    assert len(results)==2,[(hex(x['section_offset']),len(x['vertices'])) for x in results]
    return results

def components(lod,material):
    s=lod['sections'][material]; inds=lod['indices'][s[2]:s[2]+s[3]*3]
    parent={}
    def root(x):
        parent.setdefault(x,x)
        while parent[x]!=x:
            parent[x]=parent[parent[x]];x=parent[x]
        return x
    for a,b,cc in zip(inds[::3],inds[1::3],inds[2::3]):
        for x in [b,cc]:parent[root(x)]=root(a)
    groups=defaultdict(list)
    for v in set(inds):groups[root(v)].append(v)
    return sorted(groups.values(),key=len,reverse=True)

if __name__=='__main__':
    data=c.package_bytes(c.ROOT/'test-install/WellingtonGame/CookedPC/OrganizedForCooking/WeSISTER_01_SF.upk')
    names=c.name_table(data); exports=c.export_table(data,names)
    item=exports[69]; parsed=lods(data,item)
    for li,lod in enumerate(parsed):
        print('LOD',li,'vertices',len(lod['vertices']),'tris',len(lod['indices'])//3)
        for material in [0,1]:
            print(' material',material)
            for group in components(lod,material)[:16]:
                points=[lod['vertices'][v]['pos'] for v in group]
                bounds=[tuple(round(f(x[k] for x in points),2) for k in range(3)) for f in [min,max]]
                print('  component',len(group),bounds)
    out=c.ROOT/'local/sister-geometry.json'
    out.write_text(json.dumps(parsed),encoding='utf-8')
    print('Wrote',out)

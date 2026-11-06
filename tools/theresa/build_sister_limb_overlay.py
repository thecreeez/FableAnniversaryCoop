"""Generate an additional local limb-only package; retain all original assets.

No retail data is embedded in this script. Reads only the test-install package;
output is a separate generated package and a reproducible audit manifest.
"""
import struct, json, hashlib, zlib
from pathlib import Path
import compare_skeletons as c
from inspect_sister_geometry import lods, components

def preserved(lod):
    keep=set()
    for material in [0,1]:
        selected=set()
        for group in components(lod,material):
            points=[lod['vertices'][v]['pos'] for v in group]
            low=[min(v[k] for v in points) for k in range(3)]
            high=[max(v[k] for v in points) for k in range(3)]
            hand=material==0 and min(abs(low[0]),abs(high[0]))>41 and low[2]>80 and high[2]<116
            lower=material==1 and (low[0]>0.2 or high[0]<-0.2) and high[2]<46 and max(abs(low[0]),abs(high[0]))<24
            if hand or lower:selected.update(group)
        sec=lod['sections'][material]
        for i in range(sec[2],sec[2]+sec[3]*3,3):
            triangle=lod['indices'][i:i+3]
            if all(v in selected for v in triangle): keep.add(i//3)
    return keep

def literal_lzo(data):
    assert len(data)>=19
    n=len(data)-18
    return b'\0'+b'\0'*((n-1)//255)+bytes([(n-1)%255+1])+data+b'\x11\0\0'

def repack(original,plain):
    count=c.u32(original,0x71)
    chunks=[struct.unpack_from('<4I',original,0x75+16*i) for i in range(count)]
    out=bytearray(plain[:chunks[0][0]])
    for i,(start,size,old_offset,_) in enumerate(chunks):
        block_size=c.u32(original,old_offset+4)
        blocks=[bytes(plain[p:min(p+block_size,start+size)]) for p in range(start,start+size,block_size)]
        compressed=[literal_lzo(b) for b in blocks]
        assert all(c.lzo_block(a,len(b))==b for a,b in zip(compressed,blocks))
        payload=struct.pack('<4I',0x9E2A83C1,block_size,sum(map(len,compressed)),size)
        payload+=b''.join(struct.pack('<II',len(a),len(b)) for a,b in zip(compressed,blocks))
        payload+=b''.join(compressed)
        struct.pack_into('<4I',out,0x75+16*i,start,size,len(out),len(payload))
        out+=payload
    return out

def preview(lod,keep,path):
    w,h=800,720; pixels=bytearray([24,27,33])*(w*h)
    def line(a,b,color):
        ax,ay=int(400+a[0]*3.4),int(680-a[2]*3.4)
        bx,by=int(400+b[0]*3.4),int(680-b[2]*3.4)
        steps=max(abs(ax-bx),abs(ay-by),1)
        for j in range(steps+1):
            x,y=round(ax+(bx-ax)*j/steps),round(ay+(by-ay)*j/steps)
            if 0<=x<w and 0<=y<h:pixels[(y*w+x)*3:(y*w+x)*3+3]=bytes(color)
    for draw_kept in [False,True]:
        for i in range(0,len(lod['indices']),3):
            if (i//3 in keep)!=draw_kept:continue
            tri=[lod['vertices'][v]['pos'] for v in lod['indices'][i:i+3]]
            for a,b in zip(tri,tri[1:]+tri[:1]):line(a,b,(80,220,150) if draw_kept else (65,70,80))
    def chunk(kind,payload):return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload))
    raw=b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

if __name__=='__main__':
    folder=c.ROOT/'test-install/WellingtonGame/CookedPC/OrganizedForCooking'
    source=folder/'WeSISTER_01_SF.upk'; original=source.read_bytes()
    plain=c.package_bytes(source); names=c.name_table(plain); exports=c.export_table(plain,names)
    report=dict(source_sha256=hashlib.sha256(original).hexdigest(),exports=[])
    for item in exports:
        if item['class_name']!='SkeletalMesh':continue
        rows=[]
        for li,lod in enumerate(lods(plain,item)):
            keep=preserved(lod)
            assert 100<len(keep)<len(lod['indices'])//3//2
            for tri in range(len(lod['indices'])//3):
                if tri in keep:continue
                a=lod['indices'][tri*3]
                struct.pack_into('<3H',plain,lod['index_offset']+tri*6,a,a,a)
            rows.append(dict(lod=li,triangles=len(lod['indices'])//3,kept=len(keep),removed=len(lod['indices'])//3-len(keep)))
            if item['index']==70:preview(lod,keep,c.ROOT/f'local/theresa-limb-overlay-lod{li}.png')
        report['exports'].append(dict(index=item['index'],lods=rows))
    # Rename only the serialized namespace name with an equal-length name.
    # Index tables, export sizes, bone/vertex data and bulk offsets stay fixed.
    p=c.u32(plain,29)
    renamed=0
    for name in names:
        size=struct.unpack_from('<i',plain,p)[0];p+=4
        length=abs(size)*(2 if size<0 else 1)
        if name.lower()=='wesister_01':
            value='wesister_c1'.encode('utf-16-le' if size<0 else 'ascii')+(b'\0\0' if size<0 else b'\0')
            assert len(value)==length
            plain[p:p+length]=value;renamed+=1
        p+=length+8
    assert renamed==1
    output=folder/'WeSISTER_C1_SF.upk'
    packed=repack(original,plain);output.write_bytes(packed)
    decoded=c.package_bytes(output)
    assert decoded[c.u32(original,0x75):]==plain[c.u32(original,0x75):]
    assert source.read_bytes()==original
    report.update(output=str(output),output_sha256=hashlib.sha256(packed).hexdigest(),bytes=len(packed))
    (c.ROOT/'local/theresa-limb-overlay-manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))

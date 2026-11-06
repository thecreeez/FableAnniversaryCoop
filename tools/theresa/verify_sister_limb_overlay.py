"""Verify generated package integrity and retained limb geometry, read-only."""
import struct
import compare_skeletons as c
from inspect_sister_geometry import lods

folder=c.ROOT/'test-install/WellingtonGame/CookedPC/OrganizedForCooking'
original=c.package_bytes(folder/'WeSISTER_01_SF.upk')
overlay=c.package_bytes(folder/'WeSISTER_C1_SF.upk')
old_names=c.name_table(original);new_names=c.name_table(overlay)
assert len(old_names)==len(new_names)
assert [(a,b) for a,b in zip(old_names,new_names) if a!=b]==[('wesister_01','wesister_c1')]
old_exports=c.export_table(original,old_names);new_exports=c.export_table(overlay,new_names)
for old,new in zip(old_exports,new_exports):
    expected=dict(old)
    if old['name']=='wesister_01':expected['name']='wesister_c1'
    assert expected==new
    if old['class_name']!='SkeletalMesh':continue
    assert c.skeleton_in_export(original,old_names,old)==c.skeleton_in_export(overlay,new_names,new)
    for li,(a,b) in enumerate(zip(lods(original,old),lods(overlay,new))):
        assert a['sections']==b['sections'] and a['vertices']==b['vertices'] and a['chunks']==b['chunks']
        coverage=set(); kept=0
        for ti in range(0,len(a['indices']),3):
            before=a['indices'][ti:ti+3];after=b['indices'][ti:ti+3]
            if len(set(after))==1:continue
            assert after==before
            kept+=1
            material=next(s[0] for s in a['sections'] if s[2]<=ti<s[2]+s[3]*3)
            assert material in [0,1], 'overlay must not duplicate face/hair'
            center=[sum(a['vertices'][v]['pos'][k] for v in after)/3 for k in range(3)]
            assert center[2]<116, 'torso/shoulder cloth survived'
            if material==0:
                assert abs(center[0])>41 and center[2]>80
                coverage.add('right-hand' if center[0]>0 else 'left-hand')
            else:
                assert center[2]<46 and abs(center[0])<24
                coverage.add('right-lower-leg' if center[0]>0 else 'left-lower-leg')
        assert len(coverage)==4 and kept>100
        print('PASS export',old['index'],'LOD',li,'retained triangles',kept,'coverage',sorted(coverage))
print('PASS unchanged skeleton, CPU vertices/weights/UVs, sections, exports; no torso/face/hair triangles; both hands and lower legs in both LODs')

"""Inspect retained UI ScriptText locally; never edit/copy retail text into git."""
from pathlib import Path
import hashlib,json
import compare_skeletons as c

root=Path(__file__).resolve().parents[3]
package=root/'test-install/WellingtonGame/CookedPC/WellingtonGame.u'
data=c.package_bytes(package)
names=c.name_table(data)
exports=c.export_table(data,names)
folder=root/'local/ui-script-audit'
folder.mkdir(exist_ok=True)
sources={}
for item in exports:
    if item['class_name']!='TextBuffer' or item['outer']<=0:continue
    owner=exports[item['outer']-1]['name']
    if owner not in ['RTTGroup','RTTGFxRenderTargetActor','UI_PageCloth']:continue
    raw=data[item['offset']:item['offset']+item['size']]
    size=c.u32(raw,20)
    assert size>0 and 24+size==len(raw),(owner,size)
    text=raw[24:].rstrip(b'\0').decode('utf-8').replace('\r\n','\n')
    (folder/(owner+'.uc')).write_text(text,encoding='utf-8',newline='\n')
    sources[owner]=text
assert 'mRTTGroup.ApplyCameraHeroView' in sources['UI_PageCloth']
hero_view=sources['RTTGroup'].split('function ApplyCameraHeroView(',1)[1].split('function ApplyCameraNothingView(',1)[0]
assert 'SetupRTTActorMesh' not in hero_view and 'SetCameraOffsetView' in hero_view
report={
    'package_sha256':hashlib.sha256(package.read_bytes()).hexdigest(),
    'script_exports':sorted(sources),
    'hero_camera_uses_rotating_item_actor':False,
    'native_clothing_clone':{'build_rva':'019FE990','tick_rva':'019F67C8',
        'close_rva':'019FC45B','release_rva':'019F4DC9','clone_field':'1B8','graphics_begin':'2CC','graphics_end':'2D0'},
    'ui_lighting':{'submit_rva':'01C49C90','handler_rva':'01C47990','channels':'02000001'},
    'engine_calls':False,
}
(folder/'inventory-capture-audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS: clothing page uses RTT hero camera; retained script exports audited locally; native clone lifecycle documented. No game launch.')

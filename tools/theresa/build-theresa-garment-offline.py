from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parents[3]
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');version='10.0.26100.0'
env=os.environ.copy()
env['INCLUDE']=';'.join(str(p) for p in [vc/'include',sdk/'Include'/version/'ucrt',sdk/'Include'/version/'shared',sdk/'Include'/version/'um'])
env['LIB']=';'.join(str(p) for p in [vc/'lib/x86',sdk/'Lib'/version/'ucrt/x86',sdk/'Lib'/version/'um/x86'])
env['PATH']=str(vc/'bin/Hostx64/x86')+';'+env.get('PATH','')
env['TMP']=env['TEMP']=str(root/'local/build-temp')
exe=root/'src/bin/Release/TheresaGarmentShape.Offline.exe'
cmd=[str(vc/'bin/Hostx64/x86/cl.exe'),'/nologo','/std:c++17','/EHsc','/MD','/O2','/W4','/WX','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
    '/I'+str(root/'src/Client'),'/Fo'+str(root/'local/build-temp')+'\\','/Fe'+str(exe),str(root/'src/Tests/TheresaGarmentShape.Offline.cpp'),str(root/'src/Client/Core/Hooking/CodePatch.cpp')]
cmd+=['/link','user32.lib']
subprocess.run(cmd,cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True)

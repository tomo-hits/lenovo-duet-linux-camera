#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# Modified on 2026-10-05: S1 package pins; see docs/S1_VALIDATION.json.
# SPDX-License-Identifier: MIT
"""Package only fresh build stages. Signing key stays at the explicit local path."""
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)

p=argparse.ArgumentParser()
p.add_argument('--kernel',type=Path,required=True);p.add_argument('--userspace',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--sign-key',type=Path,required=True)
p.add_argument('--public-key',type=Path,required=True);a=p.parse_args()
kit=Path(__file__).resolve().parent.parent; out=a.output.absolute();require(not out.exists() and not out.is_symlink(), 'Output must be a new directory');out.mkdir()
require(a.sign_key.is_file() and a.public_key.is_file(), "Invalid or incompatible input")
keys=out/'keys';keys.mkdir();shutil.copy2(a.public_key,keys/a.public_key.name)
repo=out/'repo/aarch64';repo.mkdir(parents=True);stages=out/'stages';stages.mkdir()
roots={n:stages/n for n in ['libcamera','libcamera-ipa','libcamera-tools','libcamera-dev','pipewire-libs','gst-plugin-pipewire','duet-camera-modules','duet-camera-config','duet-camera']}
for d in roots.values():d.mkdir()
def copy(src,dest):
 if any(part.startswith("._") or part in (".DS_Store","__pycache__") for part in src.parts):return
 dest.parent.mkdir(parents=True,exist_ok=True)
 if src.is_symlink():dest.symlink_to(os.readlink(src))
 else:shutil.copy2(src,dest)
def write(name,path,data,mode=0o644):
 dest=roots[name]/path;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(data);dest.chmod(mode)
for src in sorted((a.userspace/'stage').rglob('*')):
 if not(src.is_file() or src.is_symlink()):continue
 rel=src.relative_to(a.userspace/'stage');r=str(rel)
 # These modules belong to the distribution's pipewire-pulse package. Keep
 # its matching 1.6.8 build installed rather than claiming its SONAMEs/files.
 if r in ('usr/lib/pipewire-0.3/libpipewire-module-filter-chain.so',
          'usr/lib/pipewire-0.3/libpipewire-module-protocol-pulse.so',
          'usr/lib/pipewire-0.3/libpipewire-module-pulse-tunnel.so'):continue
 if r.startswith(('usr/lib/libcamera/ipa/','usr/share/libcamera/ipa/')):name='libcamera-ipa'
 elif r.startswith('usr/include/libcamera/') or r.startswith('usr/lib/pkgconfig/libcamera') or r in ('usr/lib/libcamera.so','usr/lib/libcamera-base.so'):name='libcamera-dev'
 elif r.startswith(('usr/lib/libcamera.so.','usr/lib/libcamera-base.so.')):name='libcamera'
 elif r=='usr/bin/cam':name='libcamera-tools'
 elif r.startswith('usr/lib/gstreamer-1.0/'):name='gst-plugin-pipewire'
 elif r=='usr/share/pipewire/client.conf' or r.startswith('usr/share/pipewire/client.conf.avail/') or r.startswith(('usr/lib/libpipewire-0.3.so','usr/lib/pipewire-0.3/','usr/lib/spa-0.2/')) and '/jack/' not in r and '/v4l2/' not in r:name='pipewire-libs'
 else:continue
 copy(src,roots[name]/rel)
for directory,obj in [('mt8183-p1-public','mt8183_p1'),('mt8183-seninf-dual-highres','mtk_seninf'),('ov02a10-standard-fps-range','ov02a10'),('ov8856-standard-balanced-fps','ov8856'),('dw9768-upstream','dw9768')]:
 copy(a.kernel/'modules'/directory/(obj+'.ko'),roots['duet-camera-modules']/('lib/modules/6.18.28-mt81/extra/duet-camera/'+obj+'.ko'))
for name in ['duet-camera-check.py','duet-camera-activate.py','fdt.py','legacy_bindings.py','camera-nodes.json','integrate-camera-dtb.py','kernel-abi.json']:
 copy(kit/'packaging/postmarketos'/name,roots['duet-camera-modules']/('usr/lib/duet-camera/'+name if name!='kernel-abi.json' else 'usr/share/duet-camera/'+name))
for command in ['check','activate']:
 write('duet-camera-modules','usr/bin/duet-camera-'+command,'#!/bin/sh\nexec python3 /usr/lib/duet-camera/duet-camera-'+command+'.py "$@"\n',0o755)
write('duet-camera-config','etc/modprobe.d/duet-camera.conf','options mtk_seninf probe_only=0\n')
write('duet-camera-config','usr/lib/udev/rules.d/75-duet-camera.rules','SUBSYSTEM=="video4linux", ATTR{name}=="MT8183 P1 RAW", GROUP="video", MODE="0660", TAG+="uaccess"\nSUBSYSTEM=="misc", KERNEL=="udmabuf", GROUP="video", MODE="0660", TAG+="uaccess"\n')
profile='''wireplumber.profiles = { main = { monitor.libcamera = required } }
monitor.libcamera.rules = [
 { matches = [ { node.name = "libcamera_input._base_soc_i2c_11008000_camera-sensor_3d" } { node.name = "libcamera_input._base_soc_i2c_11009000_camera-sensor_10" } ]
   actions = { update-props = { video.framerate = "RATE/1" video.size = "1536x864" node.pause-on-idle = true } } }
]
'''
# Include actual notices, not only standard license templates.
def notice(root,folders):
 chunks=[]
 for folder in folders:
  for path in sorted((root/folder).rglob('*')):
   if path.suffix not in ('.c','.cpp','.h','.hpp') or not path.is_file():continue
   lines=path.read_text(errors='replace').splitlines()[:100]
   owners=[line.strip(' /*\t') for line in lines if 'copyright' in line.lower()]
   if owners:chunks.append(str(path.relative_to(root))+'\n'+'\n'.join(owners))
 return '\n\n'.join(chunks)+'\n'
libnotice=notice(a.userspace/'libcamera',['src','include'])
pwnotice=notice(a.userspace/'pipewire',['src','spa'])
module_dirs=['mt8183-p1-public','mt8183-seninf-dual-highres','ov02a10-standard-fps-range','ov8856-standard-balanced-fps','dw9768-upstream']
knnotice=notice(kit/'modules',module_dirs)
for name in ['libcamera','libcamera-ipa','libcamera-tools','libcamera-dev']:
 write(name,'usr/share/licenses/'+name+'/NOTICE.txt',libnotice)
 copy(kit/'LIBCAMERA-COPYING.rst',roots[name]/('usr/share/licenses/'+name+'/COPYING.rst'))
for name in ['pipewire-libs','gst-plugin-pipewire']:
 write(name,'usr/share/licenses/'+name+'/NOTICE.txt',pwnotice)
 copy(kit/'PIPEWIRE-COPYING',roots[name]/('usr/share/licenses/'+name+'/COPYING'))
write('duet-camera-modules','usr/share/licenses/duet-camera-modules/NOTICE.txt',knnotice)
for directory in module_dirs:
 for license in (kit/'modules'/directory/'LICENSES').glob('*'):
  copy(license,roots['duet-camera-modules']/('usr/share/licenses/duet-camera-modules/'+directory+'/'+license.name))
manifest={};payloads={}
def build(name,version,license,extra=()):
 root=roots[name];needs=set();provides=set()
 for src in root.rglob('*'):
  if src.is_symlink() or not src.is_file():continue
  if src.read_bytes()[:4]!=b'\x7fELF':continue
  data=subprocess.run(['readelf','-d',str(src)],check=True,capture_output=True,text=True).stdout
  needs.update(re.findall(r'\(NEEDED\).*?\[(.*?)\]',data));provides.update(re.findall(r'\(SONAME\).*?\[(.*?)\]',data))
 for lic in (kit/'LICENSES').glob('*'):copy(lic,root/'usr/share/licenses'/name/lic.name)
 require((root/'usr/share/licenses'/name/'PROJECT-MIT.txt').is_file(), 'Missing project MIT notice')
 dest=repo/(name+'-'+version+'.apk')
 info=dict(name=name,version=version,arch='aarch64',license=license,description='Experimental Duet SKU176 camera '+name,depends=' '.join([*extra,*sorted('so:'+s for s in needs-provides)]),provides=' '.join(sorted('so:'+s for s in provides)))
 args=['apk','mkpkg','--files',str(root),'--output',str(dest),'--sign-key',str(a.sign_key)]
 for k,v in info.items():args+=['--info',k+':'+v]
 subprocess.run(args,check=True)
 subprocess.run(['apk','verify','--keys-dir',str(keys),str(dest)],check=True)
 manifest[dest.name]=dict(**info,sha256=hashlib.sha256(dest.read_bytes()).hexdigest())
 payloads[dest.name]={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file() and not p.is_symlink()}
for name,license,extra in [('libcamera','LGPL-2.1-or-later',['libcamera-ipa=0.7.2-r103']),('libcamera-ipa','LGPL-2.1-or-later AND BSD-2-Clause AND CC0-1.0',[]),('libcamera-tools','GPL-2.0-or-later',['libcamera=0.7.2-r103']),('libcamera-dev','LGPL-2.1-or-later',['libcamera=0.7.2-r103'])]:build(name,'0.7.2-r103',license,extra)
build('pipewire-libs','1.6.8-r104','MIT')
build('gst-plugin-pipewire','1.6.8-r104','MIT',['pipewire-session-manager','gst-plugins-base'])
build('duet-camera-modules','0.2.8-r0','GPL-2.0-only AND MIT',['python3','kmod','zstd','cmd:udevadm'])
for rev,fps in [(0,15),(1,30)]:
 write('duet-camera-config','usr/share/wireplumber/wireplumber.conf.d/60-duet-camera.conf',profile.replace('RATE',str(fps)))
 build('duet-camera-config','0.1.3-r'+str(rev),'MIT',['wireplumber=0.5.15-r0'])
 write('duet-camera','usr/share/doc/duet-camera/README','Experimental fresh build. See release documentation for matched kernel/FW and hardware validation. No kernel Image, firmware or OS image included.\n')
 build('duet-camera','0.2.13-r'+str(rev),'MIT',['duet-camera-modules=0.2.8-r0','duet-camera-config=0.1.3-r'+str(rev),'libcamera=0.7.2-r103','libcamera-ipa=0.7.2-r103','libcamera-tools=0.7.2-r103','pipewire-libs=1.6.8-r104','gst-plugin-pipewire=1.6.8-r104'])
subprocess.run(['apk','mkndx','--sign-key',str(a.sign_key),'--output',str(repo/'packages.adb'),*[str(p) for p in sorted(repo.glob('*.apk'))]],check=True)
(out/'payload-hashes.json').write_text(json.dumps(payloads,indent=2)+'\n')
(out/'packages.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'packages':len(manifest),'private_key_exported':False,'images_exported':False}))

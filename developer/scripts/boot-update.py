#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare or restore the recorded SKU176 internal boot configuration.

Keeps the existing kernel, initramfs, command line and developer trust chain.
Never changes firmware, GPT, hardware boot regions, or reboots automatically.
"""
import argparse, datetime, hashlib, importlib.util, json, os, pathlib, stat, subprocess, sys, tarfile, types
sys.dont_write_bytecode=True
HERE=pathlib.Path(__file__).resolve().parent
KIT=HERE.parent
WORK=pathlib.Path('/var/lib/duet-camera-boot-preparation')
BOOT_FILES=('/boot/vmlinuz.kpart','/boot/mt8183-kukui-krane-sku176.dtb','/boot/dtbs/mediatek/mt8183-kukui-krane-sku176.dtb')

def load(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),HERE/(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

tx=load('boot-transaction');prep=load('prepare-boot-candidate');update=load('apply-update')
require=tx.require

def checker(packages):
    with update.checker_zstd(packages) as env:
        result=subprocess.run([sys.executable,str(KIT/'packaging/postmarketos/duet-camera-check.py'),'--abi',str(KIT/'packaging/postmarketos/kernel-abi.json')],capture_output=True,text=True,env=env,check=False)
    data=json.loads(result.stdout)
    require(data['errors']==['camera device tree not active'],
            'Boot preparation requires an unmodified supported stock camera DT and matched kernel/firmware: '+str(data['errors']))

def current_plan():
    require(os.geteuid()==0,'Run with sudo')
    abi=json.loads((KIT/'packaging/postmarketos/kernel-abi.json').read_text())
    require(os.uname().release==abi['kernel'] and tx.sha(tx.read_file('/sys/kernel/notes'))==abi['kernel_notes_sha256'],'Kernel ABI differs')
    m=tx.mounts();root=m['/'];boot=m['/boot']
    require(root['source'].startswith('/dev/mmcblk') and root['source'].endswith('p3'),'Internal root must be eMMC partition 3')
    disk=pathlib.Path(root['source']).name[:-2]
    require(boot['source']=='/dev/'+disk+'p2' and root['uuid'] and boot['uuid'] and root['uuid']!=boot['uuid'],'Unsupported root/boot layout')
    p={'schema':1,'scope':'SKU176 matched kernel; boot DT and two isolation files only',
       'expected_boot_id_before':pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
       'kernel_release':abi['kernel'],'kernel_notes_sha256':abi['kernel_notes_sha256'],
       'live_fdt_before_sha256':tx.sha(tx.read_file('/sys/firmware/fdt')),
       'disk':disk,'disk_cid':(pathlib.Path('/sys/class/block')/disk/'device/cid').read_text().strip(),
       'disk_bytes':int((pathlib.Path('/sys/class/block')/disk/'size').read_text())*512,
       'raw_device':'/dev/'+disk+'p1','raw_start_sectors':8192,'raw_size_bytes':33554432,
       'original_boot_mount_options':boot['options'],'power_failure_atomic':False}
    for label,row in [('root',root),('boot',boot)]:
        for field in ('device','uuid','fstype'):p[label+'_'+field]=row['source' if field=='device' else field]
    count,digest=tx.package_digest();p.update(before_package_count=count,before_package_map_sha256=digest,before_world_sha256=tx.sha(tx.read_file('/etc/apk/world')))
    tx.identity(p,original_boot=True)
    return p

def locked_work():
    import fcntl
    if not WORK.exists():WORK.mkdir(mode=0o700)
    tx.parents_safe(WORK/'lock')
    st=WORK.lstat();require(stat.S_ISDIR(st.st_mode) and st.st_uid==0 and stat.S_IMODE(st.st_mode)==0o700,'Unsafe boot preparation directory')
    fd=os.open(WORK/'lock',os.O_CREAT|os.O_WRONLY|os.O_NOFOLLOW,0o600)
    st=os.fstat(fd);require(stat.S_ISREG(st.st_mode) and st.st_uid==0 and stat.S_IMODE(st.st_mode)==0o600,'Unsafe preparation lock')
    fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);return fd

def latest_work():
    value=json.loads(tx.read_file(WORK/'latest.json',True))
    directory=WORK/value['directory']
    require(directory.parent==WORK and directory.name==pathlib.Path(value['directory']).name,'Invalid boot backup pointer')
    tx.parents_safe(directory/'plan.json')
    st=directory.lstat();require(stat.S_ISDIR(st.st_mode) and st.st_uid==0 and stat.S_IMODE(st.st_mode)==0o700,'Unsafe backup directory')
    require(tx.sha(tx.read_file(directory/'plan.json',True))==value['plan_sha256'],'Recorded boot plan differs')
    return directory,value

def mounted(p,action):
    was_ro='ro' in tx.mounts()['/boot']['options'].split(',')
    if was_ro:tx.run(['mount','-o','remount,rw','/boot'])
    succeeded=False
    try:
        action();succeeded=True
    finally:
        # Install protects /boot; restore reinstates its original RO/RW setting.
        ro=was_ro if not succeeded else (action.__name__!='restore_action' or 'ro' in p['original_boot_mount_options'].split(','))
        tx.run(['mount','-o','remount,'+('ro' if ro else 'rw'),'/boot'])

def protect_boot_mount(text):
    lines=text.splitlines(keepends=True);matches=0
    for i,line in enumerate(lines):
        words=line.split()
        if not words or words[0].startswith('#') or len(words)<2 or words[1]!='/boot':continue
        require(len(words)==6,'Unsupported /boot fstab entry')
        options=[o for o in words[3].split(',') if o not in ('rw','ro')]
        words[3]=','.join(['ro',*options]);lines[i]='\t'.join(words)+'\n';matches+=1
    require(matches==1,'Expected exactly one /boot fstab entry')
    return ''.join(lines)

def prepare(packages):
    if (WORK/'latest.json').exists():
        old, pointer=latest_work();old_plan=tx.load_plan(old/'plan.json')
        if tx.load_state(pathlib.Path(pointer['state']))['status']=='PREPARED':
            tx.cancel_prepared(types.SimpleNamespace(state=pointer['state'],execute=True),old_plan)
        require(tx.load_state(pathlib.Path(pointer['state']))['status'] in ('RESTORED','APPLY_ABORTED_RESTORED','CANCELLED_PREPARED'),'A boot transaction is still active')
    p=current_plan();checker(packages);update.selected_apks(packages)
    name=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    directory=WORK/name;directory.mkdir(mode=0o700)
    raw=tx.raw_bytes(p);tx.create_bytes(directory/'original-p1.bin',raw)
    # Only these fixed files enter the file-only candidate preparer.
    with tarfile.open(directory/'boot-inputs.tar','w') as archive:
        for path in (*BOOT_FILES,'/boot/vmlinuz','/boot/initramfs'):
            tx.parents_safe(path);tx.read_file(path);archive.add(path,arcname=path.lstrip('/'),recursive=False)
    (directory/'boot-inputs.tar').chmod(0o600)
    args=types.SimpleNamespace(boot_tar=directory/'boot-inputs.tar',raw_partition=directory/'original-p1.bin',source_kit=KIT,output=directory/'candidate')
    prep.prepare(args);inputs=directory/'candidate';record=json.loads((inputs/'preparation.json').read_text())
    # Check the existing native signature before using its unchanged keyblock.
    require('Body verification succeeded.' in tx.run(['vbutil_kernel','--verify','/boot/vmlinuz.kpart','--verbose']),'Original signature invalid')
    command=record['native_sign_command'];subprocess.run(command,cwd=inputs,check=True)
    args.candidate=inputs/'candidate.kpart';args.validation=inputs/'validation.json';prep.validate(args)
    candidate=tx.read_file(args.candidate);dtb=tx.read_file(inputs/'camera-sku176.dtb')
    p.update(raw_before_sha256=tx.sha(raw),raw_after_sha256=tx.sha(candidate+raw[len(candidate):]),candidate_kpart_sha256=tx.sha(candidate),candidate_kpart_bytes=len(candidate),candidate_dtb_sha256=tx.sha(dtb),boot_files={})
    for path in BOOT_FILES:
        meta,_=tx.snapshot(path);require(meta['kind']=='file','Boot assets must be regular files')
        source='candidate.kpart' if path.endswith('vmlinuz.kpart') else 'camera-sku176.dtb'
        p['boot_files'][path]={'before_'+k:meta[k] for k in ('sha256','bytes','mode','uid','gid')}
        p['boot_files'][path].update(candidate_file=source,candidate_sha256=tx.sha(candidate if source=='candidate.kpart' else dtb))
    init_spec=importlib.util.spec_from_file_location('scp_init',KIT/'packaging/postmarketos/external-scp-init.py');init=importlib.util.module_from_spec(init_spec);init_spec.loader.exec_module(init)
    p['preboot_configs']={'/etc/fstab':protect_boot_mount(tx.read_file('/etc/fstab').decode()),
                         '/etc/modprobe.d/duet-camera-scp-isolation.conf':init.INTERNAL_CONFIG,
                         '/etc/modprobe.d/duet-camera-first-install.conf':'blacklist ov02a10\nblacklist ov8856\nblacklist dw9768\nblacklist mtk_seninf\nblacklist mt8183_p1\n'}
    fstab_meta,_=tx.snapshot('/etc/fstab')
    require(fstab_meta['kind']=='file','fstab must be a regular file')
    p['preboot_config_metadata']={'/etc/fstab':{k:fstab_meta[k] for k in ('mode','uid','gid')}}
    tx.create_bytes(directory/'plan.json',(json.dumps(p,indent=2)+'\n').encode());p=tx.load_plan(directory/'plan.json')
    state=tx.STATE_PARENT/name;a=types.SimpleNamespace(inputs=str(inputs),state=str(state),execute=True)
    tx.prepare(a,p)
    pointer={'directory':name,'plan_sha256':tx.PLAN_SHA256,'state':str(state)}
    tx.atomic_bytes(WORK/'latest.json',(json.dumps(pointer,indent=2)+'\n').encode())
    def apply_action():tx.apply(a,p)
    mounted(p,apply_action)
    print('Boot settings saved and applied. Restart normally, log in, then run sudo sh install.sh again. Keep '+str(WORK)+' and '+str(tx.STATE_PARENT)+'.')

def restore():
    directory,value=latest_work();p=tx.load_plan(directory/'plan.json');a=types.SimpleNamespace(state=value['state'],execute=True)
    state=tx.load_state(pathlib.Path(a.state))
    if state['status'] in ('RESTORED','APPLY_ABORTED_RESTORED','CANCELLED_PREPARED'):return
    if state['status']=='PREPARED':
        tx.cancel_prepared(a,p);return
    def restore_action():tx.restore(a,p)
    mounted(p,restore_action)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=('check','prepare','restore'));parser.add_argument('--packages',type=pathlib.Path,default=KIT.parent/'packages');a=parser.parse_args()
    require(os.geteuid()==0,'Run with sudo')
    if a.action=='check':
        current_plan();checker(a.packages.resolve());update.selected_apks(a.packages.resolve());print('Boot preparation check PASS; no writes.');return
    fd=locked_work()
    try:
        with tx.transaction_lock():
            prepare(a.packages.resolve()) if a.action=='prepare' else restore()
    finally:os.close(fd)
if __name__=='__main__':main()

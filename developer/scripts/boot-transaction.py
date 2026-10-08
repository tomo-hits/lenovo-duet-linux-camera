#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recorded-device boot transaction. Never reboots/remounts or changes GPT/FW.

check is read-only. prepare only creates verified backups in a new root-only
state directory. apply/restore perform writes and require explicit --execute. A single kernel slot is NOT power-failure atomic.
"""
import argparse, base64, contextlib, datetime, fcntl, hashlib, json, os, pathlib, re, stat, subprocess, sys, tempfile
PLAN_SHA256 = None
STATE_PARENT = pathlib.Path('/var/lib/duet-camera-boot-transaction')
LOCK = pathlib.Path('/run/lock/duet-camera-boot-transaction.lock')
CHUNK = 1024*1024

def require(ok, why):
    if not ok: raise RuntimeError(why)

def sha(b): return hashlib.sha256(b).hexdigest()
def now(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def run(args): return subprocess.run(args,check=True,capture_output=True,text=True).stdout

def read_file(path, private=False):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    try:
        s=os.fstat(fd);require(stat.S_ISREG(s.st_mode),'Expected regular file: '+str(path))
        if private:require(s.st_uid==0 and stat.S_IMODE(s.st_mode)&0o077==0,'State file permissions are not root-only')
        with os.fdopen(fd,'rb',closefd=False) as f:return f.read()
    finally:os.close(fd)

def sync_dir(path):
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:os.fsync(fd)
    finally:os.close(fd)

def parents_safe(path):
    p=pathlib.Path(path).absolute().parent
    while p!=p.parent:
        s=p.lstat();require(stat.S_ISDIR(s.st_mode) and not stat.S_ISLNK(s.st_mode),'Destination ancestor is not a real directory')
        require(s.st_uid==0 and s.st_mode&0o022==0,'Destination ancestor is writable by non-root')
        p=p.parent

def atomic_bytes(path,data,mode=0o600,uid=0,gid=0):
    path=pathlib.Path(path);parents_safe(path)
    fd,temp=tempfile.mkstemp(prefix='.duet-transaction-',dir=path.parent)
    try:
        os.fchmod(fd,mode);os.fchown(fd,uid,gid)
        with os.fdopen(fd,'wb',closefd=False) as f:f.write(data);f.flush();os.fsync(fd)
        os.close(fd);fd=-1;os.replace(temp,path);sync_dir(path.parent)
    finally:
        if fd>=0:os.close(fd)
        if os.path.lexists(temp):os.unlink(temp)

def create_bytes(path,data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        with os.fdopen(fd,'wb',closefd=False) as f:f.write(data);f.flush();os.fsync(fd)
    finally:os.close(fd)

def load_plan(path):
    global PLAN_SHA256
    parents_safe(path)
    b=read_file(path,True);PLAN_SHA256=sha(b)
    p=json.loads(b)
    require(p['schema']==1 and p['kernel_release']=='6.18.28-mt81','Unsupported boot plan')
    require(re.fullmatch(r'mmcblk[0-9]+',p['disk']),'Invalid eMMC disk')
    require([p[n] for n in ('raw_device','boot_device','root_device')]==
            ['/dev/'+p['disk']+'p'+str(i) for i in (1,2,3)],'Unsupported partition layout')
    require(p['raw_start_sectors']==8192 and p['raw_size_bytes']==33554432,'Unsupported kernel slot geometry')
    require(set(p['boot_files'])=={'/boot/vmlinuz.kpart','/boot/mt8183-kukui-krane-sku176.dtb','/boot/dtbs/mediatek/mt8183-kukui-krane-sku176.dtb'},'Unexpected boot targets')
    require(set(p['preboot_configs'])=={'/etc/modprobe.d/duet-camera-first-install.conf','/etc/modprobe.d/duet-camera-scp-isolation.conf','/etc/fstab'},'Unexpected config targets')
    for name,info in p['boot_files'].items():
        require(info['candidate_file']==('candidate.kpart' if name.endswith('vmlinuz.kpart') else 'camera-sku176.dtb'),'Unexpected candidate file')
    return p

def package_digest():
    packages={}
    for block in pathlib.Path('/lib/apk/db/installed').read_text().split('\n\n'):
        f=dict(line.split(':',1) for line in block.splitlines() if line.startswith(('P:','V:')))
        if 'P' in f:packages[f['P']]=f['V']
    return len(packages),sha(''.join(k+'='+v+'\n' for k,v in sorted(packages.items())).encode())

def package_guard(p):
    count,digest=package_digest()
    require((count,digest)==(p['before_package_count'],p['before_package_map_sha256']),
            'Original package versions must be present before this boot transaction')
    require(sha(read_file('/etc/apk/world'))==p['before_world_sha256'],'Original apk world differs')

def mounts():
    result={}
    for mount in ('/','/boot'):
        j=json.loads(run(['findmnt','--json','-o','SOURCE,FSTYPE,OPTIONS,UUID,TARGET','--mountpoint',mount]))
        rows=j.get('filesystems',[]);require(len(rows)==1,'Expected one direct mount')
        row=rows[0];require(row['target']==mount,'Wrong mountpoint');result[mount]=row
    return result

def identity(p, original_boot=False, writable_boot=False):
    require(os.geteuid()==0,'Run from an authenticated root terminal')
    require(os.uname().release==p['kernel_release'],'Unexpected running kernel')
    require(sha(read_file('/sys/kernel/notes'))==p['kernel_notes_sha256'],'Running kernel notes differ')
    current=pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    require(re.fullmatch(r'[0-9a-f-]{36}',current),'Invalid boot id')
    if original_boot:
        require(current==p['expected_boot_id_before'],'Original boot id changed')
        require(sha(read_file('/sys/firmware/fdt'))==p['live_fdt_before_sha256'],'Original live DT changed')
    m=mounts()
    for label,mount in [('root','/'),('boot','/boot')]:
        item=m[mount];require(item['source']==p[label+'_device'] and item['uuid']==p[label+'_uuid'] and item['fstype']==p[label+'_fstype'],'Root/boot device or filesystem identity differs')
    require('rw' in m['/']['options'].split(','),'Root is not writable')
    if writable_boot:require('rw' in m['/boot']['options'].split(','),'/boot must be made writable by the operator for this transaction')
    disk=pathlib.Path('/sys/class/block')/p['disk']
    require((disk/'device/type').read_text().strip()=='MMC','Target is not eMMC')
    require((disk/'device/cid').read_text().strip()==p['disk_cid'],'eMMC CID differs')
    require(int((disk/'size').read_text())*512==p['disk_bytes'],'eMMC capacity differs')
    require((disk/'ro').read_text().strip()=='0','eMMC is unexpectedly read-only')
    for suffix in ('boot0','boot1'):
        require((pathlib.Path('/sys/class/block')/(p['disk']+suffix)/'ro').read_text().strip()=='1','Hardware boot-region RO protection differs')
    disk_real=disk.resolve(strict=True)
    for name in (p['root_device'],p['boot_device'],p['raw_device']):
        device=pathlib.Path(name);s=device.lstat()
        require(stat.S_ISBLK(s.st_mode) and not stat.S_ISLNK(s.st_mode),'Expected direct block device')
        node=pathlib.Path('/sys/class/block')/device.name
        require(node.resolve(strict=True).parent==disk_real,'Partition is not on selected eMMC')
        major,minor=map(int,(node/'dev').read_text().split(':'))
        require(s.st_rdev==os.makedev(major,minor),'Block node major/minor differs')
    raw=pathlib.Path('/sys/class/block')/pathlib.Path(p['raw_device']).name
    require(int((raw/'partition').read_text())==1 and int((raw/'start').read_text())==p['raw_start_sectors'] and int((raw/'size').read_text())*512==p['raw_size_bytes'],'Kernel partition geometry differs')
    return {'boot_id':current,'mounts':m}

def open_raw(p, writable=False):
    flags=os.O_RDWR if writable else os.O_RDONLY
    fd=os.open(p['raw_device'],flags|os.O_NOFOLLOW)
    try:
        s=os.fstat(fd);node=pathlib.Path('/sys/class/block')/pathlib.Path(p['raw_device']).name
        major,minor=map(int,(node/'dev').read_text().split(':'))
        require(stat.S_ISBLK(s.st_mode) and s.st_rdev==os.makedev(major,minor),'Opened raw target differs')
        return fd
    except Exception:os.close(fd);raise

def read_raw_fd(fd,size):
    chunks=[]
    for offset in range(0,size,CHUNK):
        block=os.pread(fd,min(CHUNK,size-offset),offset)
        require(len(block)==min(CHUNK,size-offset),'Short raw partition read');chunks.append(block)
    return b''.join(chunks)

def raw_bytes(p):
    fd=open_raw(p)
    try:return read_raw_fd(fd,p['raw_size_bytes'])
    finally:os.close(fd)

def write_raw(p,data,before_sha,after_sha,on_start=None):
    require(0<len(data)<=p['raw_size_bytes'],'Raw write length exceeds reviewed partition')
    fd=open_raw(p,True)
    try:
        current=read_raw_fd(fd,p['raw_size_bytes'])
        if before_sha is not None:require(sha(current)==before_sha,'Raw partition changed before write')
        if on_start:on_start()
        for offset in range(0,len(data),CHUNK):
            block=data[offset:offset+CHUNK];done=0
            while done<len(block):
                n=os.pwrite(fd,block[done:],offset+done);require(n>0,'Short raw write');done+=n
        os.fsync(fd)
        require(sha(read_raw_fd(fd,p['raw_size_bytes']))==after_sha,'Full raw partition readback differs')
    finally:os.close(fd)

def snapshot(path):
    path=pathlib.Path(path);parents_safe(path)
    try:s=path.lstat()
    except FileNotFoundError:return {'kind':'absent'},None
    meta={'mode':stat.S_IMODE(s.st_mode),'uid':s.st_uid,'gid':s.st_gid}
    if stat.S_ISLNK(s.st_mode):return dict(meta,kind='link',target=os.readlink(path)),None
    require(stat.S_ISREG(s.st_mode),'Unexpected snapshot path type')
    b=read_file(path);return dict(meta,kind='file',bytes=len(b),sha256=sha(b)),b

def same_snapshot(path,expected):
    actual,_=snapshot(path);return actual=={k:v for k,v in expected.items() if k!='backup_file'}

def restore_snapshot(path,meta,state):
    path=pathlib.Path(path);parents_safe(path)
    if meta['kind']=='absent':
        if os.path.lexists(path):
            require(not path.is_dir() or path.is_symlink(),'Will not remove a directory');path.unlink();sync_dir(path.parent)
    elif meta['kind']=='file':
        data=read_file(state/meta['backup_file'],True);require(sha(data)==meta['sha256'],'Saved file hash differs')
        atomic_bytes(path,data,meta['mode'],meta['uid'],meta['gid'])
    elif meta['kind']=='link':
        temp=path.parent/('.duet-link-'+os.urandom(12).hex())
        try:os.symlink(meta['target'],temp);os.lchown(temp,meta['uid'],meta['gid']);os.replace(temp,path);sync_dir(path.parent)
        finally:
            if os.path.lexists(temp):temp.unlink()
    else:raise RuntimeError('Unexpected saved object type')
    require(same_snapshot(path,meta),'Restored file metadata/content differs')

def expected_after(p,path):
    if path in p['boot_files']:
        s=p['boot_files'][path];return {'kind':'file','mode':s['before_mode'],'uid':s['before_uid'],'gid':s['before_gid'],'sha256':s['candidate_sha256']}
    metadata=p.get('preboot_config_metadata',{}).get(path,{'mode':0o644,'uid':0,'gid':0})
    return dict(metadata,kind='file',sha256=sha(p['preboot_configs'][path].encode()))

def matches_after(p,path):
    actual,_=snapshot(path);actual.pop('bytes',None);return actual==expected_after(p,path)

def boot_files_before(p):
    for path,s in p['boot_files'].items():
        meta,_=snapshot(path)
        require(meta=={'kind':'file','mode':s['before_mode'],'uid':s['before_uid'],'gid':s['before_gid'],'bytes':s['before_bytes'],'sha256':s['before_sha256']},'Original boot file changed: '+path)

def verify_candidate(p,inputs,private=False):
    candidate=read_file(inputs/'candidate.kpart',private);dtb=read_file(inputs/'camera-sku176.dtb',private)
    require(len(candidate)==p['candidate_kpart_bytes'] and sha(candidate)==p['candidate_kpart_sha256'],'Candidate kernel digest differs')
    require(sha(dtb)==p['candidate_dtb_sha256'],'Candidate DT digest differs')
    result=run(['vbutil_kernel','--verify',str(inputs/'candidate.kpart'),'--verbose'])
    require('Body verification succeeded.' in result,'Candidate kernel signature verification missing')
    require(sha(read_file(inputs/'candidate.kpart',private))==p['candidate_kpart_sha256'],'Candidate changed during signature verification')
    return candidate,dtb

def state_dir(value,create=False):
    path=pathlib.Path(value).absolute()
    require(path.parent==STATE_PARENT and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,100}',path.name),'State must be one new directory below fixed state parent')
    if create:
        if not STATE_PARENT.exists():STATE_PARENT.mkdir(mode=0o700)
        parents_safe(path);s=STATE_PARENT.lstat();require(stat.S_IMODE(s.st_mode)==0o700,'State parent must be mode0700')
        path.mkdir(mode=0o700)
    s=path.lstat();require(stat.S_ISDIR(s.st_mode) and not path.is_symlink() and s.st_uid==0 and stat.S_IMODE(s.st_mode)==0o700,'Unsafe state directory')
    return path

def save_state(path,s):
    s['updated_utc']=now();atomic_bytes(path/'state.json',(json.dumps(s,indent=2)+'\n').encode())

def load_state(path):
    s=json.loads(read_file(path/'state.json',True));require(s['plan_sha256']==PLAN_SHA256,'State belongs to another reviewed plan');return s

def precheck(p,inputs):
    evidence=identity(p,original_boot=True);package_guard(p);boot_files_before(p)
    original=raw_bytes(p);require(sha(original)==p['raw_before_sha256'],'Original raw partition changed')
    candidate,dtb=verify_candidate(p,inputs)
    require(sha(candidate+original[len(candidate):])==p['raw_after_sha256'],'Candidate raw partition digest differs')
    return evidence,original,candidate,dtb

def check(a,p):
    evidence,*_=precheck(p,pathlib.Path(a.inputs))
    for path in p['preboot_configs']:snapshot(path)
    print(json.dumps({'status':'CHECK_PASS','boot_id':evidence['boot_id'],'boot_writable':'rw' in evidence['mounts']['/boot']['options'].split(','),'writes':False,'power_failure_atomic':False}))

def prepare(a,p):
    evidence,original,candidate,dtb=precheck(p,pathlib.Path(a.inputs));path=state_dir(a.state,create=True)
    s={'schema':1,'plan_sha256':PLAN_SHA256,'created_utc':now(),'status':'PREPARING','before_identity':evidence,
       'baseline_boot_mount_options':p['original_boot_mount_options'],'snapshots':{},'journal':[]}
    create_bytes(path/'original-p1.bin',original);create_bytes(path/'candidate.kpart',candidate);create_bytes(path/'camera-sku176.dtb',dtb)
    for i,name in enumerate([*p['boot_files'],*p['preboot_configs']]):
        meta,data=snapshot(name)
        if data is not None:meta['backup_file']=f'file-{i}.bin';create_bytes(path/meta['backup_file'],data)
        s['snapshots'][name]=meta
    require(sha(read_file(path/'original-p1.bin',True))==p['raw_before_sha256'],'Saved raw backup mismatch')
    verify_candidate(p,path,private=True);s['status']='PREPARED';save_state(path,s);sync_dir(path);os.sync()
    print(json.dumps({'status':'PREPARED','state':str(path),'backup_verified':True,'boot_or_device_written':False}))

def inspect_saved(p,path,s):
    raw=read_file(path/'original-p1.bin',True);require(len(raw)==p['raw_size_bytes'] and sha(raw)==p['raw_before_sha256'],'Saved raw partition differs')
    require(set(s['snapshots'])==set(p['boot_files'])|set(p['preboot_configs']),'Snapshot set differs')
    for name,meta in s['snapshots'].items():
        if name in p['boot_files']:
            expected=p['boot_files'][name]
            actual={k:v for k,v in meta.items() if k!='backup_file'}
            require(actual=={'kind':'file','mode':expected['before_mode'],'uid':expected['before_uid'],
                    'gid':expected['before_gid'],'bytes':expected['before_bytes'],
                    'sha256':expected['before_sha256']},'Saved boot metadata differs from pinned original')
        if meta['kind']=='file':
            require(re.fullmatch(r'file-[0-5]\.bin',meta.get('backup_file','')),'Invalid saved file path')
            b=read_file(path/meta['backup_file'],True);require(len(b)==meta['bytes'] and sha(b)==meta['sha256'],'Saved file digest differs')
    return raw

def cancel_prepared(a,p):
    """Retire a verified, untouched backup without writing boot targets."""
    require(a.execute,'--execute is required after operator review')
    path=state_dir(a.state);s=load_state(path)
    require(s['status']=='PREPARED' and s['journal']==[],
            'Only an untouched PREPARED transaction can be cancelled')
    identity(p);package_guard(p);inspect_saved(p,path,s)
    require(sha(raw_bytes(p))==p['raw_before_sha256'],'Original partition changed; refuse cancellation')
    for name,meta in s['snapshots'].items():
        require(same_snapshot(name,meta),'Original snapshot changed; refuse cancellation: '+name)
    s['status']='CANCELLED_PREPARED'
    s['journal'].append({'event':'prepared_cancelled_unchanged','time':now()})
    save_state(path,s)
    print(json.dumps({'status':s['status'],'state':str(path),'boot_or_device_written':False,'backup_retained':True}))

def apply(a,p):
    require(a.execute,'--execute is required after operator review')
    path=state_dir(a.state);s=load_state(path);require(s['status']=='PREPARED','State is not PREPARED')
    identity(p,original_boot=True,writable_boot=True);package_guard(p);boot_files_before(p)
    original=inspect_saved(p,path,s);candidate,dtb=verify_candidate(p,path,private=True)
    require(sha(raw_bytes(p))==p['raw_before_sha256'],'Original partition changed')
    for name,meta in s['snapshots'].items():require(same_snapshot(name,meta),'Snapshot target changed before apply')
    s['status']='APPLYING';s['journal'].append({'event':'apply_started','time':now()});save_state(path,s)
    raw_owned=False
    try:
        for name,text in p['preboot_configs'].items():
            meta=expected_after(p,name);atomic_bytes(name,text.encode(),meta['mode'],meta['uid'],meta['gid'])
        for name,info in p['boot_files'].items():atomic_bytes(name,candidate if info['candidate_file']=='candidate.kpart' else dtb,info['before_mode'],info['before_uid'],info['before_gid'])
        for name in s['snapshots']:require(matches_after(p,name),'Staged file readback differs')
        identity(p,original_boot=True,writable_boot=True)
        def begin():
            nonlocal raw_owned
            s['journal'].append({'event':'raw_write_started','time':now()});save_state(path,s);raw_owned=True
        write_raw(p,candidate,p['raw_before_sha256'],p['raw_after_sha256'],begin)
        os.sync();s['status']='APPLIED';s['journal'].append({'event':'apply_readback_pass','time':now()});save_state(path,s);os.sync()
    except Exception as error:
        s['apply_error']=str(error);s['status']='FAILED_REQUIRES_RECOVERY'
        try:
            identity(p,original_boot=True,writable_boot=True)
            for name,meta in s['snapshots'].items():require(same_snapshot(name,meta) or matches_after(p,name),'Unexpected external change prevents owned rollback')
            if raw_owned:write_raw(p,original,None,p['raw_before_sha256'])
            else:require(sha(raw_bytes(p))==p['raw_before_sha256'],'Unowned raw change prevents rollback')
            for name,meta in s['snapshots'].items():restore_snapshot(name,meta,path)
            os.sync();s['status']='APPLY_ABORTED_RESTORED';s['journal'].append({'event':'owned_apply_rollback_pass','time':now()})
        except Exception as rollback_error:s['rollback_error']=str(rollback_error)
        save_state(path,s);os.sync();raise RuntimeError('Apply failed; state='+s['status']) from error
    print(json.dumps({'status':'APPLIED','state':str(path),'raw_full_readback':True,'boot_files_and_configs_readback':True,'rebooted':False}))

def restore(a,p):
    require(a.execute,'--execute is required after operator review')
    path=state_dir(a.state);s=load_state(path);require(s['status']=='APPLIED','State is not APPLIED; incomplete states require reviewed recovery')
    identity(p,writable_boot=True);package_guard(p);original=inspect_saved(p,path,s);verify_candidate(p,path,private=True)
    require(sha(raw_bytes(p))==p['raw_after_sha256'],'Current kernel partition differs from exact candidate')
    for name in s['snapshots']:require(matches_after(p,name),'Current staged file differs; refuse overwrite')
    s['status']='RESTORING';s['journal'].append({'event':'restore_started','time':now()});save_state(path,s)
    try:
        for name,meta in s['snapshots'].items():restore_snapshot(name,meta,path)
        identity(p,writable_boot=True);write_raw(p,original,p['raw_after_sha256'],p['raw_before_sha256'])
        for name,meta in s['snapshots'].items():require(same_snapshot(name,meta),'Final restored file differs')
        os.sync();s['status']='RESTORED';s['journal'].append({'event':'restore_readback_pass','time':now()});save_state(path,s);os.sync()
    except Exception as error:
        s['status']='FAILED_REQUIRES_RECOVERY';s['restore_error']=str(error);save_state(path,s);os.sync();raise
    print(json.dumps({'status':'RESTORED','state':str(path),'raw_full_readback':True,'files_metadata_and_absence_readback':True,'rebooted':False}))

@contextlib.contextmanager
def transaction_lock():
    # Share admission with the user wrapper: a cancellation must never race
    # a low-level apply that is about to leave PREPARED.
    LOCK.parent.mkdir(exist_ok=True)
    fd=os.open(LOCK,os.O_WRONLY|os.O_CREAT|os.O_NOFOLLOW,0o600)
    try:
        st=os.fstat(fd);require(stat.S_ISREG(st.st_mode) and st.st_uid==0 and stat.S_IMODE(st.st_mode)==0o600,'Unsafe transaction lock')
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        yield
    finally:os.close(fd)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=['check','prepare','apply','restore']);parser.add_argument('--plan',required=True);parser.add_argument('--state');parser.add_argument('--inputs');parser.add_argument('--execute',action='store_true');a=parser.parse_args()
    require(os.geteuid()==0,'Run from an authenticated root terminal');p=load_plan(a.plan)
    require(a.inputs is not None or a.action in ('apply','restore'),'--inputs required')
    require(a.state is not None or a.action=='check','--state required')
    if a.action=='check':check(a,p);return
    with transaction_lock():{'prepare':prepare,'apply':apply,'restore':restore}[a.action](a,p)
if __name__=='__main__':main()

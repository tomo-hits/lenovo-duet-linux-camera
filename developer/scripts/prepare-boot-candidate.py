#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local file-only preparation and validation for one existing SKU176 boot blob.

No device writes, mounts, signing-key reads, SSH, or firmware operations.
Signing is delegated to the existing native vbutil_kernel using developer keys.
"""
import argparse, gzip, hashlib, importlib, json, lzma, os, pathlib, stat, struct, sys, tarfile, zlib
sys.dont_write_bytecode = True
TARGET = '/images/fdt-27'
BOOT_DTB = 'boot/dtbs/mediatek/mt8183-kukui-krane-sku176.dtb'

def require(ok, msg):
    if not ok: raise ValueError(msg)

def digest(data): return hashlib.sha256(data).hexdigest()
def align4(n): return (n+3)&~3
def align4096(n): return (n+4095)&~4095

def read_regular(path):
    path = pathlib.Path(path)
    require(stat.S_ISREG(path.lstat().st_mode) and not path.is_symlink(), 'Expected regular non-symlink input')
    return path.read_bytes()

def write_new(path, data):
    with pathlib.Path(path).open('xb') as f: f.write(data)
    pathlib.Path(path).chmod(0o600)

def modules(source_kit):
    here = pathlib.Path(source_kit).resolve() / 'packaging/postmarketos'
    sys.path.insert(0, str(here))
    dt = importlib.import_module('fdt')
    integrate = importlib.import_module('integrate-camera-dtb')
    spec = json.loads(read_regular(here/'camera-nodes.json'))
    return dt, integrate, spec

def layout(blob):
    require(blob[:8] == b'CHROMEOS' and len(blob)>=65536, 'Expected ChromeOS kernel blob')
    keysize = struct.unpack_from('<I', blob, 16)[0]
    require(112<=keysize<65536, 'Unexpected keyblock size')
    size = struct.unpack_from('<I', blob, keysize)[0]
    major, minor = struct.unpack_from('<II', blob, keysize+32)
    require((major,minor)==(2,2), 'Only recorded vboot preamble 2.2 supported')
    start = keysize+size
    version = struct.unpack_from('<I',blob,keysize+40)[0]
    load, boot_address = struct.unpack_from('<QQ',blob,keysize+48)
    boot_size = struct.unpack_from('<I',blob,keysize+64)[0]
    body_size = struct.unpack_from('<I',blob,keysize+88)[0]
    vh_address = struct.unpack_from('<Q',blob,keysize+96)[0]
    vh_size, reserved, flags = struct.unpack_from('<III',blob,keysize+104)
    require(start==65536 and boot_size==4096 and vh_address==vh_size==reserved==0, 'Unexpected original ARM boot layout')
    require(len(blob)==start+body_size, 'Kernel blob length differs from signed length')
    boot_offset=boot_address-load
    config_offset=boot_offset-8192
    require(0<config_offset and boot_offset+boot_size==body_size, 'Invalid boot/config layout')
    fit_header=struct.unpack_from('>10I',blob,start)
    fit_size=fit_header[1]
    require(fit_header[0]==0xd00dfeed and fit_size<=config_offset, 'FIT exceeds reserved kernel region')
    body=blob[start:]
    require(not any(body[fit_size:config_offset]), 'Unexpected nonzero FIT alignment padding')
    config=body[config_offset:config_offset+4096]
    require(b'\0' in config and not any(config[config.index(b'\0'):]), 'Unsupported nonzero commandline padding')
    params=body[config_offset+4096:boot_offset]
    require(params==bytes(4096), 'Nonzero ARM params require separate analysis')
    return {'keyblock_size':keysize,'preamble_size':size,'header_version':[major,minor],
      'version':version,'load':load,'bootloader_address':boot_address,'bootloader_size':boot_size,
      'body_size':body_size,'body_start':start,'config_offset':config_offset,'flags':flags,
      'fit_size':fit_size,'fit':body[:fit_size],'keyblock':blob[:keysize],
      'commandline':config,'params':params,'bootloader':body[boot_offset:boot_offset+boot_size]}

def patch_fit(raw, replacements):
    h=list(struct.unpack_from('>10I',raw)); _,total,st,strings,reserve,version,compat,cpu,slen,tlen=h
    require(st+tlen==strings and strings+slen==total==len(raw), 'Unsupported FIT block layout')
    stack=[]; edits={}; p=st; end=st+tlen
    while p<end:
        start=p; token=struct.unpack_from('>I',raw,p)[0];p+=4
        if token==1:
            stop=raw.index(b'\0',p,end); name=raw[p:stop].decode('ascii');p=align4(stop+1)
            stack.append('/' if not stack else stack[-1].rstrip('/')+'/'+name)
        elif token==2: stack.pop()
        elif token==3:
            size,nameoff=struct.unpack_from('>II',raw,p);p+=8
            stop=raw.index(b'\0',strings+nameoff,strings+slen);name=raw[strings+nameoff:stop].decode('ascii')
            value_start=p;p=align4(p+size); key=(stack[-1],name)
            if key in replacements:
                v=replacements[key];edits[start]=(p,struct.pack('>III',3,len(v),nameoff)+v+bytes(-len(v)%4))
        elif token==4: pass
        elif token==9: break
        else: raise ValueError('Unexpected FDT token')
    require(len(edits)==len(replacements), 'FIT property replacement coverage mismatch')
    chunks=[];cursor=st
    for start,(stop,new) in sorted(edits.items()): chunks.extend([raw[cursor:start],new]);cursor=stop
    chunks.append(raw[cursor:end]);new_struct=b''.join(chunks)
    h[1]=st+len(new_struct)+slen;h[3]=st+len(new_struct);h[9]=len(new_struct)
    result=struct.pack('>10I',*h)+raw[40:st]+new_struct+raw[strings:strings+slen]
    return result

def check_crcs(fdt):
    for path,props in fdt.nodes.items():
        if path.startswith('/images/') and 'data' in props:
            hp=fdt.nodes.get(path+'/hash')
            require(hp is not None and hp.get('algo')==b'crc32\0', 'Expected crc32 FIT image hash')
            require(hp.get('value')==struct.pack('>I',zlib.crc32(props['data'])&0xffffffff), 'Invalid FIT image CRC')

def semantic_check(old_fit,new_fit,dt,expected_dtb):
    old,new=dt.Fdt(old_fit),dt.Fdt(new_fit)
    require(set(old.nodes)==set(new.nodes),'FIT node set changed')
    differences=[]
    for p in old.nodes:
        require(set(old.nodes[p])==set(new.nodes[p]), 'FIT attribute set changed')
        for k,v in old.nodes[p].items():
            if new.nodes[p][k]!=v:differences.append([p,k])
    require(differences==[[TARGET,'data'],[TARGET+'/hash','value']], 'FIT changed outside target DT and CRC')
    require(new.nodes[TARGET]['data']==expected_dtb,'Candidate DT differs from validated integration')
    require(old.reservation_bytes==new.reservation_bytes and old.boot_cpu==new.boot_cpu,'FIT reservations/CPU changed')
    check_crcs(old);check_crcs(new)
    return {'changed_fit_properties':differences,'all_other_fit_node_attributes_byte_identical':True,
      'kernel_compressed_sha256':digest(new.nodes['/images/kernel-1']['data']),
      'initramfs_sha256':digest(new.nodes['/images/ramdisk-1']['data']),
      'other_dtbs_unchanged':26}

def baseline(tar_path, partition_path, source_kit):
    dt,integrate,spec=modules(source_kit)
    raw_tar=read_regular(tar_path)
    import io
    with tarfile.open(fileobj=io.BytesIO(raw_tar)) as t:
        original=t.extractfile('boot/vmlinuz.kpart').read()
        stock=t.extractfile(BOOT_DTB).read()
        require(t.extractfile('boot/mt8183-kukui-krane-sku176.dtb').read()==stock,'Static DT duplicates differ')
        kernel=t.extractfile('boot/vmlinuz').read();initramfs=t.extractfile('boot/initramfs').read()
    partition=read_regular(partition_path)
    require(len(partition)==33554432 and partition[:len(original)]==original,'Raw kernel partition/file prefix mismatch')
    old=layout(original);fit=dt.Fdt(old['fit']);check_crcs(fit)
    require(fit.nodes[TARGET]['description']==b'mt8183-kukui-krane-sku176\0' and fit.nodes[TARGET]['data']==stock,'Unexpected DT target')
    require(fit.nodes['/configurations/conf-27']['fdt']==b'fdt-27\0','Unexpected target configuration')
    image=gzip.decompress(kernel)
    require(lzma.decompress(fit.nodes['/images/kernel-1']['data'])==image,'FIT/package kernel mismatch')
    require(fit.nodes['/images/ramdisk-1']['data']==initramfs,'FIT/package initramfs mismatch')
    abi=json.loads(read_regular(pathlib.Path(source_kit)/'packaging/postmarketos/kernel-abi.json'))
    require(digest(image)==abi['image_sha256'],'Kernel ABI image hash mismatch')
    camera,audit=integrate.integrate(stock,spec)
    original_dt,new_dt=dt.Fdt(stock),dt.Fdt(camera)
    require(len(new_dt.nodes)-len(original_dt.nodes)==26,'Incorrect added camera node count')
    for p,props in original_dt.nodes.items():
        for k,v in props.items():require(new_dt.nodes[p][k]==v,'Non-camera DT attribute changed')
    replacements={(TARGET,'data'):camera,(TARGET+'/hash','value'):struct.pack('>I',zlib.crc32(camera)&0xffffffff)}
    new_fit=patch_fit(old['fit'],replacements)
    comparison=semantic_check(old['fit'],new_fit,dt,camera)
    report={'source_boot_tar_sha256':digest(raw_tar),'raw_partition_sha256':digest(partition),
      'original_kpart_sha256':digest(original),'original_kpart_bytes':len(original),
      'raw_partition_bytes':len(partition),'raw_partition_prefix_matches_boot_file':True,
      'source_dtb_sha256':digest(stock),'integrated_dtb_sha256':digest(camera),'dt_audit':audit,
      'fit_before_bytes':len(old['fit']),'fit_after_bytes':len(new_fit),
      'fit_before_sha256':digest(old['fit']),'fit_after_sha256':digest(new_fit),
      'original_fit_padding_bytes':old['config_offset']-len(old['fit']),
      'expected_body_growth_bytes':align4096(len(new_fit))-old['config_offset'],
      'kernel_uncompressed_sha256':digest(image),'comparison':comparison,
      'body_params_zero_and_preserved':True,'commandline_block_sha256':digest(old['commandline']),
      'bootloader_sha256':digest(old['bootloader']),'keyblock_sha256':digest(old['keyblock']),
      'original_vboot_fields':{k:old[k] for k in ['keyblock_size','preamble_size','header_version','version','load','bootloader_address','bootloader_size','body_size','body_start','config_offset','flags']},
      'boot_or_device_written':False,'firmware_modified':False}
    return dt,old,new_fit,camera,report

def prepare(a):
    out=pathlib.Path(a.output)
    require(not out.exists() and not out.is_symlink(),'Output must be new')
    dt,old,new_fit,camera,report=baseline(a.boot_tar,a.raw_partition,a.source_kit)
    out.mkdir(parents=True,mode=0o700)
    for name,data in {'candidate.fit':new_fit,'camera-sku176.dtb':camera,'original.keyblock':old['keyblock'],
      'original.cmdline':old['commandline'].split(b'\0',1)[0],
      'original.bootloader':old['bootloader']}.items():write_new(out/name,data)
    report['native_sign_command']=['vbutil_kernel','--pack','candidate.kpart','--keyblock','original.keyblock',
      '--signprivate','/usr/share/vboot/devkeys/kernel_data_key.vbprivk','--version',str(old['version']),
      '--vmlinuz','candidate.fit','--bootloader','original.bootloader','--config','original.cmdline',
      '--arch','arm','--kloadaddr',str(old['load']),'--pad',str(old['body_start']),'--flags',str(old['flags'])]
    write_new(out/'preparation.json',(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({'prepared':str(out),'camera_dtb_sha256':report['integrated_dtb_sha256'],
      'expected_body_growth_bytes':report['expected_body_growth_bytes'],'devices_written':False}))

def validate(a):
    dt,old,new_fit,camera,report=baseline(a.boot_tar,a.raw_partition,a.source_kit)
    candidate=read_regular(a.candidate);new=layout(candidate)
    require(new['fit']==new_fit,'Signed FIT bytes differ from exact prepared candidate')
    semantic_check(old['fit'],new['fit'],dt,camera)
    for k in ['keyblock_size','preamble_size','header_version','version','load','bootloader_size','body_start','flags','keyblock','commandline','params','bootloader']:
        require(new[k]==old[k], 'Vboot metadata or unchanged boot content differs: '+k)
    growth=report['expected_body_growth_bytes']
    require(new['config_offset']==old['config_offset']+growth,'Unexpected config offset change')
    require(new['body_size']==old['body_size']+growth,'Unexpected signed body size change')
    require(new['bootloader_address']==old['bootloader_address']+growth,'Unexpected bootloader relocation')
    require(len(candidate)==len(read_regular(a.candidate))<=report['raw_partition_bytes'],'Candidate exceeds kernel partition')
    report.update({'signed_candidate_sha256':digest(candidate),'signed_candidate_bytes':len(candidate),
      'all_content_and_vboot_fields_validated':True,'signature_validation':'REQUIRES_NATIVE_VBUTIL_KERNEL_VERIFY',
      'vboot_bootloader_address_increase':growth})
    write_new(a.validation,(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps({'validation':str(a.validation),'sha256':digest(candidate),'bytes':len(candidate),
      'content_vboot_metadata':'PASS','signature':'REQUIRES_NATIVE_VBUTIL_KERNEL_VERIFY'}))

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='mode',required=True)
    for mode in ['prepare','validate']:
        s=sub.add_parser(mode);s.add_argument('--boot-tar',required=True);s.add_argument('--raw-partition',required=True);s.add_argument('--source-kit',required=True)
        if mode=='prepare':s.add_argument('--output',required=True)
        else:s.add_argument('--candidate',required=True);s.add_argument('--validation',required=True)
    a=p.parse_args();(prepare if a.mode=='prepare' else validate)(a)
if __name__=='__main__':main()

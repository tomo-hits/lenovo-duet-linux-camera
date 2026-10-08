#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise actual cold-boot guards; never touch host sysfs."""
from pathlib import Path
import importlib.util, tempfile, unittest
from unittest.mock import patch
HERE=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('scp_init',HERE/'packaging/postmarketos/external-scp-init.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Guards(unittest.TestCase):
 def test_no_clients(self):
  with tempfile.TemporaryDirectory() as d:
   m.check_clients(Path(d))
 def test_every_consumer_refused(self):
  for name in m.CONSUMERS:
   with self.subTest(name=name),tempfile.TemporaryDirectory() as d:
    p=Path(d)/'sys/module'/name;p.mkdir(parents=True)
    with self.assertRaisesRegex(ValueError,'consumer loaded'):m.check_clients(Path(d))
 def test_unknown_holder_refused(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'sys/module/mtk_scp/holders/unreviewed';p.mkdir(parents=True)
   with self.assertRaisesRegex(ValueError,'module holders'):m.check_clients(Path(d))
 def test_existing_provider_refused_without_stop(self):
  with patch.object(m.os,'geteuid',return_value=0),patch.object(m,'Path') as P,patch.object(m.subprocess,'run') as run:
   P.return_value.read_text.return_value=m.CONFIG
   P.return_value.exists.return_value=True
   with self.assertRaisesRegex(ValueError,'already loaded'):m.main([])
   run.assert_not_called();P.return_value.write_text.assert_not_called()
 def run_main(self, variant='ok', internal=False):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)
   def path(value):
    value=str(value)
    return root/value.lstrip('/') if value.startswith('/') else Path(value)
   config = m.INTERNAL_CONFIG if internal and variant != 'external_config' else m.CONFIG
   for f,v in {'etc/modprobe.d/duet-camera-scp-isolation.conf':config,
               'sys/block/mmcblk0/device/type':'MMC', 'sys/block/mmcblk0/ro':'1',
               'proc/sys/kernel/random/boot_id':'new-boot'}.items():
    p=root/f;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(v)
   for n in ('sda2','sda3'):
    node=root/'sys/devices/usb1/block/sda'/n;node.mkdir(parents=True)
    (node/'partition').write_text('2')
    p=root/'sys/class/block'/n;p.parent.mkdir(parents=True,exist_ok=True);p.symlink_to(node)
    p=root/'dev'/n;p.parent.mkdir(exist_ok=True);p.touch()
   mounts={'/':'/dev/sda3','/boot':'/dev/sda2'}
   if internal:
    for n in ('mmcblk0p2','mmcblk0p3'):
     node=root/'sys/devices/platform/mmc_host/block/mmcblk0'/n;node.mkdir(parents=True,exist_ok=True)
     (node/'partition').write_text('2')
     p=root/'sys/class/block'/n;p.symlink_to(node)
     p=root/'dev'/n;p.parent.mkdir(exist_ok=True);p.touch()
    disk=node.parent
    (disk/'device').mkdir();(disk/'device/type').write_text('SD' if variant=='sd' else 'MMC')
    (root/'sys/block/mmcblk0/ro').write_text('0')
    mounts={'/':'/dev/mmcblk0p3','/boot':'/dev/mmcblk0p2'}
    if variant=='mixed':mounts['/boot']='/dev/sda2'
    if variant=='not_partition':(node/'partition').unlink()
   if variant=='writable_mmc':(root/'sys/block/mmcblk0/ro').write_text('0')
   if variant=='duplicate':
    p=root/'run/duet-camera-scp-init-attempt.json';p.parent.mkdir();p.write_text('{}')
   (root/'run').mkdir(exist_ok=True)
   states=[]
   def output(args,**kw):
    if args[0]=='findmnt':return mounts[args[-1]]
    return '{"compatible":'+('false' if variant=='incompatible' else 'true')+'}'
   def load(args,**kw):
    states.append(args)
    (root/'sys/module/mtk_scp/holders').mkdir(parents=True)
    p=root/'sys/class/remoteproc/remoteproc0';p.mkdir(parents=True)
    (p/'name').write_text('scp');(p/'state').write_text('running')
    if variant != 'missing_recovery':
     (p/'recovery').write_text('disabled' if variant=='already_disabled' else 'enabled')
    if variant=='holder':(root/'sys/module/mtk_scp/holders/unknown').mkdir()
   original_write=Path.write_text;original_clients=m.check_clients
   stop_writes=[]
   recovery_writes=[]
   def write(p,value,*args,**kw):
    if p.name=='state' and value=='stop\n':
     stop_writes.append(value)
     return original_write(p,'running' if variant=='retained_ref' else 'offline')
    if p.name=='recovery' and value=='disabled\n':
     recovery_writes.append(value)
     return original_write(p,'enabled' if variant=='recovery_refused' else value)
    return original_write(p,value,*args,**kw)
   with patch.object(m,'check_clients',side_effect=lambda:original_clients(root)),patch.object(m,'Path',side_effect=path),patch.object(m.os,'geteuid',return_value=0),patch.object(m.subprocess,'check_output',side_effect=output),patch.object(m.subprocess,'run',side_effect=load),patch.object(Path,'write_text',write):
    if variant in ('ok','already_disabled'):
     m.main(['--internal-emmc'] if internal else [])
     self.assertEqual((root/'sys/class/remoteproc/remoteproc0/recovery').read_text().strip(),'disabled')
     self.assertEqual(len(recovery_writes),0 if variant=='already_disabled' else 1)
    else:
     with self.assertRaises((ValueError,FileExistsError)):m.main(['--internal-emmc'] if internal else [])
   return states,stop_writes
 def test_recovery_already_disabled_is_not_rewritten(self):
  self.run_main('already_disabled')
 def test_missing_recovery_refuses_activation(self):
  states,writes=self.run_main('missing_recovery');self.assertEqual(len(writes),1)
 def test_recovery_readback_required(self):
  states,writes=self.run_main('recovery_refused');self.assertEqual(len(writes),1)
 def test_main_single_reference(self):
  states,writes=self.run_main()
  self.assertEqual(states,[['modprobe','--ignore-install','mtk_scp']]);self.assertEqual(len(writes),1)
 def test_main_never_retries_stop(self):
  states,writes=self.run_main('retained_ref');self.assertEqual(len(writes),1)
 def test_main_guards_before_load(self):
  for name in ('writable_mmc','incompatible','duplicate'):
   with self.subTest(name=name):
    states,writes=self.run_main(name);self.assertEqual(states,[]);self.assertEqual(writes,[])
 def test_main_holder_prevents_stop(self):
  states,writes=self.run_main('holder');self.assertEqual(writes,[])
 def test_internal_mode_single_reference_and_no_retry(self):
  states,writes=self.run_main(internal=True)
  self.assertEqual(states,[['modprobe','--ignore-install','mtk_scp']]);self.assertEqual(len(writes),1)
  states,writes=self.run_main('retained_ref',internal=True);self.assertEqual(len(writes),1)
 def test_internal_guards_before_load(self):
  for variant in ('external_config','sd','mixed','not_partition','incompatible','duplicate'):
   with self.subTest(variant=variant):
    states,writes=self.run_main(variant,internal=True);self.assertEqual(states,[]);self.assertEqual(writes,[])
 def test_internal_holder_prevents_stop(self):
  states,writes=self.run_main('holder',internal=True);self.assertEqual(writes,[])
if __name__=='__main__':unittest.main()

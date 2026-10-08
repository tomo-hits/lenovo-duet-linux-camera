#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only compatibility check. Never boots, loads modules or writes media."""
from pathlib import Path
import json,hashlib,sys,argparse,importlib,subprocess
from fdt import normalized_input
ap=argparse.ArgumentParser();ap.add_argument('--root',default='/');ap.add_argument('--abi',default='/usr/share/duet-camera/kernel-abi.json');args=ap.parse_args();root=Path(args.root)
abi=json.loads(Path(args.abi).read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def firmware_sha(path):
 if path.exists():return sha(path),str(path),False
 compressed=path.with_name(path.name+'.zst')
 if not compressed.is_file():raise ValueError('SCP firmware missing (raw or .zst)')
 # Match Linux's raw-first lookup. Hash the bytes loaded by the kernel without
 # extracting firmware into the filesystem or changing the packaged input.
 try:
  data=subprocess.run(['zstd','--decompress','--stdout','--quiet','--',str(compressed)],check=True,capture_output=True).stdout
 except FileNotFoundError as e:raise ValueError('Install zstd to verify compressed SCP firmware') from e
 except subprocess.CalledProcessError as e:raise ValueError('Cannot decompress SCP firmware') from e
 return hashlib.sha256(data).hexdigest(),str(compressed),True
errors=[];observed={}
try:
 observed['kernel']=(root/'proc/sys/kernel/osrelease').read_text().strip()
 if observed['kernel']!=abi['kernel']:errors.append('kernel release mismatch')
 observed['kernel_notes_sha256']=sha(root/'sys/kernel/notes')
 if observed['kernel_notes_sha256']!=abi['kernel_notes_sha256']:errors.append('kernel build mismatch')
 _,tree,_=normalized_input((root/'sys/firmware/fdt').read_bytes());observed['compatible']=tree.strings('/','compatible')
 if abi['compatible'] not in observed['compatible']:errors.append('unsupported board')
 spec=json.loads(Path(__file__).with_name('camera-nodes.json').read_text())
 # Validate the complete graph, including a narrowly checked legacy mapping.
 # An unintegrated stock tree is never accepted just because we can produce it.
 _,audit=importlib.import_module('integrate-camera-dtb').integrate((root/'sys/firmware/fdt').read_bytes(),spec)
 observed['camera_nodes_present']=audit['already_integrated']
 observed['camera_graph_matches']=audit['already_integrated']
 observed['legacy_camera_binding']=audit['legacy_binding_migrated']
 if not observed['camera_nodes_present']:errors.append('camera device tree not active')
 observed['firmware_sha256'],observed['firmware_path'],observed['firmware_compressed']=firmware_sha(root/'lib/firmware/mediatek/mt8183/scp.img')
 if observed['firmware_sha256']!=abi['firmware_sha256']:errors.append('firmware ABI mismatch')
except (OSError,ValueError,KeyError,AssertionError) as e:errors.append(str(e))
print(json.dumps(dict(compatible=not errors,observed=observed,errors=errors,read_only=True)),flush=True)
sys.exit(bool(errors))

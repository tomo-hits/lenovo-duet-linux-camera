#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: GPL-2.0-only
"""Integrate camera-only nodes by provider paths; no boot/storage operations."""
from pathlib import Path
import json,struct,sys,hashlib
from fdt import Fdt,normalized_input,cells
from legacy_bindings import canonicalize


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)


def serialize(nodes,reserve,boot_cpu):
 strings=bytearray();offsets={};body=bytearray();align=lambda b:b+b'\0'*(-len(b)%4)
 def node(p):
  body.extend(cells(1)+align((p.rsplit('/',1)[-1] if p!='/' else '').encode()+b'\0'))
  for k,v in nodes[p].items():
   if k not in offsets:offsets[k]=len(strings);strings.extend(k.encode()+b'\0')
   body.extend(cells(3,len(v),offsets[k])+align(v))
  for c in nodes:
   if c!='/' and (c.rsplit('/',1)[0] or '/')==p:node(c)
  body.extend(cells(2))
 node('/');body.extend(cells(9));start=40+len(reserve);names=start+len(body)
 return cells(0xd00dfeed,names+len(strings),start,names,40,17,16,boot_cpu,len(strings),len(body))+reserve+body+strings

def integrate(raw,spec):
 _,base,_=normalized_input(raw);require(spec['compatible'] in base.strings('/','compatible'), 'unsupported board')
 require(len(spec['nodes'])==26, "Invalid or incompatible input")
 canonical,legacy=canonicalize(base.nodes)
 nodes={p:dict(v) for p,v in canonical.items()};existing=[];handles={}
 for p,v in nodes.items():
  if 'phandle'in v:
   n=struct.unpack('>I',v['phandle'])[0];require(n not in existing and n not in (0,0xffffffff), "Invalid or incompatible input");existing.append(n);handles[p]=n
 next_handle=max(existing)+1
 def handle(p):
  nonlocal next_handle
  require(p in nodes, 'missing provider '+p)
  if p not in handles:
   require(next_handle<0xffffffff, "Invalid or incompatible input");handles[p]=next_handle;next_handle+=1;nodes[p]['phandle']=cells(handles[p])
  return handles[p]
 overlap=set(nodes)&set(spec['nodes']);require(not overlap or overlap==set(spec['nodes']), 'partial camera integration')
 for p in sorted(spec['nodes'],key=lambda p:(p.count('/'),p)):
  require((p.rsplit('/',1)[0] or '/')in nodes, "Invalid or incompatible input")
  if p not in nodes:nodes[p]={}
 for p,props in spec['nodes'].items():
  if 'phandle'in props:handle(p)
 for p,props in spec['nodes'].items():
  expected={}
  for k,v in props.items():
   if 'hex'in v:expected[k]=bytes.fromhex(v['hex'])
   elif 'self'in v:expected[k]=cells(handle(p))
   else:expected[k]=cells(*(handle(x['node']) if isinstance(x,dict) else x for x in v['cells']))
  if overlap:require(nodes[p]==expected, ('existing camera mismatch',p))
  else:nodes[p]=expected
 for p,props in canonical.items():
  for k,v in props.items():require(nodes[p][k]==v, ('original property changed',p,k))
 out=serialize(nodes,base.reservation_bytes,base.boot_cpu);parsed=Fdt(out)
 require(parsed.nodes==nodes and parsed.reservations==base.reservations and parsed.boot_cpu==base.boot_cpu, "Invalid or incompatible input")
 return out,dict(camera_nodes=26,provider_references_by_path=True,original_properties_preserved=not legacy,legacy_binding_migrated=legacy,non_camera_properties_preserved=True,reservations_preserved=True,already_integrated=bool(overlap))
if __name__=='__main__':
 src,out=map(Path,sys.argv[1:3]);require(src.resolve()!=out.resolve() and not out.exists() and not out.is_symlink(), "Invalid or incompatible input")
 spec=json.loads(Path(__file__).with_name('camera-nodes.json').read_text());raw=src.read_bytes();result,audit=integrate(raw,spec)
 with out.open('xb') as f:f.write(result)
 print(json.dumps(dict(**audit,source_sha256=hashlib.sha256(raw).hexdigest(),output_sha256=hashlib.sha256(result).hexdigest())),flush=True)

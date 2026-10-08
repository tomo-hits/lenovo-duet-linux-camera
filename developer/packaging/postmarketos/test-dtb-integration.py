#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: MIT
"""Production integration regression, using compiled real board DTBs."""
from pathlib import Path
import importlib,json,sys,copy
from fdt import normalized_input,cells

def require(condition):
    if not condition:
        raise AssertionError("DT regression failed")

impl=importlib.import_module('integrate-camera-dtb')
spec=json.loads(Path(__file__).with_name('camera-nodes.json').read_text())
stock=Path(sys.argv[1]).read_bytes();active=Path(sys.argv[2]).read_bytes()
out,audit=impl.integrate(stock,spec);require(not audit['already_integrated'])
again,audit=impl.integrate(out,spec);require(again==out and audit['already_integrated'])
_,base,_=normalized_input(stock);_,tree,_=normalized_input(out)
require(all(tree.nodes[n][k]==v for n,p in base.nodes.items() for k,v in p.items()))
_,audit=impl.integrate(active,spec);require(audit['already_integrated'])
def reject(raw,label):
 try:impl.integrate(raw,spec)
 except (AssertionError,ValueError,KeyError):return label
 raise RuntimeError('accepted invalid tree: '+label)
bad=[]
nodes=copy.deepcopy(base.nodes);nodes['/']['compatible']=b'google,unsupported\0'
bad.append(reject(impl.serialize(nodes,base.reservation_bytes,base.boot_cpu),'wrong SKU'))
nodes=copy.deepcopy(tree.nodes);del nodes[next(n for n in spec['nodes'] if not any(x.startswith(n+'/') for x in spec['nodes']))]
bad.append(reject(impl.serialize(nodes,tree.reservation_bytes,tree.boot_cpu),'partial integration'))
nodes=copy.deepcopy(tree.nodes);p=next(p for p,v in nodes.items() if p in spec['nodes'] and 'reg' in v);nodes[p]['reg']=cells(0xdead)
bad.append(reject(impl.serialize(nodes,tree.reservation_bytes,tree.boot_cpu),'wrong camera register'))
nodes=copy.deepcopy(tree.nodes);p=next(p for p,v in nodes.items() if p in spec['nodes'] and 'remote-endpoint' in v);nodes[p]['remote-endpoint']=cells(0xdead)
bad.append(reject(impl.serialize(nodes,tree.reservation_bytes,tree.boot_cpu),'wrong endpoint provider'))
nodes=copy.deepcopy(base.nodes);p=next(v['cells'][0]['node'] for v in spec['nodes'].values() for v in v.values() if 'cells' in v and v['cells'] and isinstance(v['cells'][0],dict));del nodes[p]
bad.append(reject(impl.serialize(nodes,base.reservation_bytes,base.boot_cpu),'missing provider'))
bad.append(reject(stock[:-8],'truncated FDT'))
for compat in (b'mediatek,mt8183-p1-raw', b'codex,mt8183-p1-integrated',
               *impl.canonicalize.__globals__['EXCLUSIVE']):
 nodes=copy.deepcopy(tree.nodes);nodes['/other-p1']={'compatible':compat+b'\0'}
 bad.append(reject(impl.serialize(nodes,tree.reservation_bytes,tree.boot_cpu),
                   'unexpected P1 '+compat.decode()))
nodes=copy.deepcopy(tree.nodes)
nodes['/camera@1a006000']['compatible'] += b'codex,mt8183-p1-integrated\0'
bad.append(reject(impl.serialize(nodes,tree.reservation_bytes,tree.boot_cpu), 'multiple compatible aliases'))
print(json.dumps({'compiled_stock_integrated':True,'all_original_properties_preserved':True,'compiled_tree_idempotent':True,'actual_active_tree_matches':True,'negative_cases_rejected':bad}),flush=True)

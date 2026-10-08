# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only migration of the original development DT camera namespace."""


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)

OLD_PATH = '/p1-integrated-test'
NEW_PATH = '/camera@1a006000'
OLD_COMPAT = b'codex,mt8183-p1-integrated\0'
NEW_COMPAT = b'mediatek,mt8183-p1-raw\0'

EXCLUSIVE = {b'mediatek,mt8183-camisp', *(b'codex,mt8183-p1-'+x for x in
    (b'composer', b'dma-probe', b'graph', b'resources'))}

def canonicalize(nodes):
    # Match the driver's whole-tree uniqueness and competing-driver guards.
    identities = []
    for path, props in nodes.items():
        raw = props.get('compatible', b'')
        values = set(raw.split(b'\0'))
        require(not values & EXCLUSIVE, ('competing P1 driver', path))
        if values & {OLD_COMPAT[:-1], NEW_COMPAT[:-1]}:
            require((path, raw) in ((OLD_PATH, OLD_COMPAT),
                                  (NEW_PATH, NEW_COMPAT)), ('P1 path/binding mismatch', path))
            identities.append(path)
    require(len(identities) <= 1, 'multiple P1 identities')
    legacy = OLD_PATH in nodes
    if not legacy:
        return {p:dict(v) for p,v in nodes.items()}, False
    require(NEW_PATH not in nodes, 'mixed old/new P1 nodes')
    require(nodes[OLD_PATH].get('compatible') == OLD_COMPAT, 'unexpected old P1 binding')
    family = [p for p in nodes if p == OLD_PATH or p.startswith(OLD_PATH+'/')]
    require(len(family) == 4, 'unexpected legacy P1 graph')
    result = {}
    for p,props in nodes.items():
        target = NEW_PATH+p[len(OLD_PATH):] if p in family else p
        require(target not in result, "Invalid or incompatible input")
        result[target] = dict(props)
    result[NEW_PATH]['compatible'] = NEW_COMPAT
    return result, True

#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
# SPDX-License-Identifier: GPL-2.0-only
"""Audit actual arm64 PREL32 exports in a public uncompressed kernel Image.

No inferred exports: validates every table record against System.map. CRC zero
is permitted only when the public config explicitly disables MODVERSIONS.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)


p = argparse.ArgumentParser()
p.add_argument("--image", type=Path, required=True)
p.add_argument("--system-map", type=Path, required=True)
p.add_argument("--config", type=Path, required=True)
p.add_argument("--output-dir", type=Path, required=True)
a = p.parse_args()
image = a.image.read_bytes()
require(image[56:60] == b"ARM\x64", "Not an uncompressed arm64 Image")
require("# CONFIG_MODVERSIONS is not set" in a.config.read_text(), "Invalid or incompatible input")
require("CONFIG_ARM64=y" in a.config.read_text(), "Invalid or incompatible input")
symbols = {}
for line in a.system_map.read_text().splitlines():
    addr, kind, name = line.split()
    symbols.setdefault(name, set()).add(int(addr, 16))
def unique(name):
    values = symbols[name]
    require(len(values) == 1, (name, values))
    return next(iter(values))
base = unique("_text")
def offset(address, width=1):
    off = address - base
    require(0 <= off <= len(image) - width, (hex(address), off, width))
    return off
def cstr(address):
    off = offset(address)
    end = image.index(b"\0", off)
    require(end - off < 512, "Invalid or incompatible input")
    return image[off:end].decode("ascii")
records = []
for suffix, export in (("", "EXPORT_SYMBOL"), ("_gpl", "EXPORT_SYMBOL_GPL")):
    start = unique("__start___ksymtab" + suffix)
    stop = unique("__stop___ksymtab" + suffix)
    require((stop - start) % 12 == 0, "Invalid or incompatible input")
    for addr in range(start, stop, 12):
        rels = struct.unpack_from("<iii", image, offset(addr, 12))
        value, name_addr, ns_addr = (addr + 4 * i + rel for i, rel in enumerate(rels))
        name, namespace = cstr(name_addr), cstr(ns_addr)
        require(unique("__ksymtab_" + name) == addr, name)
        require(value in symbols[name], (name, hex(value), symbols[name]))
        require("\t" not in namespace and "\n" not in namespace, "Invalid or incompatible input")
        records.append(dict(name=name, provider="vmlinux", export=export,
                            namespace=namespace, crc="0x00000000",
                            table_address=hex(addr), value=hex(value)))
require(len({r["name"] for r in records}) == len(records), "Invalid or incompatible input")
records.sort(key=lambda r: r["name"])
a.output_dir.mkdir(exist_ok=True, parents=True)
(a.output_dir / "public-kernel-exports.symvers").write_text("".join(
    f'{r["crc"]}\t{r["name"]}\tvmlinux\t{r["export"]}\t{r["namespace"]}\n'
    for r in records))
report = dict(kernel="6.18.28-mt81", method="All actual arm64 PREL32 builtin exports; checked table/name/value against matching System.map; namespace read from Image; MODVERSIONS disabled. Public external modules are not covered by this table.",
              inputs=[dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                      for path in (a.image, a.system_map, a.config)], symbols=records)
(a.output_dir / "public-kernel-exports.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"audited_builtin_exports": len(records), "gpl_exports": sum(r["export"] == "EXPORT_SYMBOL_GPL" for r in records), "namespaced_exports": sum(bool(r["namespace"]) for r in records)}))

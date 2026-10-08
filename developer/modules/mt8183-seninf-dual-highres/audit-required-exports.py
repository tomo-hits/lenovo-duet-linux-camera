#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Resolve this module's references from audited Image and actual public ELFs.

Reads AArch64 ELF64 PREL32 export records, including GPL class and namespace.
No symbols are inferred from source declarations or modules.symbols.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cstring(data, offset):
    assert 0 <= offset < len(data)
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii")


def module_exports(data, provider):
    assert data[:7] == b"\x7fELF\x02\x01\x01"
    assert struct.unpack_from("<HH", data, 16) == (1, 183)  # ET_REL, AArch64
    offset = struct.unpack_from("<Q", data, 40)[0]
    entsize, count, strindex = struct.unpack_from("<HHH", data, 58)
    assert entsize == 64
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * entsize)
                for i in range(count)]

    def section_bytes(index):
        section = sections[index]
        value = data[section[4]:section[4] + section[5]]
        assert len(value) == section[5]
        return value

    strings = section_bytes(strindex)
    names = [cstring(strings, section[0]) for section in sections]
    symindex = names.index(".symtab")
    symsec = sections[symindex]
    assert symsec[1] == 2 and symsec[9] == 24
    symstrings = section_bytes(symsec[6])
    symbols = []
    for pos in range(0, symsec[5], 24):
        name, info, other, index, value, size = struct.unpack_from(
            "<IBBHQQ", data, symsec[4] + pos)
        symbols.append((cstring(symstrings, name), index, value))
    by_name = {sym[0]: sym for sym in symbols if sym[0]}
    records = []
    for section_name, export in (("__ksymtab", "EXPORT_SYMBOL"),
                                 ("__ksymtab_gpl", "EXPORT_SYMBOL_GPL")):
        if section_name not in names:
            continue
        index = names.index(section_name)
        table = sections[index]
        assert table[5] % 12 == 0
        relocations = {}
        for relsec in sections:
            if relsec[1] != 4 or relsec[7] != index:
                continue
            assert relsec[6] == symindex and relsec[9] == 24
            for pos in range(0, relsec[5], 24):
                target, info, addend = struct.unpack_from(
                    "<QQq", data, relsec[4] + pos)
                assert info & 0xffffffff == 261  # R_AARCH64_PREL32
                assert target not in relocations
                relocations[target] = (symbols[info >> 32], addend)
        assert set(relocations) == set(range(0, table[5], 4))

        def rel_string(pos):
            symbol, addend = relocations[pos]
            assert names[symbol[1]] == "__ksymtab_strings"
            return cstring(section_bytes(symbol[1]), symbol[2] + addend)

        for pos in range(0, table[5], 12):
            name, namespace = rel_string(pos + 4), rel_string(pos + 8)
            symbol, addend = relocations[pos]
            assert symbol[1] not in (0, 0xfff1)  # must resolve inside this ELF
            target = by_name[name]
            assert (symbol[1], symbol[2] + addend) == target[1:]
            assert by_name["__ksymtab_" + name][1:] == (index, pos)
            assert "\t" not in namespace and "\n" not in namespace
            records.append(dict(name=name, provider=provider, export=export,
                                namespace=namespace, crc="0x00000000"))
    assert len({r["name"] for r in records}) == len(records)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builtin-json", type=Path, required=True)
    parser.add_argument("--module-root", type=Path, required=True)
    parser.add_argument("--undefined-list", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    builtin = json.loads(args.builtin_json.read_text())
    for item in builtin["inputs"]:
        assert digest(Path(item["path"])) == item["sha256"], item["path"]
    configs = [Path(item["path"]) for item in builtin["inputs"]
               if Path(item["path"]).name == "config"]
    assert len(configs) == 1
    assert "# CONFIG_MODVERSIONS is not set" in configs[0].read_text()
    paths = ["drivers/media/v4l2-core/" + name
             for name in ("videodev", "v4l2-async", "v4l2-fwnode")]
    paths.append("drivers/media/mc/mc")
    media_records, inputs = [], []
    for provider in paths:
        path = args.module_root / "kernel" / (provider + ".ko.zst")
        data = subprocess.check_output(["zstd", "-d", "-c", str(path)])
        records = module_exports(data, provider)
        media_records.extend(records)
        inputs.append(dict(path=str(path), sha256=digest(path),
                           elf_sha256=hashlib.sha256(data).hexdigest(),
                           exports=len(records)))
    records = builtin["symbols"] + media_records
    by_name = {record["name"]: record for record in records}
    assert len(by_name) == len(records)
    required = {line.split()[-1] for line in args.undefined_list.read_text().splitlines()}
    required.discard("__this_module")  # supplied by ordinary Kbuild modpost
    assert required <= by_name.keys(), sorted(required - by_name.keys())
    selected = [by_name[name] for name in sorted(required)]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "required-exports.symvers").write_text("".join(
        f'{r["crc"]}\t{r["name"]}\t{r["provider"]}\t{r["export"]}\t{r["namespace"]}\n'
        for r in selected))
    report = dict(kernel="6.18.28-mt81", builtin_audit_sha256=digest(args.builtin_json),
                  inputs=builtin["inputs"] + inputs, media_export_records=media_records,
                  required_symbols=selected, missing_symbols=[],
                  scope="Actual Image + four public media ELFs; CRC zero only because MODVERSIONS is disabled")
    (args.output_dir / "required-exports.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(required=len(selected), audited_media_exports=len(media_records),
                         missing=0)))


if __name__ == "__main__":
    main()

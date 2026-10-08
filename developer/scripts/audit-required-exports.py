#!/usr/bin/env python3
# Modified on 2026-10-05: publication review fixes; see docs/PUBLICATION_REVIEW.json.
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


def require(condition, message="Invalid or incompatible input"):
    if not condition:
        raise ValueError(message)



def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cstring(data, offset):
    require(0 <= offset < len(data), "Invalid or incompatible input")
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii")


def module_exports(data, provider):
    require(data[:7] == b"\x7fELF\x02\x01\x01", "Invalid or incompatible input")
    require(struct.unpack_from("<HH", data, 16) == (1, 183), "Invalid or incompatible input")  # ET_REL, AArch64
    offset = struct.unpack_from("<Q", data, 40)[0]
    entsize, count, strindex = struct.unpack_from("<HHH", data, 58)
    require(entsize == 64, "Invalid or incompatible input")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * entsize)
                for i in range(count)]

    def section_bytes(index):
        section = sections[index]
        value = data[section[4]:section[4] + section[5]]
        require(len(value) == section[5], "Invalid or incompatible input")
        return value

    strings = section_bytes(strindex)
    names = [cstring(strings, section[0]) for section in sections]
    symindex = names.index(".symtab")
    symsec = sections[symindex]
    require(symsec[1] == 2 and symsec[9] == 24, "Invalid or incompatible input")
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
        require(table[5] % 12 == 0, "Invalid or incompatible input")
        relocations = {}
        for relsec in sections:
            if relsec[1] != 4 or relsec[7] != index:
                continue
            require(relsec[6] == symindex and relsec[9] == 24, "Invalid or incompatible input")
            for pos in range(0, relsec[5], 24):
                target, info, addend = struct.unpack_from(
                    "<QQq", data, relsec[4] + pos)
                require(info & 0xffffffff == 261, "Invalid or incompatible input")  # R_AARCH64_PREL32
                require(target not in relocations, "Invalid or incompatible input")
                relocations[target] = (symbols[info >> 32], addend)
        require(set(relocations) == set(range(0, table[5], 4)), "Invalid or incompatible input")

        def rel_string(pos):
            symbol, addend = relocations[pos]
            require(names[symbol[1]] == "__ksymtab_strings", "Invalid or incompatible input")
            return cstring(section_bytes(symbol[1]), symbol[2] + addend)

        for pos in range(0, table[5], 12):
            name, namespace = rel_string(pos + 4), rel_string(pos + 8)
            symbol, addend = relocations[pos]
            require(symbol[1] not in (0, 0xfff1), "Invalid or incompatible input")  # must resolve inside this ELF
            target = by_name[name]
            require((symbol[1], symbol[2] + addend) == target[1:], "Invalid or incompatible input")
            require(by_name["__ksymtab_" + name][1:] == (index, pos), "Invalid or incompatible input")
            require("\t" not in namespace and "\n" not in namespace, "Invalid or incompatible input")
            records.append(dict(name=name, provider=provider, export=export,
                                namespace=namespace, crc="0x00000000"))
    require(len({r["name"] for r in records}) == len(records), "Invalid or incompatible input")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builtin-json", type=Path, required=True)
    parser.add_argument("--module-root", type=Path, required=True)
    parser.add_argument("--undefined-list", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sensor-module", type=Path, required=True)
    parser.add_argument("--sensor-sha", required=True)
    args = parser.parse_args()
    builtin = json.loads(args.builtin_json.read_text())
    for item in builtin["inputs"]:
        require(digest(Path(item["path"])) == item["sha256"], item["path"])
    configs = [Path(item["path"]) for item in builtin["inputs"]
               if Path(item["path"]).name == "config"]
    require(len(configs) == 1, "Invalid or incompatible input")
    require("# CONFIG_MODVERSIONS is not set" in configs[0].read_text(), "Invalid or incompatible input")
    paths = ["drivers/media/v4l2-core/" + name
             for name in ("videodev", "v4l2-async", "v4l2-fwnode")]
    paths.append("drivers/media/mc/mc")
    paths.extend("drivers/media/common/videobuf2/"+name for name in ("videobuf2-common", "videobuf2-v4l2", "videobuf2-vmalloc", "videobuf2-memops"))
    paths.extend(["drivers/remoteproc/mtk_scp", "drivers/remoteproc/mtk_scp_ipi"])
    media_records, inputs = [], []
    for provider in paths:
        path = args.module_root / "kernel" / (provider + ".ko.zst")
        data = subprocess.check_output(["zstd", "-d", "-c", str(path)])
        records = module_exports(data, provider)
        media_records.extend(records)
        inputs.append(dict(path=str(path), sha256=digest(path),
                           elf_sha256=hashlib.sha256(data).hexdigest(),
                           exports=len(records)))
    require(digest(args.sensor_module) == args.sensor_sha, "Invalid or incompatible input")
    sensor_records = module_exports(args.sensor_module.read_bytes(), "ov8856")
    # Pure upstream OV8856 has no private sensor exports. Neither capture nor
    # receiver may depend on a front telemetry interface.
    require(not sensor_records, "Invalid or incompatible input")
    inputs.append(dict(path=str(args.sensor_module), sha256=args.sensor_sha, exports=len(sensor_records)))
    records = builtin["symbols"] + media_records + sensor_records
    by_name = {record["name"]: record for record in records}
    require(len(by_name) == len(records), "Invalid or incompatible input")
    required = {line.split()[-1] for line in args.undefined_list.read_text().splitlines()}
    required.discard("__this_module")  # supplied by ordinary Kbuild modpost
    require(required <= by_name.keys(), sorted(required - by_name.keys()))
    selected = [by_name[name] for name in sorted(required)]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "required-exports.symvers").write_text("".join(
        f'{r["crc"]}\t{r["name"]}\t{r["provider"]}\t{r["export"]}\t{r["namespace"]}\n'
        for r in selected))
    report = dict(kernel="6.18.28-mt81", builtin_audit_sha256=digest(args.builtin_json),
                  inputs=builtin["inputs"] + inputs, media_export_records=media_records,
                  required_symbols=selected, missing_symbols=[],
                  sensor_export_records=sensor_records, scope="Actual Image/public media/SCP and pinned sensor ELF; MODVERSIONS disabled")
    (args.output_dir / "required-exports.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(required=len(selected), audited_media_exports=len(media_records),
                         missing=0)))


if __name__ == "__main__":
    main()

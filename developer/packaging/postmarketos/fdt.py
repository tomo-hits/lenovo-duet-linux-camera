#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Add only the reviewed SKU176 sensor/pinctrl nodes to a supplied DTB.

Runs dtc/fdtoverlay locally; never connects to a device or installs a DTB.
Existing properties, reservation entries, and boot CPU ID must remain intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


class Fdt:
    def __init__(self, data):
        self.data = data
        if len(data) < 40:
            raise ValueError("DTB header too short")
        magic, size, st, strings, reserve, version, compat, self.boot_cpu, slen, tlen = struct.unpack_from(">10I", data)
        if magic != 0xd00dfeed or version != 17 or compat > 17 or size > len(data):
            raise ValueError("Unsupported or invalid DTB header")
        if max(st + tlen, strings + slen, reserve + 16) > size:
            raise ValueError("DTB block outside totalsize")
        self.reservations = []
        pos = reserve
        while True:
            if pos + 16 > size:
                raise ValueError("Unterminated reservation map")
            address, length = struct.unpack_from(">QQ", data, pos)
            pos += 16
            if address == length == 0:
                break
            self.reservations.append((address, length))
        self.reservation_bytes = data[reserve:pos]
        self.nodes = {}
        stack = []
        pos, end = st, st + tlen
        while pos + 4 <= end:
            token = struct.unpack_from(">I", data, pos)[0]
            pos += 4
            if token == 1:
                stop = data.index(b"\0", pos, end)
                name = data[pos:stop].decode("ascii")
                pos = (stop + 4) & ~3
                path = "/" if not stack else stack[-1].rstrip("/") + "/" + name
                if path in self.nodes or (not stack and name):
                    raise ValueError("Duplicate/invalid node")
                self.nodes[path] = {}
                stack.append(path)
            elif token == 2:
                if not stack:
                    raise ValueError("Unbalanced DTB node")
                stack.pop()
            elif token == 3:
                if not stack or pos + 8 > end:
                    raise ValueError("Invalid property")
                length, nameoff = struct.unpack_from(">II", data, pos)
                pos += 8
                if nameoff >= slen or pos + length > end:
                    raise ValueError("Property outside block")
                stop = data.index(b"\0", strings + nameoff, strings + slen)
                name = data[strings + nameoff:stop].decode("ascii")
                if name in self.nodes[stack[-1]]:
                    raise ValueError("Duplicate property")
                self.nodes[stack[-1]][name] = data[pos:pos + length]
                pos = (pos + length + 3) & ~3
            elif token == 4:
                continue
            elif token == 9:
                if stack or "/" not in self.nodes:
                    raise ValueError("Unbalanced tree")
                return
            else:
                raise ValueError("Unknown DTB structure token")
        raise ValueError("Missing DTB end marker")

    def strings(self, path, name):
        value = self.nodes[path][name]
        if not value.endswith(b"\0"):
            raise ValueError("Expected string property")
        return value[:-1].decode("ascii").split("\0")

    def cell(self, path, name):
        value = self.nodes[path][name]
        if len(value) != 4:
            raise ValueError("Expected one cell")
        return struct.unpack(">I", value)[0]

    def unique(self, property_name, value):
        matches = [path for path, props in self.nodes.items()
                   if property_name in props and value in self.strings(path, property_name)]
        if len(matches) != 1:
            raise ValueError(f"Expected one provider for {property_name}={value}")
        return matches[0]

    def phandle(self, path):
        handle = self.cell(path, "phandle")
        if handle in (0, 0xffffffff):
            raise ValueError("Invalid provider phandle")
        for other, props in self.nodes.items():
            if other != path and props.get("phandle") == struct.pack(">I", handle):
                raise ValueError("Duplicate provider phandle")
        return handle


def cells(*values):
    return struct.pack(">" + "I" * len(values), *values)


def normalized_input(data):
    try:
        return data, Fdt(data), None
    except ValueError as error:
        if str(error) != "Missing DTB end marker":
            raise
    header = struct.unpack_from(">10I", data)
    structure_end = header[2] + header[9]
    if structure_end + 4 == header[3] and data[structure_end:structure_end + 4] == cells(9):
        fixed = bytearray(data)
        struct.pack_into(">I", fixed, 36, header[9] + 4)
        fixed = bytes(fixed)
        if fixed[:36] != data[:36] or fixed[40:] != data[40:]:
            raise ValueError("Header normalization changed other bytes")
        return fixed, Fdt(fixed), {"field": "size_dt_struct", "byte_offset": 36,
                                  "old": header[9], "new": header[9] + 4,
                                  "reason": "FDT_END is immediately beyond declared structure and immediately before strings; include its 4 bytes"}
    # The observed RAM-repeat FDT instead ends at the root END_NODE, with
    # strings immediately following. Add only the stream terminator; strict
    # re-parsing below still rejects an unclosed root or a truncated property.
    if (structure_end != header[3] or header[3] + header[8] != header[1] or
            header[1] != len(data) or header[2] % 4 or header[9] % 4 or
            data[structure_end - 4:structure_end] != cells(2) or
            header[4] < 40 or header[4] % 8):
        raise ValueError("Missing end marker is not an audited runtime layout")
    reserve_end = header[4]
    while reserve_end + 16 <= header[2]:
        reservation = struct.unpack_from(">QQ", data, reserve_end)
        reserve_end += 16
        if reservation == (0, 0):
            break
    else:
        raise ValueError("Reservation map must end before the structure block")
    fixed = bytearray(data[:structure_end] + cells(9) + data[structure_end:])
    updates = {"totalsize": (4, header[1]), "off_dt_strings": (12, header[3]),
               "size_dt_struct": (36, header[9])}
    for offset, old in updates.values():
        struct.pack_into(">I", fixed, offset, old + 4)
    restored = bytearray(fixed[:structure_end] + fixed[structure_end + 4:])
    for offset, old in updates.values():
        struct.pack_into(">I", restored, offset, old)
    if restored != data:
        raise ValueError("Terminator normalization changed other bytes")
    fixed = bytes(fixed)
    return fixed, Fdt(fixed), {
        "kind": "insert_fdt_end_before_strings", "byte_offset": structure_end,
        "inserted_hex": cells(9).hex(),
        "header_updates": {name: {"byte_offset": offset, "old": old, "new": old + 4}
                           for name, (offset, old) in updates.items()},
        "reason": "Structure ends at the closed root END_NODE and strings start immediately; insert only FDT_END"}


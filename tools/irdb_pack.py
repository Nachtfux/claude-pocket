#!/usr/bin/env python3
"""Pack the Flipper-IRDB into a compact binary that is embedded in the
firmware (no SD card needed).

    python3 tools/irdb_pack.py <path/to/Flipper-IRDB> firmware/assets/irdb.bin

Source: https://github.com/Lucaslhm/Flipper-IRDB (CC0-1.0). The `_Converted_`
tree (bulk conversions from other databases, ~7k files) is skipped to keep
the blob around 1 MB; so is `_Converted_/Power_off.ir`.

Layout (all little-endian, offsets absolute from the start of the blob):

  header   "IRDB" u32 version, u32 n_nodes, u32 n_signals, u32 n_names,
           u32 off_nodes, u32 off_signals, u32 off_names, u32 off_strings,
           u32 off_raw                                          (40 bytes)
  nodes    n_nodes x 12 bytes: u32 name_off, u32 first, u16 count, u16 flags
           flags bit0 = device (first/count index signals), otherwise
           first/count index child nodes. Node 0 is the root.
  signals  n_signals x 11 bytes: u16 name_idx, u8 proto, u32 a, u32 c
           proto 0 = raw: a = offset of the raw record, c = carrier Hz
  names    n_names x u32 string offsets (deduplicated button names)
  strings  NUL-terminated UTF-8
  raw      u16 n, u8 k, u8 bits, k x u16 levels, packed level indices
           (LSB-first bitstream, `bits` per timing). k == 0 means the n
           timings follow as plain u16 (signals with > 16 distinct levels).
"""

import math
import os
import re
import struct
import sys

PROTOS = {
    "NEC": 1, "NECext": 2, "Samsung32": 3, "Kaseikyo": 4, "SIRC": 5,
    "SIRC15": 6, "SIRC20": 7, "RC5": 8, "RC5X": 9, "RC6": 10,
}
SKIP_DIRS = {".github", "_Converted_"}
MAX_RAW = 1024          # firmware's ir::MAX_RAW
NODE_DEVICE = 1


def parse_ir(path):
    text = open(path, encoding="utf-8", errors="replace").read().replace("\r", "")
    for block in re.split(r"\n#[^\n]*", text):
        fields = dict(re.findall(r"^(\w+):[ \t]*(.*)$", block, re.M))
        if "name" in fields:
            yield fields


def le_bytes(s):
    v = 0
    for i, b in enumerate(s.split()[:4]):
        v |= (int(b, 16) & 0xFF) << (8 * i)
    return v


def cluster(timings, tol=0.08):
    """Group near-identical durations; return (levels, index per timing)."""
    groups = []
    for x in sorted(set(timings)):
        if groups and x <= groups[-1][0] * (1 + tol) + 4:
            groups[-1][1].append(x)
        else:
            groups.append([x, [x]])
    levels = [round(sum(g) / len(g)) for _, g in groups]
    lookup = {}
    for gi, (_, members) in enumerate(groups):
        for m in members:
            lookup[m] = gi
    return levels, [lookup[t] for t in timings]


def pack_raw(timings):
    timings = [min(t, 0xFFFF) for t in timings]
    levels, idx = cluster(timings)
    if len(levels) > 16:
        return struct.pack("<HBB", len(timings), 0, 16) + struct.pack(
            "<%dH" % len(timings), *timings)
    bits = max(1, math.ceil(math.log2(len(levels))))
    acc = nacc = 0
    out = bytearray()
    for i in idx:
        acc |= i << nacc
        nacc += bits
        while nacc >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nacc -= 8
    if nacc:
        out.append(acc & 0xFF)
    return (struct.pack("<HBB", len(timings), len(levels), bits)
            + struct.pack("<%dH" % len(levels), *levels) + bytes(out))


class Packer:
    def __init__(self):
        self.strings = bytearray()
        self.string_off = {}
        self.names = []
        self.name_idx = {}
        self.signals = []        # (name_idx, proto, a, c)
        self.raw = bytearray()
        self.raw_off = {}
        self.stats = {"parsed": 0, "raw": 0, "skipped": 0}

    def string(self, s):
        if s not in self.string_off:
            self.string_off[s] = len(self.strings)
            self.strings += s.encode("utf-8") + b"\0"
        return self.string_off[s]

    def name(self, s):
        if s not in self.name_idx:
            self.name_idx[s] = len(self.names)
            self.names.append(self.string(s))
        return self.name_idx[s]

    def device_signals(self, path):
        sigs = []
        for f in parse_ir(path):
            name = f["name"].strip()[:40]
            if f.get("type") == "parsed" and f.get("protocol") in PROTOS:
                sigs.append((self.name(name), PROTOS[f["protocol"]],
                             le_bytes(f.get("address", "")),
                             le_bytes(f.get("command", ""))))
                self.stats["parsed"] += 1
            elif f.get("type") == "raw":
                t = [int(x) for x in f.get("data", "").split() if x.isdigit()]
                if not t or len(t) > MAX_RAW:
                    self.stats["skipped"] += 1
                    continue
                key = tuple(t)
                if key not in self.raw_off:
                    self.raw_off[key] = len(self.raw)
                    self.raw += pack_raw(t)
                freq = int(float(f.get("frequency", "38000") or 38000))
                sigs.append((self.name(name), 0, self.raw_off[key], freq))
                self.stats["raw"] += 1
            else:
                self.stats["skipped"] += 1
        return sigs


def build_tree(packer, root):
    """Return a nested tree: (name, is_device, children_or_signals)."""
    def walk(path):
        entries = []
        for e in os.scandir(path):
            if e.name.startswith(".") or e.name in SKIP_DIRS:
                continue
            if e.is_dir():
                sub = walk(e.path)
                if sub:
                    entries.append((e.name, False, sub))
            elif e.name.lower().endswith(".ir") and e.name != "Power_off.ir":
                sigs = packer.device_signals(e.path)
                if sigs:
                    entries.append((e.name[:-3], True, sigs))
        entries.sort(key=lambda x: (x[1], x[0].lower()))   # folders first
        return entries
    return ("IR database", False, walk(root))


def serialize(packer, tree):
    nodes = []   # [name_off, first, count, flags]

    # Breadth-first so each node's children are contiguous.
    nodes.append([packer.string(tree[0]), 0, 0, 0])
    queue = [(0, tree)]
    while queue:
        idx, (_, is_dev, payload) = queue.pop(0)
        if is_dev:
            nodes[idx][1] = len(packer.signals)
            nodes[idx][2] = len(payload)
            nodes[idx][3] = NODE_DEVICE
            packer.signals.extend(payload)
            continue
        nodes[idx][1] = len(nodes)
        nodes[idx][2] = len(payload)
        for child in payload:
            nodes.append([packer.string(child[0]), 0, 0, 0])
            queue.append((len(nodes) - 1, child))

    hdr_size = 40
    off_nodes = hdr_size
    off_signals = off_nodes + 12 * len(nodes)
    off_names = off_signals + 11 * len(packer.signals)
    off_strings = off_names + 4 * len(packer.names)
    off_raw = off_strings + len(packer.strings)

    out = bytearray(b"IRDB")
    out += struct.pack("<9I", 1, len(nodes), len(packer.signals),
                       len(packer.names), off_nodes, off_signals, off_names,
                       off_strings, off_raw)
    for n in nodes:
        out += struct.pack("<IIHH", *n)
    for s in packer.signals:
        out += struct.pack("<HBII", *s)
    for n in packer.names:
        out += struct.pack("<I", n)
    out += packer.strings
    out += packer.raw
    return bytes(out), len(nodes)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    packer = Packer()
    tree = build_tree(packer, src)
    blob, n_nodes = serialize(packer, tree)
    assert len(packer.names) < 0x10000, "name_idx is u16"
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(blob)
    print(f"{dst}: {len(blob) / 1e6:.2f} MB, {n_nodes} nodes, "
          f"{len(packer.signals)} signals ({packer.stats['parsed']} parsed, "
          f"{packer.stats['raw']} raw, {packer.stats['skipped']} skipped), "
          f"{len(packer.names)} names, raw pool {len(packer.raw) / 1e3:.0f} KB")


if __name__ == "__main__":
    main()

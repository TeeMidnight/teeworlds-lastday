#!/usr/bin/env python3
"""
view_map_json.py - View the embedded JSON data of a Teeworlds .map file.

Background
----------
Mapcreator (src/engine/map/mapcreator.cpp) writes arbitrary JSON blobs into a
map via CMapCreator::AddJsonData(). Every call stores one JSON string as a
compressed data chunk and references it from a MAPITEMTYPE_JSON item
(src/game/mapitems.h):

    MAPITEMTYPE_JSON = 16
    struct CMapItemJson { int m_Version; int m_Data; }   // m_Data -> data chunk index

For example mapgen.cpp does:
    Creator.AddJsonData(world_json, size, 0);   // id 0: world / entrance data
    Creator.AddJsonData(some_json, size, 1);    // id 1: resource data

A .map file is a "DATA" datafile (version 4). Its layout is:

    [4 bytes magic "DATA"]
    [8 int32  : version, size, swaplen, num_item_types,
                num_items, num_raw_data, item_size, data_size]
    [num_item_types * (3 int32) : type, start_index, count]
    [num_items int32    : item data offsets (relative to item block)]
    [num_raw_data int32 : data chunk offsets (relative to data block)]
    [num_raw_data int32 : data chunk UNCOMPRESSED sizes      ]  (v4 only)
    [item block   (item_size  bytes)  : each item = {int32 type_and_id,
                                                     int32 size, payload}]
    [data block   (data_size  bytes)  : zlib-compressed chunks]

    item type   = (type_and_id >> 16) & 0xffff
    item id     =   type_and_id        & 0xffff

Run:
    python3 view_map_json.py <mapfile>                 # list all embedded JSON ids
    python3 view_map_json.py <mapfile> [id] [--raw]    # dump JSON with that id
"""

import argparse
import json
import struct
import zlib
import sys

MAPITEMTYPE_JSON = 16


class MapFile:
    """Minimal reader for a Teeworlds version-4 'DATA' datafile."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.raw = f.read()

        magic = self.raw[0:4]
        if magic not in (b'DATA', b'ATAD'):  # ATAD = big-endian variant
            raise ValueError(f"not a teeworlds datafile (magic={magic!r})")

        (self.version, size, swaplen, num_item_types,
         self.num_items, self.num_raw_data, self.item_size,
         self.data_size) = struct.unpack_from('<8i', self.raw, 4)

        if self.version not in (3, 4):
            raise ValueError(f"unsupported datafile version {self.version}")

        off = 4 + 8 * 4

        # item type table: (type, start_item_idx, count)
        self.item_types = []
        for _ in range(num_item_types):
            t, start, n = struct.unpack_from('<3i', self.raw, off)
            self.item_types.append((t, start, n))
            off += 12

        self.item_offsets = struct.unpack_from(f'<{self.num_items}i', self.raw, off)
        off += 4 * self.num_items

        self.data_offsets = struct.unpack_from(f'<{self.num_raw_data}i', self.raw, off)
        off += 4 * self.num_raw_data

        # v4 stores uncompressed sizes for every data chunk
        self.data_uncompressed_sizes = None
        if self.version >= 4:
            self.data_uncompressed_sizes = struct.unpack_from(f'<{self.num_raw_data}i', self.raw, off)
            off += 4 * self.num_raw_data

        self.item_start = off
        self.data_start = self.item_start + self.item_size

    def item_for(self, index):
        """Return (item_type, item_id, payload_bytes) for the item at `index`."""
        off = self.item_start + self.item_offsets[index]
        type_and_id, size = struct.unpack_from('<2i', self.raw, off)
        payload = self.raw[off + 8: off + 8 + size]
        return (type_and_id >> 16) & 0xffff, type_and_id & 0xffff, payload

    def items_of_type(self, wanted_type):
        """Yield (item_id, payload_bytes) for every item of the given type."""
        for t, start, n in self.item_types:
            if t == wanted_type:
                for i in range(start, start + n):
                    itype, iid, payload = self.item_for(i)
                    yield iid, payload

    def get_data(self, index):
        """Return the fully decompressed contents of data chunk `index`."""
        if index < 0 or index >= self.num_raw_data:
            raise IndexError(f"data chunk {index} out of range (0..{self.num_raw_data - 1})")

        start = self.data_start + self.data_offsets[index]
        if index + 1 < len(self.data_offsets):
            end = self.data_start + self.data_offsets[index + 1]
        else:
            end = len(self.raw)

        comp = self.raw[start:end]
        if self.version >= 4:
            size = self.data_uncompressed_sizes[index]
            return zlib.decompress(comp, 15, size)
        return comp

    def json_chunk(self, payload):
        """Return the decompressed raw bytes of a CMapItemJson item's data chunk."""
        # CMapItemJson { int m_Version; int m_Data; } -> m_Data indexes a data chunk
        version, data_idx = struct.unpack_from('<2i', payload, 0)
        return self.get_data(data_idx)

    def find_json(self, json_id):
        """Return the parsed (dict/list) object for a MAPITEMTYPE_JSON id, or None."""
        for iid, payload in self.items_of_type(MAPITEMTYPE_JSON):
            if iid == json_id:
                return decode_json_chunk(self.json_chunk(payload))
        return None

    def all_json(self):
        """Return {json_id: parsed_object} for every embedded JSON item."""
        result = {}
        for iid, payload in self.items_of_type(MAPITEMTYPE_JSON):
            result[iid] = decode_json_chunk(self.json_chunk(payload))
        return result


def decode_json_chunk(raw):
    """
    Decode a MAPITEMTYPE_JSON data chunk into a Python object.

    mapcreator stores the JSON string as a data chunk, often with a trailing
    NUL byte (C-style string) and optional whitespace/newline. Strip that
    padding before calling json.loads, since Python rejects it as
    "Extra data".
    """
    # remove trailing NUL bytes and trailing whitespace
    trimmed = raw.rstrip(b'\x00 \t\r\n')
    try:
        return json.loads(trimmed.decode('utf-8', 'replace'))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        return f"<invalid JSON: {e}; raw={raw!r}>"


def dump(obj, raw_mode, out):
    out.write(json.dumps(obj, ensure_ascii=False, indent=2) + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='View the JSON data embedded in a Teeworlds .map file.')
    parser.add_argument('mapfile', help='path to the .map file')
    parser.add_argument('json_id', nargs='?', type=int, default=None,
                        help='id of the JSON entry to print (default: list all)')
    parser.add_argument('--raw', action='store_true',
                        help='print the raw JSON string instead of pretty-printing')
    args = parser.parse_args(argv)

    m = MapFile(args.mapfile)

    if args.json_id is None:
        entries = m.all_json()
        print(f"Found {len(entries)} embedded JSON entrie(s) in {args.mapfile}:")
        for iid in sorted(entries):
            print(f"  id={iid}")
        return 0

    obj = m.find_json(args.json_id)
    if obj is None:
        print(f"No MAPITEMTYPE_JSON item with id {args.json_id} found in {args.mapfile}",
              file=sys.stderr)
        return 1
    dump(obj, args.raw, sys.stdout)
    return 0


if __name__ == '__main__':
    sys.exit(main())

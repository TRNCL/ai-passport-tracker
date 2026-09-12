#!/usr/bin/env python3
"""Pack tracker music files into a single binary image for the tracks partition."""

from __future__ import annotations

import json
import os
import struct
import sys
from pathlib import Path

MAGIC = 0x534B5254  # "TRKS"
VERSION = 1
MAX_PARTITION_SIZE = 0x4A0000  # 4,849,664 bytes (4.625 MB)

HEADER_STRUCT = struct.Struct("<IHHI I")  # magic, version, count, toc_size, total_size (16 bytes)
# Note: "<IHHI I" has a space between I and I? No, "<IHHII" is 16 bytes: 4 + 2 + 2 + 4 + 4 = 16 bytes.
HEADER_STRUCT = struct.Struct("<IHHII")
ENTRY_STRUCT = struct.Struct("<32s32s16sIIII")  # 32+32+16+4+4+4+4 = 96 bytes


def pack_tracks(tracks_dir: Path, output_file: Path, json_file: Path | None = None) -> None:
    if json_file is None or not json_file.is_file():
        json_file = tracks_dir / "tracks.json"

    if not json_file.is_file():
        raise FileNotFoundError(f"Missing track metadata file: {json_file}")

    with open(json_file, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    if not isinstance(manifest, list) or len(manifest) == 0:
        raise ValueError("Invalid manifest: must be a non-empty list of track objects")

    count = len(manifest)
    print(f"Packing {count} tracks from {tracks_dir} into {output_file}...")

    # Calculate TOC size and payload starting offset (aligned to 64 bytes)
    toc_size = HEADER_STRUCT.size + count * ENTRY_STRUCT.size
    first_payload_offset = (toc_size + 63) & ~63

    entries_data = bytearray()
    payload_data = bytearray()
    current_payload_offset = first_payload_offset

    for i, item in enumerate(manifest):
        fname = item["file"]
        title = item.get("title", Path(fname).stem)
        artist = item.get("artist", "Unknown")
        fmt = item.get("format", "MOD 4CH")

        track_path = tracks_dir / fname
        if not track_path.is_file():
            raise FileNotFoundError(f"Track file not found: {track_path}")

        raw_bytes = track_path.read_bytes()
        file_size = len(raw_bytes)

        # Align payload offset to 64 bytes
        pad_len = (64 - (len(payload_data) % 64)) % 64
        payload_data.extend(b"\x00" * pad_len)
        data_offset = first_payload_offset + len(payload_data)

        # Build entry
        title_b = title.encode("utf-8")[:31].ljust(32, b"\x00")
        artist_b = artist.encode("utf-8")[:31].ljust(32, b"\x00")
        fmt_b = fmt.encode("utf-8")[:15].ljust(16, b"\x00")

        entry_bytes = ENTRY_STRUCT.pack(
            title_b, artist_b, fmt_b, data_offset, file_size, 0, 0
        )
        entries_data.extend(entry_bytes)

        payload_data.extend(raw_bytes)
        print(f"  [{i}] {title} by {artist} ({fmt}): {file_size} bytes @ 0x{data_offset:x}")

    total_size = first_payload_offset + len(payload_data)
    if total_size > MAX_PARTITION_SIZE:
        raise ValueError(
            f"Packed tracks image size ({total_size} bytes) exceeds maximum partition limit ({MAX_PARTITION_SIZE} bytes)!"
        )

    # Build full header
    header_bytes = HEADER_STRUCT.pack(MAGIC, VERSION, count, toc_size, total_size)

    # Pad between TOC and first payload
    toc_padding = b"\x00" * (first_payload_offset - (len(header_bytes) + len(entries_data)))

    full_image = header_bytes + entries_data + toc_padding + payload_data

    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_bytes(full_image)

    print(f"\nSuccessfully generated {output_file}:")
    print(f"  Total size: {len(full_image)} bytes ({len(full_image)/1024:.1f} KB, {len(full_image)/1024/1024:.2f} MB)")
    print(f"  Partition capacity: {MAX_PARTITION_SIZE} bytes ({MAX_PARTITION_SIZE/1024/1024:.3f} MB)")
    print(f"  Free space remaining: {MAX_PARTITION_SIZE - len(full_image)} bytes ({(MAX_PARTITION_SIZE - len(full_image))/1024/1024:.2f} MB)")


def main() -> int:
    script_dir = Path(__file__).parent.resolve()
    project_dir = script_dir.parent

    tracks_dir = project_dir / "main" / "tracks"
    output_file = project_dir / "build" / "tracks.bin"

    if len(sys.argv) > 1:
        output_file = Path(sys.argv[1]).resolve()

    try:
        pack_tracks(tracks_dir, output_file)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

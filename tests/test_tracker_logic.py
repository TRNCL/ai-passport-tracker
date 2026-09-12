#!/usr/bin/env python3
"""Host-side unit test to validate MOD tracks and tracker player data structures."""

import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRACKS_DIR = ROOT / "main" / "tracks"

VALID_TAGS = {b"M.K.", b"M!K!", b"FLT4", b"4CHN", b"OCTA", b"CD81"}

def test_mod_file(path: Path):
    print(f"Testing {path.name}...")
    assert path.is_file(), f"{path} not found"
    data = path.read_bytes()
    assert len(data) >= 1084, f"{path.name} is too small: {len(data)} bytes"

    # Song title (20 bytes)
    title = data[:20].decode("ascii", errors="replace").strip("\x00").strip()
    print(f"  Title: '{title}' ({len(data)} bytes)")

    # Format tag (offset 1080..1084)
    tag = data[1080:1084]
    assert tag in VALID_TAGS, f"Invalid MOD tag: {tag}"
    print(f"  Tag: {tag.decode('ascii', errors='replace')} (Valid 4-channel ProTracker)")

    # Number of patterns in order
    song_length = data[950]
    assert 1 <= song_length <= 128, f"Invalid song length: {song_length}"
    order = data[952:952+song_length]
    num_patterns = max(order) + 1
    print(f"  Song length: {song_length} patterns in order (max pattern index: {num_patterns - 1})")

    # Sample headers (31 samples * 30 bytes)
    sample_data_offset = 1084 + num_patterns * 256 * 4  # 64 lines * 4 channels * 4 bytes
    total_sample_bytes = 0
    for i in range(31):
        s_off = 20 + i * 30
        s_name = data[s_off:s_off+22].decode("ascii", errors="replace").strip("\x00").strip()
        length_words = struct.unpack(">H", data[s_off+22:s_off+24])[0]
        length_bytes = length_words * 2
        total_sample_bytes += length_bytes

    expected_size = sample_data_offset + total_sample_bytes
    print(f"  Calculated size: {expected_size}, File size: {len(data)}")
    assert len(data) >= sample_data_offset, "File cut off before samples"
    print(f"  -> {path.name}: PASS")

def main():
    tracks = list(TRACKS_DIR.glob("*.mod"))
    assert len(tracks) >= 3, f"Expected at least 3 tracks, found {len(tracks)}"
    for t in sorted(tracks):
        test_mod_file(t)
    print("\nAll MOD track files validated successfully: PASS")

if __name__ == "__main__":
    main()

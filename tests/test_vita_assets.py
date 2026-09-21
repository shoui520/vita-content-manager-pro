import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_vita_icon_is_installable_indexed_png():
    data = (ROOT / "src/vita/sce_sys/icon0.png").read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    assert data[12:16] == b"IHDR"
    width, height, depth, color_type = struct.unpack(">IIBB", data[16:26])
    assert (width, height) == (128, 128)
    assert depth == 8
    assert color_type == 3  # Indexed PNG, required by Vita package assets.
    assert len(data) <= 420 * 1024

#!/usr/bin/env python3
"""Generate 2x Lanczos+sharpened *_hires.tga UI textures into hires/ from read-only gamedata/."""
import struct
from pathlib import Path
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
DAT_MAP = ROOT / "analysis" / "reports" / "dat-map.tsv"
GAMEDATA = ROOT / "gamedata"
HIRES_DIR = ROOT / "hires"

SKIP_NAMES = {
    "blur_map.tga",
    "heat_normal.tga",
    "noise_filter.tga",
    "rain_normal.tga",
    "sphere_normal.tga",
    "Hud.tga",
}


def make_hires_tga(src_bytes: bytes, out_path: Path) -> tuple[int, int, int, int] | None:
    if len(src_bytes) < 18 or src_bytes[16] != 32:
        return None
    w, h = struct.unpack_from("<HH", src_bytes, 12)
    pix_bytes = w * h * 4
    if w == 0 or h == 0 or len(src_bytes) < 18 + pix_bytes:
        return None

    # Already >= 2048? Skip upscaling beyond GLES2 2048 texture limit.
    if w >= 2048 or h >= 2048:
        return None

    nw, nh = w * 2, h * 2
    img = Image.frombytes("RGBA", (w, h), src_bytes[18 : 18 + pix_bytes])

    # Upscale RGB and Alpha cleanly, with gentle crispness on RGB only so UI text/icons sharpen cleanly.
    r, g, b, a = img.split()
    rgb = Image.merge("RGB", (r, g, b)).resize((nw, nh), Image.Resampling.LANCZOS)
    rgb = rgb.filter(ImageFilter.UnsharpMask(radius=0.8, percent=65, threshold=2))
    a_up = a.resize((nw, nh), Image.Resampling.LANCZOS)
    r2, g2, b2 = rgb.split()
    up = Image.merge("RGBA", (r2, g2, b2, a_up))

    # Standard uncompressed 32-bit TGA (id_len=0, cmap_type=0, img_type=2) required by CImageLoaderTGA.
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, nw, nh, 32, src_bytes[17])
    out_path.write_bytes(hdr + up.tobytes())
    return w, h, nw, nh


def main() -> int:
    HIRES_DIR.mkdir(parents=True, exist_ok=True)
    lines = DAT_MAP.read_text(encoding="utf-8", errors="replace").splitlines()[1:]
    by_name = {}
    for line in lines:
        dat, name = line.split("\t", 1)
        by_name[name] = dat

    existing_lower = {n.lower() for n in by_name}
    generated = 0
    for name, dat in sorted(by_name.items()):
        if "/" in name or not name.lower().endswith(".tga"):
            continue
        low = name.lower()
        if name in SKIP_NAMES or low.endswith("_hires.tga") or low.endswith("_ipad.tga") or "_ipad" in low:
            continue
        hires_name = name[:-4] + "_hires.tga"
        if hires_name.lower() in existing_lower:
            continue
        src_path = GAMEDATA / dat
        if not src_path.exists():
            continue
        res = make_hires_tga(src_path.read_bytes(), HIRES_DIR / hires_name)
        if res:
            w, h, nw, nh = res
            generated += 1
            print(f"generated hires/{hires_name:32s} ({w}x{h} -> {nw}x{nh})")

    print(f"Total generated in hires/: {generated}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

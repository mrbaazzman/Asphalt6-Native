#!/usr/bin/env python3
"""Build build/guest.bin and resolve the Android libc/OpenSL data symbols."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recomp"))
from elf_image import Image  # noqa: E402

# Guest data cells populated by runtime/guest_data.c above image_end (0x11658c8)
SYMBOLS = {
    "_ctype_": 0x1200000,
    "_tolower_tab_": 0x1200200,
    "_toupper_tab_": 0x1200480,
    "SL_IID_ENGINE": 0x1201000,
    "SL_IID_PLAY": 0x1201004,
    "SL_IID_BUFFERQUEUE": 0x1201008,
    "__sF": 0x1202000,
    "__stack_chk_guard": 0x1202200,
    "__dso_handle": 0x1202204,
}


def main() -> int:
    img = Image(ROOT / "analysis/apk_extract/lib/armeabi-v7a/libasphalt6.so")
    img.apply_relocations()
    patched = 0
    for off, name, _rtype in getattr(img, "undefined_relocs", []):
        if name in SYMBOLS:
            struct.pack_into("<I", img.mem, off, SYMBOLS[name])
            patched += 1
    out = ROOT / "build/guest.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(img.mem))
    print(f"patched {patched} data symbol slot(s); wrote {out} ({len(img.mem):,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

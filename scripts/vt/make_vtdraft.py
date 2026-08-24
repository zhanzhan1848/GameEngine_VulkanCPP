#!/usr/bin/env python3
"""
make_vtdraft.py — VT M0.9 原型离线切 tile 工具（草稿版 .vtdraft）

把一张 PNG 切成 VT 原型数据布局：tile 128 + border 4（clamp 扩展），
CPU 生成 mip 链（box filter，最低到 128），全部 tile 打进单文件。

M1 的正式 VTBaker（VTEX 容器 + BCn/ASTC 双格式 + 页索引）以此为基础
重写进 ContentTools；本脚本仅验证数据布局与离线流程。

用法:
    python3 make_vtdraft.py <input.png> [output.vtdraft] [--tile 128] [--border 4]

输出:
    <output.vtdraft>  二进制: header + 逐 mip 逐 tile 的 (tile+2*border)² RGBA
    stdout            页索引摘要（虚拟页 → 文件内偏移）
"""
import struct
import sys
from pathlib import Path

from PIL import Image

MAGIC = b"VTDP"
VERSION = 1


def gen_mips(img, tile):
    """box filter mip 链,直到边长 == tile(即 1 tile 覆盖)。"""
    mips = [img.convert("RGBA")]
    while min(mips[-1].size) > tile:
        w, h = mips[-1].size
        small = mips[-1].resize((w // 2, h // 2), Image.BOX)
        mips.append(small)
    return mips


def extract_tile(mip, px, py, tile, border):
    """取 (px,py) tile 并做 clamp 扩展 border。M0 用 tile 内 clamp;
    M3 过滤完善时改为邻 tile 边缘扩展。"""
    w, h = mip.size
    x0, y0 = px * tile, py * tile
    out = Image.new("RGBA", (tile + 2 * border, tile + 2 * border))
    core = mip.crop((x0, y0, x0 + tile, y0 + tile))
    out.paste(core, (border, border))
    # 四边 clamp
    left = core.crop((0, 0, 1, tile))
    right = core.crop((tile - 1, 0, tile, tile))
    for by in range(border):
        out.paste(left, (by, border))
        out.paste(right, (tile + border + by, border))
        out.paste(out.crop((0, border, tile + 2 * border, border + 1)), (0, by))
        out.paste(out.crop((0, tile + border - 1, tile + 2 * border, tile + border)), (0, tile + border + by))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".vtdraft")
    tile, border = 128, 4

    img = Image.open(src)
    w, h = img.size
    assert w == h and w & (w - 1) == 0, "M0 草稿要求 2^n 正方形"
    assert w >= tile

    mips = gen_mips(img, tile)
    with open(dst, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIII", VERSION, w, h, tile))
        f.write(struct.pack("<II", border, len(mips)))
        offset = 0
        for level, mip in enumerate(mips):
            side = mip.size[0] // tile
            for py in range(side):
                for px in range(side):
                    data = extract_tile(mip, px, py, tile, border).tobytes()
                    print(f"mip{level} vPage({px},{py}) tiles={side}x{side} "
                          f"offset={offset} bytes={len(data)}")
                    offset += len(data)
                    f.write(data)
    print(f"\n{dst}: {w}x{h} tile={tile} border={border} mips={len(mips)} "
          f"total={dst.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

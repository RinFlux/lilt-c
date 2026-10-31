#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""render.py — 把主机录下的原始帧变成能用的图片（对应 lilt 的 cmd/preview 出图那一半）

用法：
    python host/render.py                 # 用默认路径，出 GIF + 概览图
    python host/render.py --every 6       # 概览图每 6 帧取一张
    python host/render.py --png           # 顺便把每一帧单独存成 PNG

帧文件格式（和 host/host_bus.c 写出来的一致）：
    一帧 = LL_SCREEN_W * LL_SCREEN_PAGES 字节，页主序：buf[page*W + x]，
    一个字节竖着管 8 个像素，低位在最上面——这正是 SSD1306 显存的格式。
所以这里按同样的方式解回 128x64 的点阵，板子上看到的和这里看到的必然一致。
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw

# Git Bash 的终端收 UTF-8，而 Windows 上 Python 默认按控制台代码页（GBK）写，
# 中文出去就是一堆乱码。掰回 UTF-8。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

W, H = 128, 64
PAGES = H // 8
FRAME_BYTES = W * PAGES          # 1024


def decode(raw: bytes) -> Image.Image:
    """一帧 1024 字节 -> 1-bit 图像（亮 = 白）。"""
    img = Image.new("1", (W, H), 0)
    px = img.load()
    for page in range(PAGES):
        base = page * W
        for x in range(W):
            b = raw[base + x]
            for bit in range(8):
                if b & (1 << bit):
                    px[x, page * 8 + bit] = 1
    return img


def scale(img: Image.Image, k: int) -> Image.Image:
    if k == 1:
        return img.copy()
    return img.resize((W * k, H * k), Image.NEAREST)


def main() -> int:
    ap = argparse.ArgumentParser(description="render lilt-c host frames")
    ap.add_argument("--src", default="out/frames.raw")
    ap.add_argument("--gif", default="out/spring-lab.gif")
    ap.add_argument("--contact", default="out/contact.png")
    ap.add_argument("--scale", type=int, default=3, help="GIF 放大倍数（默认 3）")
    ap.add_argument("--fps", type=int, default=30, help="GIF 帧率，要和录帧的 DT 一致")
    ap.add_argument("--every", type=int, default=10, help="概览图每隔几帧取一张")
    ap.add_argument("--cscale", type=int, default=2, help="概览图每格的放大倍数（默认 2）")
    ap.add_argument("--png", action="store_true", help="额外导出每帧 PNG")
    ap.add_argument("--pad", type=int, default=6, help="概览图每格留白")
    args = ap.parse_args()

    if not os.path.exists(args.src):
        print(f"找不到 {args.src}（先编译并运行 out/lab）", file=sys.stderr)
        return 1

    data = open(args.src, "rb").read()
    n = len(data) // FRAME_BYTES
    if n == 0:
        print(f"{args.src} 是空的", file=sys.stderr)
        return 1
    if len(data) % FRAME_BYTES:
        print(f"警告：{len(data)} 字节不是 {FRAME_BYTES} 的整数倍，末尾多出 "
              f"{len(data) % FRAME_BYTES} 字节已忽略", file=sys.stderr)
    print(f"{args.src}: {n} 帧")

    frames = [decode(data[i * FRAME_BYTES:(i + 1) * FRAME_BYTES]) for i in range(n)]

    # ---- 逐帧 PNG（可选，方便在图片浏览器里翻） ----
    if args.png:
        d = os.path.join(os.path.dirname(args.gif) or ".", "frames")
        os.makedirs(d, exist_ok=True)
        for i, f in enumerate(frames):
            f.save(os.path.join(d, f"f{i:04d}.png"))
        print(f"  PNG   -> {d}/f0000.png .. f{n-1:04d}.png")

    # ---- 动画 GIF ----
    os.makedirs(os.path.dirname(args.gif) or ".", exist_ok=True)
    big = [scale(f, args.scale) for f in frames]
    big[0].save(args.gif, save_all=True, append_images=big[1:],
                duration=int(round(1000 / args.fps)), loop=0, disposal=2,
                optimize=False)
    print(f"  GIF   -> {args.gif}  ({W*args.scale}x{H*args.scale}, {n} 帧 @ {args.fps}fps)")

    # ---- 概览图：等间隔抽样 + 帧号/毫秒标注，一眼看出弹簧什么时候在动 ----
    idxs = list(range(0, n, max(1, args.every)))
    if idxs[-1] != n - 1:
        idxs.append(n - 1)
    cols = 6
    rows = (len(idxs) + cols - 1) // cols
    sw, sh = W * args.cscale, H * args.cscale
    cw, ch = sw + args.pad * 2, sh + args.pad * 2 + 11
    sheet = Image.new("1", (cols * cw, rows * ch), 0)
    dr = ImageDraw.Draw(sheet)
    for k, i in enumerate(idxs):
        cx = (k % cols) * cw
        cy = (k // cols) * ch
        dr.rectangle([cx, cy, cx + cw - 1, cy + ch - 1], outline=1)
        sheet.paste(scale(frames[i], args.cscale), (cx + args.pad, cy + args.pad + 10))
        ms = i * 1000 // args.fps
        dr.text((cx + args.pad, cy + args.pad), f"#{i:03d}  {ms}ms", fill=1)
    sheet.save(args.contact)
    print(f"  概览  -> {args.contact}  ({len(idxs)} 帧，每 {args.every} 帧取一张)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

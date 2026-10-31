#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_panel.py — 屏幕方向那道闸门

ll_board.cpp 里有两处能让画面转 180 度的东西：

  1. 面板命令 0xA1（段重映射）+ 0xC8（COM 反向扫描），在芯片里翻；
  2. ll_bus_push_page() 里 #if LL_ROTATE_180 那段软件翻转，页序、字节序、位序一起倒。

这两处方向相同，叠在一起等于没转，只能留一层。脚本先读 ll_board.cpp 判断当前
哪几层在生效，再按 SSD1306 / SH1106 的显存到物理像素映射把画面走一遍，
看 LL_ROTATE_180=1 时到底转没转。

    python host/check_panel.py                        # 不转就退 1
    python host/check_panel.py 别处的/ll_board.cpp     # 查另一份副本

装进 host/build.sh 之后，任何人再往翻转路径上加一层都会当场被抓到。
"""

import os
import re
import sys

# Git Bash 的终端收 UTF-8，而 Windows 上 Python 默认按控制台代码页（GBK）写，
# 中文出去就是一堆乱码。这里把 stdout 掰回 UTF-8，和 host/build.sh 里 C 程序的输出对齐。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

W, PAGES = 128, 8
BOARD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "ll_board.cpp")


def rev8(b):
    """和 ll_board.cpp 里那张 NIBBLE_REV 表等价，位序整体反转。"""
    return int(format(b, "08b")[::-1], 2)


def read_flags(path=BOARD):
    """从 ll_board.cpp 里读出"翻转路径上有哪几层在生效"。

    软件那层要看 ll_bus_push_page() 的函数体，不能只搜 rev8 有没有定义：
    把推页里的翻转删掉、留着那个没人用的辅助函数，画面一样不会转。
    """
    src = open(path, encoding="utf-8").read()

    m = re.search(r"ll_bus_push_page\s*\([^)]*\)\s*\{([\s\S]*?)\n\}", src)
    body = m.group(1) if m else ""
    sw = ("LL_ROTATE_180" in body) and ("rev8" in body)

    # 硬件翻转：段重映射和 COM 扫描有没有跟着 LL_ROTATE_180 变。
    seg = bool(re.search(r"0xA0\s*\+\s*\(\s*LL_ROTATE_180", src))
    com = bool(re.search(r"0xC0\s*\+\s*\(\s*LL_ROTATE_180", src))
    return sw, seg, com


def push(fb, sw_rotate):
    """帧缓冲 -> 显存。抄 ll_bus_push_page 的写法。"""
    ram = [[0] * W for _ in range(PAGES)]
    for p in range(PAGES):
        row = fb[p]
        if sw_rotate:
            data = [rev8(row[W - 1 - i]) for i in range(W)]
            page = PAGES - 1 - p
        else:
            data, page = row, p
        ram[page] = list(data)
    return ram


def lit_pixels(ram, chip, seg_remap, com_reverse):
    """显存 -> 物理像素。

    SSD1306 显存 128 列，SEG 号就等于列地址；SH1106 显存 132 列，
    画面占 SEG 2..129，写的时候从列地址 2 起。
    0xA1 / 0xC8 把地址到 SEG、COM 的映射各自反过来，两个一起用就是转 180 度。
    """
    out = set()
    ncols = 132 if chip == "sh1106" else 128
    col0 = 2 if chip == "sh1106" else 0
    off = 2 if chip == "sh1106" else 0

    for p in range(PAGES):
        for i in range(W):
            col = col0 + i
            byte = ram[p][i]
            for b in range(8):
                if not (byte >> b) & 1:
                    continue
                seg = (ncols - 1 - col) if seg_remap else col
                com = (63 - (p * 8 + b)) if com_reverse else (p * 8 + b)
                out.add((seg - off, com))
    return out


def where_do_they_land(chip, sw, seg, com, points):
    fb = [[0] * W for _ in range(PAGES)]
    for x, y in points:
        fb[y // 8][x] |= 1 << (y % 8)
    return sorted(lit_pixels(push(fb, sw), chip, seg, com))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else BOARD
    sw, seg, com = read_flags(path)
    print("检查 %s" % path)
    print("LL_ROTATE_180=1 时生效的层：")
    print("   软件翻转（页序/字节序/位序）  %s" % ("有" if sw else "无"))
    print("   0xA1 段重映射                %s" % ("有" if seg else "无"))
    print("   0xC8 COM 反向扫描            %s" % ("有" if com else "无"))
    print()

    points = [(0, 0), (10, 5), (127, 63), (64, 32)]
    want = sorted((127 - x, 63 - y) for x, y in points)
    print("点亮 %s" % (points,))
    print("转 180 度之后应该落在 %s" % (want,))
    print()

    bad = 0
    for chip in ("ssd1306", "sh1106"):
        got = where_do_they_land(chip, sw, seg, com, points)
        if got == want:
            verdict = "PASS 转了 180 度"
        elif got == sorted(points):
            verdict = "FAIL 位置一点没变，等于没转"
            bad = 1
        else:
            verdict = "FAIL 落点和两种预期都不符，需要人工看一眼"
            bad = 1
        print("   %-8s -> %s   %s" % (chip, got, verdict))

    print()
    if bad:
        print("FAIL LL_ROTATE_180 没把画面转过来。")
        print("     软件翻转和 0xA1/0xC8 是同方向的两次 180 度，叠起来回到原点。")
        print("     留一层就行，建议删掉 ll_bus_push_page 里 #if LL_ROTATE_180 那段软件翻转，")
        print("     让芯片自己翻，CPU 和栈都省下来。")
        return 1
    print("PASS LL_ROTATE_180 在两种面板上都转了 180 度。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

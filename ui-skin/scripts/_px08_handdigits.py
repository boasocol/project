# -*- coding: utf-8 -*-
"""手绘 10x14 数字点阵 + 按屏配色合成精灵；与设计稿同位置对比"""
import os
from PIL import Image
import numpy as np
import ui_paths

DIG = ui_paths.DIG
os.makedirs(DIG, exist_ok=True)

ART = {
"0": """
..######..
.########.
##########
###....###
###....###
###....###
###....###
###....###
###....###
##########
.########.
..######..
..........
..........""",
"1": """
....###...
...####...
.#####....
#######...
...###....
...###....
...###....
...###....
...###....
...###....
...###....
#########.
#########.
..........""",
"2": """
..######..
.########.
###....###
.......###
......###.
.....###..
....###...
...###....
..###.....
.###......
###.......
##########
##########
..........""",
"3": """
..######..
.########.
###....###
.......###
.......###
.....####.
....####..
.......###
.......###
.......###
###....###
.########.
..######..
..........""",
"4": """
......###.
.....####.
....#####.
...###.###
..###..###
.###...###
###....###
##########
##########
.......###
.......###
.......###
.......###
..........""",
"5": """
##########
##########
###.......
###.......
###.......
########..
.########.
.......###
.......###
.......###
###....###
.########.
..######..
..........""",
"6": """
..######..
.########.
###....###
###.......
###.......
###.......
########..
.########.
###....###
###....###
###....###
.########.
..######..
..........""",
"7": """
##########
##########
......###.
......###.
.....###..
.....###..
....###...
....###...
...###....
...###....
..###.....
..###.....
..###.....
..........""",
"8": """
..######..
.########.
###....###
###....###
###....###
.########.
.########.
###....###
###....###
###....###
###....###
.########.
..######..
..........""",
"9": """
..######..
.########.
###....###
###....###
###....###
.########.
..#######.
.......###
.......###
.......###
###....###
.########.
..######..
..........""",
":": """
....
.##.
.##.
.##.
....
....
....
....
.##.
.##.
.##.
....
....
....""",
}

C = {
  "main": dict(fill=(253, 219, 75), navy=(16, 24, 62), cream=(255, 252, 226),
               org=(240, 120, 40), cell=4),
  "flat": dict(fill=(254, 245, 5), navy=(2, 16, 62), cream=None,
               org=None, cell=3),
}

def grid(ch):
    rows = [r for r in ART[ch].strip("\n").split("\n")]
    w = max(len(r) for r in rows)
    g = np.zeros((len(rows), w), bool)
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c == "#":
                g[y, x] = True
    return g

def dilate(m):
    out = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out |= np.roll(np.roll(m, dy, axis=0), dx, axis=1)
    return out

def compose(ch, style):
    cfg = C[style]
    g = grid(ch)
    h, w = g.shape
    pad = 1
    M = np.zeros((h + 2 * pad + 1, w + 2 * pad + 1), bool)
    M[pad:pad + h, pad:pad + w] = g
    img = np.zeros(M.shape + (4,), np.uint8)
    if style == "main":
        halo = dilate(M)
        ext = np.zeros_like(halo); ext[1:, 1:] = halo[:-1, :-1]
        img[ext & ~halo] = (*cfg["cream"], 255)
        img[halo] = (*cfg["navy"], 255)
        img[M] = (*cfg["fill"], 255)
        acc = M & (~np.roll(M, 1, axis=0) | ~np.roll(M, 1, axis=1))
        img[acc] = (*cfg["org"], 255)
    else:
        halo = dilate(M)
        img[halo] = (*cfg["navy"], 255)
        img[M] = (*cfg["fill"], 255)
    im = Image.fromarray(img, "RGBA")
    return im.resize((im.width * cfg["cell"], im.height * cfg["cell"]), Image.NEAREST)

sets = {s: {ch: compose(ch, s) for ch in ART} for s in C}
for s in sets:
    for ch, im in sets[s].items():
        im.save(os.path.join(DIG, f"{s}_{'col' if ch==':' else ch}.png"))
print("main digit:", sets["main"]["0"].size, "flat:", sets["flat"]["0"].size)

# 对比：设计稿 20:32 vs 合成（没有设计稿目录时跳过，不影响精灵产出）
try:
    SRC = ui_paths.SRC
    mock = Image.open(os.path.join(SRC, "时间界面（全显）.png")).convert("RGB")
    blank = Image.open(os.path.join(SRC, "时间界面.png")).convert("RGB")

    def draw_time(base, s, text, x0, y0, step, col_gap, colw):
        im = base.copy()
        x = x0
        for i, ch in enumerate(text):
            if ch == ":":
                sp = sets[s]["col"]
                im.paste(sp, (x + col_gap, y0), sp)
                x += 0
            else:
                sp = sets[s][ch]
                im.paste(sp, (x, y0 - 4), sp)
                x += step
        return im

    # 用设计稿位置：2=43 0=85 :=130 3=150 2=190
    test = draw_time(blank, "main", "2032", 43, 27, 42, 0, 0)
    crop_m = mock.crop((30, 15, 240, 90)).resize((210*3, 75*3), Image.NEAREST)
    crop_t = test.crop((30, 15, 240, 90)).resize((210*3, 75*3), Image.NEAREST)
    cmp = Image.new("RGB", (crop_m.width, crop_m.height*2 + 6), (0,0,0))
    cmp.paste(crop_m, (0,0)); cmp.paste(crop_t, (0, crop_m.height + 6))
    cmp.save(os.path.join(DIG, "_px08_digit_vs.png"))
    print("saved digit_vs")
except Exception as e:
    print("跳过设计稿对比（%s）" % e)

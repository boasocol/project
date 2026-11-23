# -*- coding: utf-8 -*-
"""数字合成 v3：黄填充 + 藏青描边 + 整字右下米白阴影（不做橙线）"""
import os
from PIL import Image
import numpy as np
import importlib.util, sys
import ui_paths

DIG  = ui_paths.DIG
SRC  = ui_paths.SRC
HERE = os.path.dirname(os.path.abspath(__file__))

spec = importlib.util.spec_from_file_location("hd3", os.path.join(HERE, "_px08_handdigits.py"))
hd = importlib.util.module_from_spec(spec); sys.modules["hd3"] = hd
spec.loader.exec_module(hd)
ART = hd.ART

C = {
  "main": dict(fill=(253, 219, 75), navy=(16, 24, 62), shadow=(255, 250, 222), cell=3, shadow_on=True),
  "flat": dict(fill=(254, 245, 5), navy=(2, 16, 62), shadow=(6, 26, 78), cell=3, shadow_on=True),
}

def grid(ch):
    rows = ART[ch].strip("\n").split("\n")
    w = max(len(r) for r in rows)
    g = np.zeros((len(rows), w), bool)
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            g[y, x] = c == "#"
    return g

def dilate(m):
    out = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out |= np.roll(np.roll(m, dy, axis=0), dx, axis=1)
    return out

def compose(ch, style):
    cfg = C[style]; g = grid(ch); h, w = g.shape; pad = 1
    H, Wd = h + 2*pad + 1, w + 2*pad + 1
    M = np.zeros((H, Wd), bool); M[pad:pad+h, pad:pad+w] = g
    img = np.zeros((H, Wd, 4), np.uint8)
    halo = dilate(M)
    if cfg["shadow_on"]:
        sh = np.zeros_like(halo); sh[1:, 1:] = halo[:-1, :-1]
        img[sh & ~halo] = (*cfg["shadow"], 255)
    img[halo] = (*cfg["navy"], 255)
    img[M] = (*cfg["fill"], 255)
    im = Image.fromarray(img, "RGBA")
    return im.resize((im.width*cfg["cell"], im.height*cfg["cell"]), Image.NEAREST)

sets = {s: {ch: compose(ch, s) for ch in ART} for s in C}
for s in sets:
    for ch, im in sets[s].items():
        im.save(os.path.join(DIG, f"v3_{s}_{'col' if ch==':' else ch}.png"))
print("main", sets["main"]["0"].size, "flat", sets["flat"]["0"].size)

# 与设计稿对比（素材方机器上没有设计稿目录时，跳过这一步，别让整套流程挂掉）
try:
    ref = Image.open(os.path.join(SRC, "时间界面（全显）.png")).convert("RGB")
    blank = Image.open(os.path.join(SRC, "时间界面.png")).convert("RGB")
    test = blank.copy()
    for ch, x, y in [("2",39,24),("0",81,24),(":",126,24),("3",146,24),("2",186,24)]:
        sp = sets["main"][ch]; test.paste(sp, (x,y), sp)
    box = (30, 18, 240, 85); S = 3
    a = ref.crop(box).resize(((box[2]-box[0])*S,(box[3]-box[1])*S), Image.NEAREST)
    b = test.crop(box).resize(((box[2]-box[0])*S,(box[3]-box[1])*S), Image.NEAREST)
    c = Image.new("RGB",(a.width, a.height*2+6),(0,0,0)); c.paste(a,(0,0)); c.paste(b,(0,a.height+6))
    c.save(os.path.join(DIG, "_px08_digit_vs3.png"))
    print("saved 对比图 ->", DIG)
except Exception as e:
    print("跳过设计稿对比（%s）" % e)

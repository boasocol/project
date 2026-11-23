# -*- coding: utf-8 -*-
"""闹钟屏「平面风」大数字精灵：按设计稿实测墨迹尺寸生成（20x32 / 冒号 6x32）

设计稿实测（临时区/08_像素游戏机ui界面/闹钟界面（打开状态）.png）：
  H 十位 x49..68、H 个位 x73..92、冒号 x96..101、M 十位 x105..124、M 个位 x128..147
  墨迹行 y63..94（高 32）
  → 每字 20 宽，位距 24/23，冒号 6 宽

用 10x14 手绘点阵（_px08_handdigits.ART）缩放到 20x32，风格沿用 v3 flat：
黄填充 (254,245,5) + 藏青描边 (2,16,62) + 右下深影 (6,26,78)，描边/阴影为真实 1px。
输出 px_digits/alm_<0-9>.png、alm_col.png（含描边，整块 22x34 / 8x34）。
"""
import os
import sys
import importlib.util

import numpy as np
from PIL import Image
import ui_paths

DIG  = ui_paths.DIG
HERE = os.path.dirname(os.path.abspath(__file__))
ui_paths.ensure_dirs()

INK_W, INK_H = 20, 32      # 闹钟屏（alm）
COL_W = 6

# 倒计时屏（cnt）：设计稿实测每字 28 宽 x 42 高，冒号 7 宽
CNT_W, CNT_H = 28, 42
CNT_COL_W = 7

FILL = (254, 245, 5)
NAVY = (2, 16, 62)
SHAD = (6, 26, 78)

spec = importlib.util.spec_from_file_location(
    "hd", os.path.join(HERE, "_px08_handdigits.py"))
hd = importlib.util.module_from_spec(spec)
sys.modules["hd"] = hd
spec.loader.exec_module(hd)
ART = hd.ART


def grid(ch):
    rows = ART[ch].strip("\n").split("\n")
    w = max(len(r) for r in rows)
    g = np.zeros((len(rows), w), bool)
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            g[y, x] = c == "#"
    return g


def tight(m):
    ys = np.where(m.any(axis=1))[0]
    xs = np.where(m.any(axis=0))[0]
    return m[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]


def to_size(m, w, h):
    im = Image.fromarray((m * 255).astype(np.uint8), "L")
    im = im.resize((w, h), Image.NEAREST)
    return np.asarray(im) > 127


def dilate(m):
    out = m.copy()
    out[1:, :] |= m[:-1, :]
    out[:-1, :] |= m[1:, :]
    out[:, 1:] |= m[:, :-1]
    out[:, :-1] |= m[:, 1:]
    return out


def compose(m):
    """m: 墨迹掩码 → 加 1px 描边 + 右下 1px 阴影，返回 RGBA"""
    h, w = m.shape
    img = np.zeros((h + 2, w + 2, 4), np.uint8)
    M = np.zeros((h + 2, w + 2), bool)
    M[1:1 + h, 1:1 + w] = m
    halo = dilate(M)
    sh = np.zeros_like(halo)
    sh[1:, 1:] = halo[:-1, :-1]
    img[sh & ~halo] = (*SHAD, 255)
    img[halo] = (*NAVY, 255)
    img[M] = (*FILL, 255)
    return Image.fromarray(img, "RGBA")


def colon_mask(h=INK_H, w=COL_W):
    m = np.zeros((h, w), bool)
    m[6:13, :] = True
    m[20:26, :] = True
    return m


def colon_mask_cnt(h=CNT_H, w=CNT_COL_W):
    """倒计时冒号：设计稿实测两段分别 y58..65 / y72..80（相对 47 → 11..18 / 25..33）"""
    m = np.zeros((h, w), bool)
    m[11:19, :] = True
    m[25:34, :] = True
    return m


os.makedirs(DIG, exist_ok=True)
for ch in "0123456789":
    im = compose(to_size(tight(grid(ch)), INK_W, INK_H))
    im.save(os.path.join(DIG, f"alm_{ch}.png"))
    if ch == "0":
        print("alm_0 尺寸:", im.size)

im = compose(colon_mask())
im.save(os.path.join(DIG, "alm_col.png"))
print("alm_col 尺寸:", im.size)

for ch in "0123456789":
    im = compose(to_size(tight(grid(ch)), CNT_W, CNT_H))
    im.save(os.path.join(DIG, f"cnt_{ch}.png"))
im = compose(colon_mask_cnt())
im.save(os.path.join(DIG, "cnt_col.png"))
print("cnt_0 尺寸:", Image.open(os.path.join(DIG, "cnt_0.png")).size,
      " cnt_col 尺寸:", im.size)
print("完成：alm/cnt 各 10 数字 + 冒号 →", DIG)

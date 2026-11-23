# -*- coding: utf-8 -*-
"""像素游戏机 UI —— 字库打包（思源黑体 Bold，1bpp 变宽字库，兼容现有 FONT 容器格式）

字号档位（code = base + Unicode）：
  0x00000 12px 卡片文字
  0x10000 13px 气泡文字 / 模式标题
  0x20000 14px 按钮文字
  0x30000 26px 大标题（倒计时中 / 正计时中）
输出：datasheets/ui设计/px_flash/px_font.bin
"""
import os, struct, zlib
from PIL import Image, ImageDraw, ImageFont
import numpy as np
import ui_paths
from circus_skin import ALARM_TEXT, COUNTDOWN_TEXT

DST      = ui_paths.FL
FONT_DIR = ui_paths.FONT_DIR
ui_paths.ensure_dirs()
ui_paths.require_font()

SIZES = [(12, 0x00000), (13, 0x10000), (14, 0x20000), (26, 0x30000)]
# 每档字重（按字号灵活选）：小字号 Medium 更清晰，大标题 Bold 更有分量
WEIGHT = {12: "Medium", 13: "Medium", 14: "Medium", 26: "Bold"}

# ---- 需要覆盖的字符集（从三屏实际用到的字符串收集） ----
TEXTS = [
    COUNTDOWN_TEXT, ALARM_TEXT, "农历",
    # 主屏
    "0123456789-:",
    "星期一二三四五六日",
    # 农历（正月初一 ~ 腊月三十）
    "正冬腊月初始廿十甲乙丙丁",
    "一二三四五六七八九",
    # 温湿度
    "℃%°.",
    # 气泡 / 语音播报（V10 协议 MCU→CI 全部文案）
    "主人，有什么需要帮助的吗？",
    "好的，设置成功。 ",
    "设置失败，请重试",
    "我在",
    "计时开始",
    "时间到了",
    "我去休息了，有事再叫",
    "欢迎使用语音闹钟",
    "我快没电了，快给我充电吧！",
    "闹钟已结束",
    "现在是点整分",
    # 按钮 / 模式 / 界面文字
    "开始暂停重置完成退出",
    "倒计时中正",
    "闹钟开关",
    "分秒",
    "月日年",
    "第次",
    "共",
    "音量无线蓝牙连接已",
    "℃°",
]

chars = set()
for t in TEXTS:
    chars |= set(t)
chars = sorted(chars)
print("字符数:", len(chars), "".join(chars))

def glyph_bitmap(f, ch, thr=110):
    asc, desc = f.getmetrics()
    h = asc + desc
    w = max(1, int(round(f.getlength(ch))))
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=f, anchor="la", fill=255)
    a = np.asarray(img) > thr
    return a, w, h

def pack_vflip(a, w, h):
    """1bpp 行优先、MSB 在左；行序翻转（row0=设计底部，与固件绘制约定一致）"""
    row_bytes = (w + 7) // 8
    out = bytearray()
    for y in range(h - 1, -1, -1):
        bits = bytearray(row_bytes)
        for x in range(w):
            if a[y, x]:
                bits[x >> 3] |= (0x80 >> (x & 7))
        out += bits
    return bytes(out)

items = []
for size, base in SIZES:
    f = ImageFont.truetype(os.path.join(FONT_DIR, "SourceHanSansSC-%s.otf" % WEIGHT[size]), size)
    cache = {ch: glyph_bitmap(f, ch) for ch in chars}
    asc, desc = f.getmetrics()
    H = asc + 2                               # 统一基线，并留 2 行降部（否则「星」「成」这类字底横被裁掉）
    print("  %dpx 单元格高 H=%d (asc=%d desc=%d)" % (size, H, asc, desc))
    for ch in chars:
        a, w, h = cache[ch]
        cell = np.zeros((H, w), bool)
        blk = a[0:min(H, h)]                  # 从行 0 起裁相同高度，保基线一致
        cell[:blk.shape[0]] = blk
        items.append((base + ord(ch), ch, w, H, pack_vflip(cell, w, H)))

items.sort()
count = len(items)
index_offset = 256
entry = 12
bitmap_offset = index_offset + count * entry

hdr = struct.pack('<4sBBBBBHIIII', b'FONT', 1, 1, 1, 0, 0, 0, count, index_offset, bitmap_offset, 0)
crc = zlib.crc32(hdr[:23]) & 0xFFFFFFFF
hdr = hdr[:23] + struct.pack('<I', crc) + b'\x00' * (256 - 23 - 4)

idx = bytearray(); bmp = bytearray()
off = bitmap_offset
for code, ch, w, h, data in items:
    idx += struct.pack('<IIHH', code, off, w, h)
    bmp += data
    off += len(data)

blob = hdr + bytes(idx) + bytes(bmp)
open(os.path.join(DST, "px_font.bin"), "wb").write(blob)
print(f"px_font.bin  {count} glyphs  {len(blob)} B ({len(blob)/1024:.1f} KB)")

# ---- 预览：各档位渲染一串样本 ----
SAMPLES = [(12, "2026-11-03 星期二 23℃ 58%"),
           (13, "主人，有什么需要帮助的吗？"),
           (14, "开始 暂停 重置"),
           (26, "倒计时中")]
rows = []
for size, base in SIZES:
    line = []
    for ch in SAMPLES[[s for s, _ in SAMPLES].index(size)][1]:
        for code, c, w, h, data in items:
            if code == base + ord(ch):
                row_bytes = (w + 7)//8
                a = np.zeros((h, w), bool)
                for y in range(h):
                    for x in range(w):
                        a[h-1-y, x] = (data[y*row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1
                im = Image.new("RGB", (w, h), (20, 26, 70))
                arr = np.array(im); arr[a] = (250, 250, 255)
                line.append(Image.fromarray(arr))
                break
    if line:
        W = sum(i.width for i in line)
        H = max(i.height for i in line)
        strip = Image.new("RGB", (W, H), (20, 26, 70))
        x = 0
        for i in line:
            strip.paste(i, (x, 0)); x += i.width
        rows.append((f"{size}px", strip))

W = max(s.width for _, s in rows) + 80
H = sum(s.height for _, s in rows) + 8*len(rows)
c = Image.new("RGB", (W, H), (0, 0, 0))
d = ImageDraw.Draw(c)
y = 0
for name, s in rows:
    d.text((2, y + s.height//2 - 4), name, fill=(0, 255, 0))
    c.paste(s, (76, y)); y += s.height + 8
c = c.resize((c.width*2, c.height*2), Image.NEAREST)
c.save(os.path.join(DST, "_px08_font_preview.png"))
print("preview saved", c.size)



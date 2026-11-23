# -*- coding: utf-8 -*-
"""像素游戏机 UI —— 字库打包（1bpp 变宽字库 / 4bpp alpha 抗锯齿字库，兼容现有 FONT 容器格式）

字号档位（code = base + Unicode）：
  0x00000 12px 卡片文字
  0x10000 13px 气泡文字 / 模式标题
  0x20000 14px 按钮文字
  0x30000 26px 大标题（倒计时中 / 正计时中）

清单开关：
  font_bits = 1  （默认）逐像素 1bpp 二值掩码，08 号基线逐字节不变
  font_bits = 4  4bpp alpha（16 档覆盖度），固件按背景混色 -> 真抗锯齿
容器头字节 7 = 位深标志（0/1 视为 1bpp，4 = 4bpp），固件 UI_CacheFont 读它。
输出：px_flash/px_font.bin
"""
import os, struct, zlib
from PIL import Image, ImageDraw, ImageFont
import numpy as np
import ui_paths
from ui_layout import font_texts

DST      = ui_paths.FL
FONT_DIR = ui_paths.FONT_DIR
ui_paths.ensure_dirs()
# 清单里给了 font_face 就不需要思源黑体（国风版用全字库正楷）
if not os.environ.get("UI_SKIP_FONT_CHECK"):
    _mf = os.path.join(os.path.dirname(ui_paths.MANIFEST) or ".", "_清单.txt")
    _has_face = False
    if os.path.isfile(_mf):
        for _ln in open(_mf, encoding="utf-8-sig"):
            if _ln.split("#", 1)[0].strip().lower().startswith("font_face"):
                _has_face = True
    if not _has_face:
        ui_paths.require_font()


# ---- 素材清单里的可选扩展（保持 08 号基线的字库逐字节不变） ----
#   font_extra = 20        额外字号档位（追加在 4 档之后，base = 0x40000 起）
#   text_extra = 开启国风吧  额外需要覆盖的字符（追加进字符集）
def _man(path):
    m = {}
    if path and os.path.isfile(path):
        for ln in open(path, encoding="utf-8-sig"):
            ln = ln.split("#", 1)[0].strip()
            if ln and "=" in ln:
                k, v = ln.split("=", 1)
                m[k.strip().lower()] = v.strip()
    return m


_MAN = _man(ui_paths.MANIFEST)
# 字体与档位：
#   默认（08 号基线）= 思源黑体 Medium/Bold，4 档 12/13/14/26 —— 一个键都不写时逐字节不变
#   清单写 font_face + font_sizes 可整套换字体（国风版=全字库正楷 TW-Kai，12/13/14/16）
_FACE = _MAN.get("font_face", "").strip()
if _FACE and not os.path.isabs(_FACE):
    _FACE = os.path.join(os.path.dirname(ui_paths.MANIFEST) or ".", _FACE)
_SIZES_TXT = _MAN.get("font_sizes", "").strip()

if _SIZES_TXT:
    SIZES = []
    for _i, _sz in enumerate([x for x in _SIZES_TXT.replace("，", ",").split(",") if x.strip()]):
        SIZES.append((int(_sz), _i * 0x10000))
else:
    SIZES = [(12, 0x00000), (13, 0x10000), (14, 0x20000), (26, 0x30000)]
    for _i, _sz in enumerate([x for x in _MAN.get("font_extra", "").replace("，", ",").split(",") if x.strip()]):
        SIZES.append((int(_sz), 0x40000 + _i * 0x10000))
if len(SIZES) > 4 or _SIZES_TXT:
    print("字号档位:", SIZES)
GLYPH_THR = int(_MAN.get("font_thr", "110") or 110)
GLYPH_SS = int(_MAN.get("font_ss", "1") or 1)
# 位深：1=1bpp 二值掩码（旧，08 号基线），4=4bpp alpha 抗锯齿（2026-11-20）
FONT_BITS = int(_MAN.get("font_bits", "1") or 1)
if FONT_BITS not in (1, 4):
    raise SystemExit("font_bits 只支持 1 或 4（给的是 %s）" % FONT_BITS)
print("字体:", _FACE if _FACE else "思源黑体 Medium/Bold（按档位选字重）",
      "阈值 %d 超采样 x%d 位深 %dbpp" % (GLYPH_THR, GLYPH_SS, FONT_BITS))
# 每档字重（按字号灵活选）：小字号 Medium 更清晰，大标题 Bold 更有分量
WEIGHT = {12: "Medium", 13: "Medium", 14: "Medium", 16: "Medium", 20: "Medium", 26: "Bold"}

# ---- 需要覆盖的字符集（从三屏实际用到的字符串收集） ----
TEXTS = [
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
for t in [*TEXTS, *font_texts(ui_paths.SRC)]:
    chars |= set(t)
_extra = _MAN.get("text_extra", "")
if _extra:
    chars |= set(_extra)
    print("额外字符:", _extra)
chars = sorted(chars)
print("字符数:", len(chars), "".join(chars))

def _to_alpha(gray):
    """灰度覆盖度 -> 4bit alpha（0=纸、15=满墨）。四舍五入到 16 档。"""
    return (((gray.astype(np.int32) * 15 + 127) // 255).clip(0, 15)).astype(np.uint8)


def glyph_bitmap(f, ch, thr=None):
    """返回 (位图, w, h)。
    FONT_BITS=1 -> 布尔掩码（二值化，历史行为）；FONT_BITS=4 -> 0..15 覆盖度 alpha。"""
    if thr is None:
        thr = GLYPH_THR
    asc, desc = f.getmetrics()
    h = asc + desc
    w = max(1, int(round(f.getlength(ch))))
    if GLYPH_SS <= 1:
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=f, anchor="la", fill=255)
        g = np.asarray(img)
    else:
        # 超采样路径：按 N 倍字号渲染 → LANCZOS 缩到目标格 → 二值化/取覆盖度
        n = GLYPH_SS
        fbig = ImageFont.truetype(f.path, f.size * n)
        W, H = w * n, h * n
        img = Image.new("L", (W, H), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=fbig, anchor="la", fill=255)
        g = np.asarray(img.resize((w, h), Image.LANCZOS))
    if FONT_BITS == 4:
        return _to_alpha(g), w, h
    return g > thr, w, h


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


def pack_vflip4(a, w, h):
    """4bpp alpha 行优先、**高半字节=左像素**；行序翻转（row0=设计底部）。
    固件按 (w+1)/2 字节/行、alpha 0..15 混色，与 pack_vflip 同一坐标系。"""
    row_bytes = (w + 1) // 2
    out = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(w):
            v = int(a[y, x]) & 0x0F
            if x & 1:
                row[x >> 1] |= v
            else:
                row[x >> 1] |= (v << 4)
        out += row
    return bytes(out)


def pack_glyph(a, w, h):
    return pack_vflip4(a, w, h) if FONT_BITS == 4 else pack_vflip(a, w, h)

items = []
for size, base in SIZES:
    fpath = _FACE if _FACE else os.path.join(FONT_DIR, "SourceHanSansSC-%s.otf" % WEIGHT.get(size, "Medium"))
    if not os.path.isfile(fpath):
        raise SystemExit("找不到字体文件：%s\n（清单 font_face 写绝对路径，或编辑 ui_paths_local.py 的 FONT_DIR）" % fpath)
    f = ImageFont.truetype(fpath, size)
    cache = {ch: glyph_bitmap(f, ch) for ch in chars}
    asc, desc = f.getmetrics()
    H = asc + 2                               # 统一基线，并留 2 行降部（否则「星」「成」这类字底横被裁掉）
    print("  %dpx 单元格高 H=%d (asc=%d desc=%d)" % (size, H, asc, desc))
    for ch in chars:
        a, w, h = cache[ch]
        cell = np.zeros((H, w), np.uint8 if FONT_BITS == 4 else bool)
        blk = a[0:min(H, h)]                  # 从行 0 起裁相同高度，保基线一致
        cell[:blk.shape[0]] = blk
        items.append((base + ord(ch), ch, w, H, pack_glyph(cell, w, H)))

items.sort()
count = len(items)
index_offset = 256
entry = 12
bitmap_offset = index_offset + count * entry

hdr = struct.pack('<4sBBBBBHIIII', b'FONT', 1, 1, 1,
                  0 if FONT_BITS == 1 else FONT_BITS,   # 字节 7：位深标志（0=1bpp，4=4bpp alpha）
                  0, 0, count, index_offset, bitmap_offset, 0)
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
SAMPLE_BY_SIZE = {12: "2026-11-03 星期二 23℃ 58%",
                  13: "主人，有什么需要帮助的吗？",
                  14: "开始 暂停 重置",
                  20: "设置闹钟吗？",
                  26: "倒计时中"}
rows = []
for size, base in SIZES:
    line = []
    for ch in SAMPLE_BY_SIZE.get(size, "0123456789"):
        for code, c, w, h, data in items:
            if code == base + ord(ch):
                if FONT_BITS == 4:
                    row_bytes = (w + 1) // 2
                    a = np.zeros((h, w), np.uint8)
                    for y in range(h):
                        for x in range(w):
                            by = data[y * row_bytes + (x >> 1)]
                            a[h-1-y, x] = (by & 0x0F) if (x & 1) else (by >> 4)
                    alpha = (a.astype(np.float32) / 15.0)[:, :, None]
                    arr = (np.array([20, 26, 70], np.float32) * (1 - alpha) +
                           np.array([250, 250, 255], np.float32) * alpha).astype(np.uint8)
                    line.append(Image.fromarray(arr))
                else:
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




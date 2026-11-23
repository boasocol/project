# -*- coding: utf-8 -*-
r"""字库缺字自检：把 screen.c / app_voice.c 等源码里所有中文串字面量抠出来，
和「已构建字库」的字符集比对，列出缺字——缺字的后果是屏上**静默空白**，很难发现。

用法：py -3 tools\check_font_chars.py <firmware_dir> <px_font.bin>
"""
import os
import re
import struct
import sys

SRC_DIR = sys.argv[1] if len(sys.argv) > 1 else r"HT32L52231_QFN32\project\app"
FONT = sys.argv[2] if len(sys.argv) > 2 else r"UI产物\新ui界面_国风版_20261119_20261119_135542\px_flash\px_font.bin"


def font_chars(path):
    """FONT 头（build_px_font.py '<4sBBBBBHIIII'，28B）：
       [0..4) 'FONT' | [4..9) 版本/标志 5B | [9..11) 保留 u16 |
       [11..15) count u32 | [15..19) index_off u32 | [19..23) bitmap_off u32 | 其余保留
       （固件 ui.c UI_CacheFont 就是这么读的：count 取 h[11..14]）"""
    b = open(path, "rb").read()
    assert b[:4] == b"FONT", b[:4]
    count = struct.unpack("<I", b[11:15])[0]
    index_off = struct.unpack("<I", b[15:19])[0]
    codes = set()
    for i in range(count):
        code = struct.unpack("<I", b[index_off + i * 12: index_off + i * 12 + 4])[0]
        codes.add(code & 0xFFFF)
    return codes


ESC = re.compile(r'\\x[0-9A-Fa-f]{2}|\\[nrt"\\]')


def strip_esc(lit):
    return ESC.sub("", lit)


def source_chars(dirpath):
    """只取「真正走字库绘制」的字符串：
       UI_DrawStrPx(...) / Screen_Bubble(...) / bub_default 里的 return "..."。
       printf / 注释里的中文不走字库，不能算缺字。"""
    out = {}
    lit = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
    for fn in sorted(os.listdir(dirpath)):
        if not fn.endswith(".c"):
            continue
        txt = open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
        # ① 直接传给绘制函数的字面量
        for m in re.finditer(r"(UI_DrawStrPx\s*\(|Screen_Bubble\s*\(|return\s+)([^;\n]{0,160})", txt):
            for sm in lit.finditer(m.group(2)):
                for c in strip_esc(sm.group(1)):
                    if ord(c) > 0x2000:
                        out.setdefault(c, set()).add(fn)
        # ② const char *x = "..." 这类拼串用的字面量（如单位 "°C"）
        for m in re.finditer(r"const\s+char\s*\*\w+\s*=\s*([^;\n]{0,120})", txt):
            for sm in lit.finditer(m.group(1)):
                for c in strip_esc(sm.group(1)):
                    if ord(c) > 0x1F:
                        out.setdefault(c, set()).add(fn)
        # ③ 已经收集过的不再重复
        continue
        for m in re.finditer(r"x", txt):
            for sm in lit.finditer(m.group(2)):
                for c in sm.group(1):
                    if ord(c) > 0x2000:
                        out.setdefault(c, set()).add(fn)
    return out


codes = font_chars(FONT)
used = source_chars(SRC_DIR)
missing = {c: f for c, f in used.items() if ord(c) not in codes}
print("字库字符 %d 个；源码里用到非 ASCII 字符 %d 个" % (len(codes), len(used)))
if not missing:
    print("✅ 没有缺字")
else:
    print("!! 缺字 %d 个（屏上会静默空白）：" % len(missing))
    for c, files in sorted(missing.items()):
        print("   %s U+%04X  用于 %s" % (c, ord(c), ",".join(sorted(files))))

# -*- coding: utf-8 -*-
"""像素游戏机 UI —— 组装 2MB Flash 镜像

布局（与固件 ui.h 常量一致）：
  0x000000  HTUI 分区头
  0x001000  BG0 153600 | 0x025C00 BG1 | 0x04A800 BG2  → 结束 0x071800
  0x072000  FONT（FONT 容器，px_font.bin）
  0x100000  CONF（32B 容器，沿用旧格式与默认值）
  0x110000  SPR1（px_spr.bin）
"""
import os, struct, zlib
from pathlib import Path
import countdown_layout
import ui_layout
import ui_paths

FL = ui_paths.FL
OUT = ui_paths.IMG_OUT
ui_paths.ensure_dirs()

UI_OFF, FONT_OFF, CONFIG_OFF, SPR_OFF = 0x001000, 0x072000, 0x100000, 0x110000
SCREEN_BYTES = 320 * 240 * 2
FLASH_SIZE = 0x200000

bg = [open(os.path.join(FL, f"px_bg{i}.bin"), "rb").read() for i in range(3)]
font = open(os.path.join(FL, "px_font.bin"), "rb").read()
spr = open(os.path.join(FL, "px_spr.bin"), "rb").read()
assert all(len(b) == SCREEN_BYTES for b in bg), [len(b) for b in bg]
source_dir = Path(ui_paths.SRC)
profile_path = source_dir / ui_layout.LAYOUT_FILE
layout_path = source_dir / countdown_layout.LAYOUT_FILE
countdown = skin = None
if profile_path.is_file():
    if layout_path.is_file():
        raise SystemExit("choose ui_layout.json or countdown_layout.json, not both")
    spr, countdown, skin = ui_layout.pack_file(profile_path, spr, font)
elif layout_path.is_file():
    countdown = countdown_layout.pack_file(layout_path, spr, font)

# ---- 容量硬校验：超了就报错，绝不产出 >2MB 的镜像 ----
# （2026-11-18 实测：新国风那版机器人画布大，SPR 到 1.19MB，合计 2.20MB，
#   早先的实现会静默写出一个 2.3MB 的“镜像”，烧进去必花屏）
_spr_avail = FLASH_SIZE - SPR_OFF
_need = SPR_OFF + len(spr)
if _need > FLASH_SIZE:
    msg = [
        "",
        "=" * 64,
        "!! 资源超出 2MB Flash 容量，已中止（不会产出坏镜像）",
        "   背景 3×%d = %d B" % (SCREEN_BYTES, 3 * SCREEN_BYTES),
        "   字库      = %d B" % len(font),
        "   精灵表    = %d B  (可用 %d B，超出 %d B)" % (len(spr), _spr_avail, _need - FLASH_SIZE),
        "",
        "  怎么省：",
        "   1) 动画画布缩小（每帧字节 ≈ 宽×高×2 + ceil(宽/8)×高）",
        "   2) 动画帧数减少（当前每状态固定 16 槽位）",
        "   3) 减少状态数 / 去掉用不到的状态",
        "=" * 64,
    ]
    raise SystemExit("\n".join(msg))

# CONF：亮度80/时区8/闹钟关7:00/蓝牙已配对，与旧默认一致
payload = struct.pack('<BBBBBB', 80, 8, 0, 7, 0, 1) + b'\x00' * 10
conf = struct.pack('<4sIII', b'CONF', 1, len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + payload

ui_len = SCREEN_BYTES * 3
body = struct.pack('<4sIIIIIIII', b'HTUI', 1, UI_OFF, ui_len,
                   FONT_OFF, CONFIG_OFF - FONT_OFF, CONFIG_OFF, 0x00010000, 0)
hdr = body[:32] + struct.pack('<I', zlib.crc32(body[:32]) & 0xFFFFFFFF) + b'\x00' * (256 - 36)

img = bytearray(b'\xFF' * FLASH_SIZE)
def put(off, data):
    img[off:off + len(data)] = data

put(0, hdr)
if countdown is not None:
    put(countdown_layout.OFFSET, countdown)
    print(f"  CNTL  0x{countdown_layout.OFFSET:06X}  {len(countdown):7d} B")
if skin is not None:
    if ui_layout.OFFSET + len(skin) > UI_OFF:
        raise SystemExit("layout exceeds the reserved area before BG0")
    put(ui_layout.OFFSET, skin)
    print(f"  SKIN  0x{ui_layout.OFFSET:06X}  {len(skin):7d} B")
for i, b in enumerate(bg):
    put(UI_OFF + i * SCREEN_BYTES, b)
put(FONT_OFF, font)
put(CONFIG_OFF, conf)
put(SPR_OFF, spr)

open(OUT, "wb").write(bytes(img))
print("flash_image_px.bin %d B" % os.path.getsize(OUT))
for name, off, size in [("BG0", UI_OFF, SCREEN_BYTES), ("BG1", UI_OFF + SCREEN_BYTES, SCREEN_BYTES),
                        ("BG2", UI_OFF + 2 * SCREEN_BYTES, SCREEN_BYTES),
                        ("FONT", FONT_OFF, len(font)), ("CONF", CONFIG_OFF, len(conf)),
                        ("SPR", SPR_OFF, len(spr))]:
    print("  %-5s 0x%06X  %7d B  → 0x%06X" % (name, off, size, off + size - 1))
print("  合计已用 %.2f MB / 2.00 MB" % ((SPR_OFF + len(spr)) / 1048576.0))

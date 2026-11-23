# -*- coding: utf-8 -*-
"""像素游戏机 UI —— Flash 资源打包（第一阶段：背景 + 机器人精灵 + 数字/开关精灵）
输出到 datasheets/ui设计/px_flash/：
  px_bg0.bin / px_bg1.bin / px_bg2.bin   320x240 RGB565 小端，row0=设计底部（与现有固件翻转约定一致）
  px_spr.bin                              SPR1 容器（机器人帧 + 数字 + 开关/星期钮）
  px_manifest.txt                         偏移/尺寸清单（供固件 ui.h 常量对照）

容器 SPR1：
  [0..4)  'SPR1' | [4..8) ver=1 | [8..12) n
  [16..)  n 条 16B 索引: {w:u16,h:u16, x:i16,y:i16, px_off:u32, mask_off:u32}
  px_off/mask_off 为相对容器起点的绝对偏移；像素 RGB565 行优先（row0=设计底部），掩码 1bpp。
"""
import os, struct
from PIL import Image
import numpy as np
import ui_paths
from circus_skin import SWITCH_POSITION, WEEKDAY_X, WEEKDAY_Y, digit_sprite, font

# 路径统一由 ui_paths.py 提供（可被 ui_paths_local.py / 环境变量覆盖）
SRC = ui_paths.SRC
DIG = ui_paths.DIG
DST = ui_paths.FL
ui_paths.ensure_dirs()
ui_paths.require_src()
def P(n): return os.path.join(SRC, n)

# ---------- 0) 素材清单（可选，放在素材目录里，叫 _清单.txt） ----------
# 设计方命名千奇百怪，靠关键字猜容易猜错。清单格式（一行一项，# 开头是注释）：
#     bg0 = 主界面_空.png          ← 空白背景（烧进 Flash）
#     bg1 = ...
#     bg2 = ...
#     main0 = xxx_休眠.gif         ← 屏名+状态号（0 休眠 / 1 唤醒 / 2 接受指令）
#     sec_patch = 231,45,281,72    ← 主界面秒底色补图矩形；写 none 表示不用补
# 哪个键不写就退回关键字匹配。
def load_manifest(path):
    man = {}
    if not path or not os.path.isfile(path):
        return man
    for ln in open(path, encoding="utf-8-sig"):
        ln = ln.split("#", 1)[0].strip()
        if not ln or "=" not in ln:
            continue
        k, v = ln.split("=", 1)
        man[k.strip().lower()] = v.strip()
    print("素材清单 %s -> %d 项" % (os.path.basename(path), len(man)))
    return man

MAN = load_manifest(ui_paths.MANIFEST)

# ---------- 1a) bg0 补秒胶囊（空白模板缺这块美术，仅旧版需要） ----------
def patch_sec_pill(blank_img, full_img, box):
    """从全显稿裁 box 区域，把黄色数字抹成胶囊底色，再贴回空白模板"""
    b = np.asarray(blank_img.convert("RGB")).copy()
    f = np.asarray(full_img.convert("RGB")).astype(np.int16)
    x0, y0, x1, y1 = box
    sub = f[y0:y1, x0:x1]
    r, g, bl = sub[:, :, 0], sub[:, :, 1], sub[:, :, 2]
    yellow = (r > 170) & (g > 140) & (bl < 170)
    # 胶囊填充色 = 区域内非黄像素的中位色
    flat = sub[~yellow].reshape(-1, 3)
    fill = np.median(flat, axis=0).astype(np.int16)
    sub2 = sub.copy()
    sub2[yellow] = fill
    b[y0:y1, x0:x1] = sub2.astype(np.uint8)
    return Image.fromarray(b)

# ---------- 1) 背景：空白模板 → RGB565，垂直翻转 ----------
BGS = [MAN.get("bg0", "时间界面.png"),
       MAN.get("bg1", "倒计时界面.png"),
       MAN.get("bg2", "闹钟界面.png")]
for i, n in enumerate(BGS):
    im = Image.open(P(n)).convert("RGB")
    assert im.size == (320, 240), (n, im.size)
    if i == 0:
        sp = MAN.get("sec_patch", "231,45,281,72")
        if sp.strip().lower() != "none":
            box = tuple(int(v) for v in sp.split(","))
            full = Image.open(P(MAN.get("bg0_full", "时间界面（全显）.png"))).convert("RGB")
            im = patch_sec_pill(im, full, box)          # 秒胶囊矩形，含描边
    im = im.transpose(Image.FLIP_TOP_BOTTOM)          # row0 = 设计 y=239
    a = np.asarray(im).astype(np.uint16)
    rgb565 = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
    out = rgb565.astype('<u2').tobytes()
    open(os.path.join(DST, f"px_bg{i}.bin"), "wb").write(out)
    print(f"px_bg{i}.bin  {n}  {len(out)} B")

# ---------- 2) 机器人精灵 ----------
ALL = os.listdir(SRC)
def find_gif(screen_kw, state_kw):
    for f in ALL:
        if f.endswith(".gif") and screen_kw in f and state_kw in f:
            return f
    raise SystemExit("找不到 GIF: 屏名含「%s」且状态含「%s」\n"
                     "（在素材目录里加一个 _清单.txt 显式指定，格式见 build_px_flash.py 顶部注释）"
                     % (screen_kw, state_kw))

# 时间屏只有 休眠/唤醒 两个状态；倒计时/闹钟 三个状态
# 素材目录里有 _清单.txt 时按 main0/cnt0..alm2 取，否则退回关键字匹配
_SPEC = [
    ("main", 0, "时间界面", "休眠"),
    ("main", 1, "时间界面", "唤醒"),
    ("cnt",  0, "倒计时",   "休眠"),
    ("cnt",  1, "倒计时",   "唤醒"),
    ("cnt",  2, "倒计时",   "接受指令"),
    ("alm",  0, "闹钟",     "休眠"),
    ("alm",  1, "闹钟",     "唤醒"),
    ("alm",  2, "闹钟",     "接受指令"),
]
ROBOTS = []
for scr, st, skw, tkw in _SPEC:
    gif = MAN.get("%s%d" % (scr, st)) or find_gif(skw, tkw)
    if not os.path.isfile(os.path.join(SRC, gif)):
        raise SystemExit("素材清单里指定的文件不存在：%s" % gif)
    ROBOTS.append(((scr, st), gif))
print("GIF 映射:", [(k, g) for k, g in ROBOTS])

def load_frames(path):
    im = Image.open(path)
    fr = []
    for i in range(im.n_frames):
        im.seek(i)
        fr.append(im.convert("RGBA"))
    return fr

# 每屏所有帧的并集不透明区 → 统一画布
screen_box = {}
for key, gif in ROBOTS:
    fr = load_frames(P(gif))
    x0 = y0 = 10**9; x1 = y1 = -1
    for f in fr:
        a = np.asarray(f)[:, :, 3] > 0
        ys, xs = np.where(a)
        x0 = min(x0, xs.min()); x1 = max(x1, xs.max())
        y0 = min(y0, ys.min()); y1 = max(y1, ys.max())
    s = key[0]
    if s not in screen_box:
        screen_box[s] = [x0, y0, x1, y1]
    else:
        b = screen_box[s]
        screen_box[s] = [min(b[0], x0), min(b[1], y0), max(b[2], x1), max(b[3], y1)]
print("每屏机器人并集框:", {k: tuple(v) for k, v in screen_box.items()})

# 屏幕上的贴图位置（设计稿实测）

sprites = []   # (键, w, h, x, y, pixels_rgb565_bytes, mask_bytes)

def add_sprite(key, im_rgba, x, y):
    """im_rgba: 已裁到画布；生成 RGB565（row0=设计底部）+1bpp 掩码"""
    w, h = im_rgba.size
    a = np.asarray(im_rgba)
    rgb = a[:, :, :3].transpose(1, 0, 2)        # 转成列优先 → 后续按行(设计底部起)写出
    alpha = a[:, :, 3].transpose(1, 0)
    px = bytearray(); mask = bytearray()
    mrow = (w + 7) // 8
    for yy in range(h - 1, -1, -1):             # 设计底部行先写（与背景一致）
        bits = bytearray(mrow)
        for xx in range(w):
            r, g, b = rgb[xx, yy]
            v = ((int(r) >> 3) << 11) | ((int(g) >> 2) << 5) | (int(b) >> 3)
            px += struct.pack('<H', v)
            if alpha[xx, yy] > 0:
                bits[xx >> 3] |= (0x80 >> (xx & 7))
        mask += bits
    sprites.append((key, w, h, x, y, bytes(px), bytes(mask)))

# 每个状态固定 16 个槽位 —— 必须与固件一致（ui.h 的 UI_SPR_ROBOT：state*16 + frame）。
# ⚠️ 帧数少于 16 时必须循环补足，否则后面所有精灵序号整体前移，整套 UI 会错位/丢图
#    （2026-11-18 实测：一批 8 帧的 GIF 让精灵数从 167 掉到 103，序号全乱）。
FRAMES_PER_STATE = 16

for key, gif in ROBOTS:
    s = key[0]; bx0, by0, bx1, by1 = screen_box[s]
    w, h = bx1 - bx0 + 1, by1 - by0 + 1
    fr = load_frames(P(gif))
    n0 = len(fr)
    if n0 == 0:
        raise SystemExit("GIF 没有帧：%s" % gif)
    fr = fr[:FRAMES_PER_STATE]
    k = 0
    while len(fr) < FRAMES_PER_STATE:          # 循环补足到 16 帧
        fr.append(fr[k]); k += 1
        if k >= n0: k = 0
    print("  机器人 %-34s %2d 帧 -> %d 槽位%s" %
          (gif, n0, FRAMES_PER_STATE,
           "" if n0 == FRAMES_PER_STATE else ("（循环补足）" if n0 < FRAMES_PER_STATE else "（截断）")))
    for idx, f in enumerate(fr):
        box = (bx0, by0, bx1 + 1, by1 + 1)
        add_sprite((s, key[1], idx), f.crop(box),
                   bx0, by0)

# ---------- 3) 数字 / 冒号 / 开关 / 星期钮 ----------
def add_png(key, path, x, y):
    add_sprite(key, Image.open(path).convert("RGBA"), x, y)

# 生成匹配各屏版式的数字精灵，保留固件序号和调试用 PNG。
face = font(ui_paths.FONT_DIR, 80)
for style, prefix in (("main", "v3_main_"), ("alm", "alm_"), ("cnt", "cnt_")):
    for char in "0123456789:":
        sprite = digit_sprite(style, char, face)
        suffix = "col" if char == ":" else char
        sprite.save(os.path.join(DIG, f"{prefix}{suffix}.png"))
        add_sprite((style, char), sprite, 0, 0)
for n, x, y in [("sec5", 234, 44), ("sec6", 254, 44)]:
    add_png((n,), os.path.join(DIG, f"main_s{n[-1]}.png"), x, y)

def cut_add(key, img, box, x, y):
    im = Image.open(P(img)).convert("RGBA").crop(box)
    add_sprite(key, im, x, y)

def cut_box(key, img, boxspec, x, y):
    cut_add(key, img, tuple(int(v) for v in boxspec.split(",")), x, y)

# 开关 / 星期钮（开/关 各一张）：
#   ① 优先用设计方单独给的小图：清单里写 sw_on/sw_off/tg_on/tg_off 的文件名
#   ② 没给就从闹钟全显稿抠：清单里写 sw_src / sw1_box / sw0_box / tg1_box / tg0_box
#   ③ 都没写 = 旧版 08 号那套默认值
if MAN.get("sw_on") and MAN.get("sw_off"):
    add_png(("alm_sw", 1), P(MAN["sw_on"]), *SWITCH_POSITION)     # 贴图坐标由固件 screen.c 决定
    add_png(("alm_sw", 0), P(MAN["sw_off"]), *SWITCH_POSITION)
else:
    cut_box(("alm_sw", 1), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("sw1_box", "218,68,280,106"), 218, 68)
    cut_box(("alm_sw", 0), MAN.get("sw_src0", "闹钟界面.png"),
            MAN.get("sw0_box", "218,68,280,106"), 218, 68)

if MAN.get("tg_on") and MAN.get("tg_off"):
    add_png(("alm_tg", 1), P(MAN["tg_on"]), WEEKDAY_X[0], WEEKDAY_Y)
    add_png(("alm_tg", 0), P(MAN["tg_off"]), WEEKDAY_X[0], WEEKDAY_Y)
else:
    cut_box(("alm_tg", 1), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("tg1_box", "57,180,82,196"), 57, 180)
    cut_box(("alm_tg", 0), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("tg0_box", "215,180,240,196"), 215, 180)

# ---------- 4) 写 SPR1 ----------
n = len(sprites)
hdr_len = 16 + 16 * n
body = bytearray()
idx = bytearray()
payload_offsets = {}
for key, w, h, x, y, px, mask in sprites:
    payload = (w, h, px, mask)
    if payload not in payload_offsets:
        po = hdr_len + len(body)
        payload_offsets[payload] = (po, po + len(px))
        body.extend(px)
        body.extend(mask)
    po, mo = payload_offsets[payload]
    idx += struct.pack('<HHhhII', w, h, x, y, po, mo)
hdr = struct.pack('<4sIII', b'SPR1', 1, n, 0)   # 16B：索引表紧跟其后
blob = hdr + bytes(idx) + bytes(body)
open(os.path.join(DST, "px_spr.bin"), "wb").write(blob)
print(f"px_spr.bin  {n} sprites  {len(blob)} B ({len(blob)/1024:.0f} KB)")

# ---------- 5) 清单 ----------
lines = ["# 像素游戏机 UI Flash 资源清单（build_px_flash.py 生成）",
         "BG0  0x001000  %d B" % (320*240*2),
         "BG1  0x025C00  %d B" % (320*240*2),
         "BG2  0x04A800  %d B" % (320*240*2),
         "SPR  0x110000  %d B (SPR1, %d sprites)" % (len(blob), n),
         "", "## 精灵索引"]
for i, (key, w, h, x, y, px, mask) in enumerate(sprites):
    lines.append(f"  [{i:3d}] {key} {w}x{h} at ({x},{y})")
open(os.path.join(DST, "px_manifest.txt"), "w", encoding="utf-8").write("\n".join(lines))
print("manifest ok")


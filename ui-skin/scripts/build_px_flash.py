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

# ---------- 1b) 抠图残字清除（背景方没抠干净时用） ----------
def patch_blank(im, box):
    """把 box 区域抹成「该区域里的浅色中位色」。
    用途：美术交的「（抠图）」背景偶尔还有没抠掉的动态文字（如闹钟屏的数字 08:35），
    固件再往上贴数字就会叠影。在素材阶段抹掉最省事（固件不用平涂、也不吃 SPI 带宽）。
    清单写法：blank2 = 25,24,170,74   （屏号 + 闭开区间）"""
    a = np.asarray(im.convert("RGB")).astype(np.int32)   # 必须 int32：255*299 会溢出 int16
    x0, y0, x1, y1 = box
    sub = a[y0:y1, x0:x1]
    lum = (sub[:, :, 0] * 299 + sub[:, :, 1] * 587 + sub[:, :, 2] * 114) // 1000
    light = sub[lum > 170]
    if len(light) == 0:
        raise SystemExit("blank 补丁区域里没有浅色像素可取样：%s" % (box,))
    fill = np.median(light, axis=0).astype(np.int32)
    a[y0:y1, x0:x1] = fill
    print("    残字清除 box=%s fill=RGB%s" % (box, tuple(int(v) for v in fill)))
    return Image.fromarray(a.astype(np.uint8))

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
    bl = MAN.get("blank%d" % i, "").strip()
    if bl and bl.lower() != "none":
        im = patch_blank(im, tuple(int(v) for v in bl.split(",")))
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
# 旧素材（08 号）的 GIF 是紧裁到画布原点，所以这里给的是「屏上落点」；
# 新素材（国风）的 GIF 直接是 320x240 全画布、机器人已在最终位置 → 清单里写 place_*=0,0
def _place(key, default):
    v = MAN.get(key)
    if not v:
        return default
    p = tuple(int(t) for t in v.split(","))
    return (p[0], p[1])


PLACE = {"main": _place("place_main", (38, 140)),
         "cnt":  _place("place_cnt",  (10, 165)),
         "alm":  _place("place_alm",  (255, 150))}

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
                   PLACE[s][0] + bx0, PLACE[s][1] + by0)

# ---------- 3) 数字 / 冒号 / 开关 / 星期钮 ----------
def digp(name):
    """数字/小控件 png 查找：先 DIG（我方固定输入），再素材目录"""
    p = os.path.join(DIG, name)
    if os.path.isfile(p):
        return p
    p2 = os.path.join(SRC, name)
    if os.path.isfile(p2):
        return p2
    raise SystemExit("找不到数字/控件图：%s\n  DIG=%s\n  SRC=%s" % (name, DIG, SRC))


def add_png(key, path, x, y):
    add_sprite(key, Image.open(path).convert("RGBA"), x, y)

# 主屏/闹钟/倒计时 三套大数字 + 冒号：清单里可指定前缀（dig_main/dig_alm/dig_cnt），
# 默认沿用 08 号那套（v3_main_ / alm_ / cnt_）。
DIGPFX = (("main", MAN.get("dig_main") or "v3_main_"),
          ("alm",  MAN.get("dig_alm")  or "alm_"),
          ("cnt",  MAN.get("dig_cnt")  or "cnt_"))
for style, prefix in DIGPFX:
    for ch in "0123456789":
        add_png((style, ch), digp(f"{prefix}{ch}.png"), 0, 0)
    add_png((style, ":"), digp(f"{prefix}col.png"), 0, 0)

# 秒：清单给了 dig_sec（如 sec_）就用「0-9 十张数字」，否则沿用旧的两张固定图（08 号）
if MAN.get("dig_sec"):
    for d in "0123456789":
        add_png(("sec", d), digp("%s%s.png" % (MAN["dig_sec"], d)), 0, 0)
else:
    for n, x, y in [("sec5", 234, 44), ("sec6", 254, 44)]:
        add_png((n,), digp(f"main_s{n[-1]}.png"), x, y)

def cut_add(key, img, box, x, y):
    im = Image.open(P(img)).convert("RGBA").crop(box)
    add_sprite(key, im, x, y)

def cut_box(key, img, boxspec, x, y):
    cut_add(key, img, tuple(int(v) for v in boxspec.split(",")), x, y)

# 开关（开/关 各一张）：
#   ① 优先用单独给的小图：清单 sw_on / sw_off
#   ② 没给就从闹钟全显稿抠：sw_src / sw1_box / sw0_box
#   ③ 都没写 = 旧版 08 号那套默认值
if MAN.get("sw_on") and MAN.get("sw_off"):
    add_png(("alm_sw", 1), digp(MAN["sw_on"]),  0, 0)   # 贴图坐标由固件 screen.c 决定
    add_png(("alm_sw", 0), digp(MAN["sw_off"]), 0, 0)
else:
    cut_box(("alm_sw", 1), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("sw1_box", "218,68,280,106"), 218, 68)
    cut_box(("alm_sw", 0), MAN.get("sw_src0", "闹钟界面.png"),
            MAN.get("sw0_box", "218,68,280,106"), 218, 68)

# 星期图标：
#   新式（国风）—— 清单给 tg5_on/tg5_off … tg7_on/tg7_off 六个文件，3 个图标各两态；
#   旧式（08）—— tg_on/tg_off 两张，或从闹钟稿按 tg1_box/tg0_box 抠。
TG3 = all(MAN.get("tg%d_%s" % (i, s)) for i in (5, 6, 7) for s in ("on", "off"))
if TG3:
    for i in (5, 6, 7):
        add_png(("alm_tg", i, 1), digp(MAN["tg%d_on" % i]),  0, 0)
        add_png(("alm_tg", i, 0), digp(MAN["tg%d_off" % i]), 0, 0)
elif MAN.get("tg_on") and MAN.get("tg_off"):
    add_png(("alm_tg", 1), P(MAN["tg_on"]),  0, 0)
    add_png(("alm_tg", 0), P(MAN["tg_off"]), 0, 0)
# 闹钟槽号数字（1/2/3）：清单给 dig_tag（如 tag_）就出 3 张，接在星期图标之后
if MAN.get("dig_tag"):
    for d in "123":
        add_png(("alm_tag", d), digp("%s%s.png" % (MAN["dig_tag"], d)), 0, 0)

else:
    cut_box(("alm_tg", 1), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("tg1_box", "57,180,82,196"), 57, 180)
    cut_box(("alm_tg", 0), MAN.get("sw_src", "闹钟界面（打开状态）.png"),
            MAN.get("tg0_box", "215,180,240,196"), 215, 180)

# ⚠️ 「闹钟开关」四字的抹字贴片必须排在槽号数字之后：
#    ui.h 里 UI_SPR_ALM_TAG = 179..181、UI_SPR_ALM_SWLBL = 182，顺序反了整套精灵序号会错位。
if MAN.get("spr_swlbl"):
    add_png(("swlbl", 0), digp(MAN["spr_swlbl"]), 0, 0)

# ---------- 4) 写 SPR1 ----------
n = len(sprites)
hdr_len = 16 + 16 * n
body = bytearray(); idx = bytearray()
off = hdr_len
for key, w, h, x, y, px, mask in sprites:
    po = off; mo = off + len(px)
    idx += struct.pack('<HHhhII', w, h, x, y, po, mo)
    body += px + mask
    off = mo + len(mask)
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


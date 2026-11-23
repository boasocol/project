# -*- coding: utf-8 -*-
"""把一个风格的素材打成独立产物包（不往交付包里写任何东西）。

用法：
    python pack_style.py <素材目录> [产物根目录]

产物结构（默认产物根 = 交付包上一层的 UI产物\\）：

    <产物根>\\<风格名>_<时间戳>\\
    ├─ flash_image_px.bin        2MB 镜像（烧录用）
    ├─ px_flash\\                中间产物（背景/字库/精灵表 + manifest）
    ├─ digits\\                  这次用的数字精灵（工作副本，便于复现）
    ├─ 素材快照\\                这次真正用的素材（原样拷贝，便于复现/回溯）
    └─ build_info.txt            时间/来源/精灵数/各段大小/SHA256

设计要点：
  · 交付包只读：所有中间产物写进产物包，脚本目录不改
  · 一个交付包服务所有风格：风格差异只体现在「素材目录」和产物包里
  · 每次一个时间戳目录，不会互相覆盖，历史版本都在
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time

LAYOUT_FILES = ("_清单.txt", "countdown_layout.json", "ui_layout.json")

HERE = os.path.dirname(os.path.abspath(__file__))      # 交付包\scripts
PACK = os.path.dirname(HERE)                           # 交付包根

STEPS = [
    ("主界面数字精灵", "_px08_handdigits3.py"),
    ("闹钟/倒计时数字精灵", "build_px_alm_digits.py"),
    ("中文字库", "build_px_font.py"),
    ("背景 + 动画帧 + 开关/星期钮", "build_px_flash.py"),
    ("组装 2MB 镜像", "build_px_image.py"),
]

DIGIT_TEMPLATE = os.path.join(PACK, "assets", "px_digits")   # 只读输入（主界面数字 + 秒）
FONT_DIR = os.path.join(PACK, "assets", "fonts")
# 本仓库里这套脚本不在交付包内（交付包\scripts），PACK 会指错 → 退回环境变量
if not os.path.isdir(DIGIT_TEMPLATE):
    DIGIT_TEMPLATE = os.environ.get("UI_DIG_TEMPLATE") or ""
if not os.path.isdir(FONT_DIR):
    FONT_DIR = os.environ.get("UI_FONT_DIR") or FONT_DIR


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def read_manifest(src):
    """读素材目录里的 _清单.txt（key = value，# 注释），只用来自定流程的几项"""
    man = {}
    p = os.path.join(src, "_清单.txt")
    if os.path.isfile(p):
        for ln in open(p, encoding="utf-8-sig"):
            ln = ln.split("#", 1)[0].strip()
            if ln and "=" in ln:
                k, v = ln.split("=", 1)
                man[k.strip().lower()] = v.strip()
    return man


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src = os.path.abspath(argv[1].strip('"'))
    if not os.path.isdir(src):
        print("找不到素材目录：%s" % src)
        return 2
    # 产物默认写到 ui-skin/out/（仓库内、已 gitignore），也可以第二个参数显式指定
    out_root = os.path.abspath(argv[2]) if len(argv) > 2 else os.path.join(PACK, "out")
    style = os.path.basename(src.rstrip("\\/")) or "style"
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = os.path.join(out_root, "%s_%s" % (style, stamp))

    man = read_manifest(src)
    digits = os.path.join(out, "digits")
    fl = os.path.join(out, "px_flash")
    img = os.path.join(out, "flash_image_px.bin")
    os.makedirs(out, exist_ok=True)

    # 0) 数字精灵来源有两种：
    #    ① 默认：交付包 assets\px_digits 是固定输入，alm_/cnt_ 由脚本现场生成（08 号流程）
    #    ② 清单里写 digits = none：本风格数字由我方单独提供（清单 dig_dir 指定目录，直接拷进来）
    steps = list(STEPS)
    dig_src = DIGIT_TEMPLATE
    ext_digits = man.get("digits", "").lower() == "none"
    if ext_digits:
        steps = steps[2:]                      # 跳过两个数字生成脚本
        if man.get("dig_dir"):
            d = man["dig_dir"]
            dig_src = d if os.path.isabs(d) else os.path.abspath(os.path.join(src, d))
    if dig_src and os.path.isdir(dig_src):
        shutil.copytree(dig_src, digits, dirs_exist_ok=True)

    # 1) 环境变量喂给各构建脚本（优先级高于 ui_paths_local.py）
    env = dict(os.environ)
    env.update({
        "PYTHONUTF8": "1",
        "UI_SRC": src,
        "UI_DIG": digits,
        "UI_FL": fl,
        "UI_IMG": img,
        "UI_FONT_DIR": FONT_DIR,
    })

    print("=" * 62)
    print("  素材：%s" % src)
    print("  产物：%s" % out)
    if ext_digits:
        print("  数字：外部提供（digits=none，来源 %s）" % dig_src)
    print("=" * 62)
    for i, (label, script) in enumerate(steps, 1):
        print("\n[%d/%d] %s ..." % (i, len(steps), label))
        r = subprocess.run([sys.executable, os.path.join(HERE, script)],
                           cwd=PACK, env=env, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        tail = [ln for ln in (r.stdout or "").splitlines() if ln.strip()][-3:]
        for ln in tail:
            print("   " + ln)
        if r.returncode != 0:
            print("\n!! %s 失败：\n%s" % (script, (r.stderr or r.stdout or "")[-1500:]))
            return 1

    # 3) 素材快照（这次真正用的东西，原样留一份，便于复现）
    snap = os.path.join(out, "素材快照")
    os.makedirs(snap, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)):
        if f.lower().endswith((".png", ".gif")) or f in LAYOUT_FILES:
            shutil.copy2(os.path.join(src, f), os.path.join(snap, f))
            n += 1

    # 4) build_info.txt
    spr = os.path.join(fl, "px_spr.bin")
    spr_n = "-"
    man = os.path.join(fl, "px_manifest.txt")
    if os.path.isfile(man):
        for ln in open(man, encoding="utf-8"):
            if "sprites)" in ln:
                spr_n = ln.strip()
                break
    packed_layout = "-"
    packed_sprites = "-"
    if os.path.isfile(img):
        with open(img, "rb") as image:
            image.seek(0x200)
            if image.read(4) == b"SKIN":
                packed_layout = "SKIN v1 (3 screens)"
            else:
                image.seek(0x100)
                if image.read(4) == b"CNTL":
                    packed_layout = "CNTL v1 (countdown)"
            image.seek(0x110008)
            packed_sprites = str(int.from_bytes(image.read(4), "little"))
    lines = [
        "镜像布局    : %s" % packed_layout,
        "镜像精灵数  : %s" % packed_sprites,
        "风格名      : %s" % style,
        "素材目录    : %s" % src,
        "生成时间    : %s" % time.strftime("%Y-%m-%d %H:%M:%S"),
        "素材快照    : %d 个 png/gif" % n,
        "精灵表      : %s" % spr_n,
        "",
        "文件大小：",
    ]
    for name, path in (("flash_image_px.bin", img), ("px_flash/px_bg0.bin", os.path.join(fl, "px_bg0.bin")),
                       ("px_flash/px_font.bin", os.path.join(fl, "px_font.bin")),
                       ("px_flash/px_spr.bin", spr)):
        if os.path.isfile(path):
            lines.append("  %-22s %9d B   sha256=%s" % (name, os.path.getsize(path), sha256(path)[:16]))
    open(os.path.join(out, "build_info.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")

    print("\n" + "=" * 62)
    print("  完成！产物包：")
    print("  %s" % out)
    if os.path.isfile(img):
        print("  镜像：%d B (%.2f MB)  sha256=%s" %
              (os.path.getsize(img), os.path.getsize(img) / 1048576.0, sha256(img)[:16]))
    print("=" * 62)
    print("\n  把这个产物包整个交给固件方即可（镜像 + 素材快照 + build_info）。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

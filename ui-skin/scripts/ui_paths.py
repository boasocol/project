# -*- coding: utf-8 -*-
"""构建脚本的统一路径配置 —— 换机器 / 换设计稿 / 换字体只改这里（或建 ui_paths_local.py）。

优先级：**环境变量** > ui_paths_local.py（同目录，可选） > 本文件默认值。
（环境变量优先，是为了让 `pack_style.py` 能一次喂给多个子进程不同风格，
 不会被包里残留的 ui_paths_local.py 抢走——2026-11-18 改）

四个构建脚本（build_px_flash / build_px_alm_digits / build_px_font / build_px_image）
都从这里取路径，脚本里不要再写死绝对路径。

    SRC       设计稿目录（3 张空白 + 3 张全显 + 8 个 GIF）
    FONT_DIR  思源黑体 otf 所在目录（需要 SourceHanSansSC-Medium.otf / -Bold.otf）
    DIG       数字精灵 png 目录（px_digits）
    FL        中间产物目录（px_bg*.bin / px_spr.bin / px_font.bin，默认 px_flash）
    IMG_OUT   最终 2MB 镜像输出路径（flash_image_px.bin）
"""
import os

BASE = os.path.dirname(os.path.abspath(__file__))   # ui-skin/scripts
ROOT = os.path.dirname(BASE)                        # ui-skin（本子树根，仓库内）

# ---------------- 默认值（本仓库 ui-skin/ 布局；换机器改这里或设环境变量） ----------------
_DEF_SRC      = os.path.join(ROOT, "skins", "sample08")    # 默认素材 = 08 号示例（基线）
_DEF_FONT_DIR = os.path.join(ROOT, "assets", "fonts")      # 思源黑体（不入库，见 assets/fonts/README.md）

# ---------------- 可选本地覆盖 ----------------
_LOCAL = {}
try:
    import ui_paths_local as _m           # noqa: N813  同目录的本地配置
    _LOCAL = {k: getattr(_m, k) for k in dir(_m) if k.isupper()}
except Exception:
    pass


def _pick(key, env, default):
    return os.environ.get(env) or _LOCAL.get(key) or default


SRC      = _pick("SRC",      "UI_SRC",      _DEF_SRC)
FONT_DIR = _pick("FONT_DIR", "UI_FONT_DIR", _DEF_FONT_DIR)
DIG      = _pick("DIG",      "UI_DIG",      os.path.join(BASE, "px_digits"))
FL       = _pick("FL",       "UI_FL",       os.path.join(BASE, "px_flash"))
IMG_OUT  = _pick("IMG_OUT",  "UI_IMG",      os.path.join(BASE, "flash_image_px.bin"))
# 素材清单（可选）：素材目录里的 _清单.txt，用来显式指定背景/GIF 文件名
MANIFEST = _pick("MANIFEST", "UI_MANIFEST", os.path.join(SRC, "_清单.txt"))


def require_src():
    """设计稿目录不存在时直接给出可操作的提示，别让脚本抛一堆 FileNotFound。"""
    if not os.path.isdir(SRC):
        raise SystemExit(
            "找不到设计稿目录：\n  SRC = %s\n"
            "请任选一种方式指定：\n"
            "  1) 编辑同目录 ui_paths_local.py 里的 SRC\n"
            "  2) 设环境变量 UI_SRC\n"
            "  3) 改 ui_paths.py 里的 _DEF_SRC" % SRC)


def require_font():
    for w in ("Medium", "Bold"):
        p = os.path.join(FONT_DIR, "SourceHanSansSC-%s.otf" % w)
        if not os.path.isfile(p):
            raise SystemExit(
                "找不到字体文件：\n  %s\n"
                "请编辑 ui_paths_local.py 的 FONT_DIR，或设环境变量 UI_FONT_DIR" % p)


def ensure_dirs():
    for d in (DIG, FL, os.path.dirname(IMG_OUT) or "."):
        if d:
            os.makedirs(d, exist_ok=True)

# -*- coding: utf-8 -*-
"""交付包内的默认路径（只给「直接跑某个脚本」当兜底）。

正常用 一键打包.bat / scripts\pack_style.py，它会用**环境变量**指定
素材目录与产物目录（环境变量优先级高于本文件），本文件不用改。

只有想「不通过 bat、直接 python scripts\build_px_flash.py」时才需要改这里。
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 交付包根目录

DIG      = os.path.join(ROOT, "assets", "px_digits")
FONT_DIR = os.path.join(ROOT, "assets", "fonts")
# 不设 SRC / FL / IMG_OUT：它们由 pack_style.py 用环境变量提供；
# 直接跑脚本时会报「找不到设计稿目录」并提示怎么指定。

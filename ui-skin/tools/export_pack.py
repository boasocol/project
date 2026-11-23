# -*- coding: utf-8 -*-
"""把 ui-skin/ 导出成发给美术的「UI 换肤交付包」。

仓库里的 ui-skin/ 是唯一来源；交付包只是它的发行副本，**不要直接改交付包**——
改完仓库里的东西后跑一次本脚本，交付包就同步了。

    python ui-skin/tools/export_pack.py [交付包目录]
    默认交付包目录 = 仓库上两级（HT\\7）下的 UI换肤交付包

导出内容：scripts/、两份布局模板 + countdown_layout.example.json、三份文档、
assets/px_digits、assets/fonts（只补缺失的，不覆盖）、示例素材_08号设计、一键打包.bat。
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKIN = os.path.dirname(HERE)                       # ui-skin
REPO = os.path.dirname(os.path.dirname(SKIN))      # HT32L52231_QFN32
WORKSPACE = os.path.dirname(REPO)                  # HT\7
DEFAULT_PACK = os.path.join(WORKSPACE, "UI换肤交付包")


def copy_tree(src, dst):
    if not os.path.isdir(src):
        print("  ! 缺 %s，跳过" % src)
        return 0
    shutil.copytree(src, dst, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    return sum(len(f) for _, _, f in os.walk(src))


def copy_file(src, dst):
    if not os.path.isfile(src):
        print("  ! 缺 %s，跳过" % src)
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def copy_missing(src, dst):
    """字体这类大文件：只补缺失的，避免每次导出都拷 90 MB。"""
    if not os.path.isdir(src):
        print("  ! 缺 %s，跳过" % src)
        return 0
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in os.listdir(src):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isfile(s) and not os.path.isfile(d):
            shutil.copy2(s, d)
            n += 1
    return n


def main(argv):
    pack = os.path.abspath(argv[1]) if len(argv) > 1 else DEFAULT_PACK
    os.makedirs(pack, exist_ok=True)
    print("源：%s\n目标：%s\n" % (SKIN, pack))

    n = copy_tree(os.path.join(SKIN, "scripts"), os.path.join(pack, "scripts"))
    print("  scripts/            %d 个文件" % n)
    for name in ("ui_layout.guofeng.json", "ui_layout.circus.json", "countdown_layout.example.json"):
        copy_file(os.path.join(SKIN, "layouts", name), os.path.join(pack, name))
    print("  布局模板            3 个")
    for name in os.listdir(os.path.join(SKIN, "docs")):
        copy_file(os.path.join(SKIN, "docs", name), os.path.join(pack, name))
    print("  文档                3 个")
    n = copy_tree(os.path.join(SKIN, "assets", "px_digits"), os.path.join(pack, "assets", "px_digits"))
    print("  assets/px_digits    %d 个文件" % n)
    n = copy_missing(os.path.join(SKIN, "assets", "fonts"), os.path.join(pack, "assets", "fonts"))
    print("  assets/fonts        补了 %d 个（已存在的不动）" % n)
    n = copy_tree(os.path.join(SKIN, "skins", "sample08"), os.path.join(pack, "示例素材_08号设计"))
    print("  示例素材_08号设计    %d 个文件" % n)
    copy_file(os.path.join(SKIN, "tools", "pack_skin.bat"), os.path.join(pack, "一键打包.bat"))
    print("  一键打包.bat        1 个")
    print("\n完成。交付包内容：")
    for name in sorted(os.listdir(pack)):
        print("  " + name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

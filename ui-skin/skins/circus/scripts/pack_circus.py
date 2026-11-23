"""使用原始交付包和本目录的马戏团脚本构建镜像。"""
import argparse
import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("toolkit", type=Path, help="原始 UI 换肤交付包")
    parser.add_argument("assets", type=Path, help="回传包中的素材目录")
    parser.add_argument("output", type=Path, help="产物根目录")
    args = parser.parse_args()
    toolkit = args.toolkit.resolve()
    skin = Path(__file__).resolve().parent
    output = args.output.resolve()          # 必须绝对路径：pack.HERE 会被改成 staging 目录
    entry = toolkit / "scripts" / "pack_style.py"
    spec = importlib.util.spec_from_file_location("pack_style", entry)
    pack = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pack)
    output.mkdir(parents=True, exist_ok=True)
    # staging 用普通目录（不用 tempfile）：TemporaryDirectory/mkdtemp 会把目录权限收紧到 0700，
    # 受限账户/沙箱下随后创建子目录或清理都会 PermissionError（2026-11-23 实测）。
    staging = output / ".staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True, exist_ok=True)
    try:
        scripts = staging / "scripts"
        shutil.copytree(toolkit / "scripts", scripts, ignore=shutil.ignore_patterns("__pycache__"))
        for name in ("build_px_flash.py", "build_px_font.py", "circus_skin.py"):
            shutil.copy2(skin / name, scripts)
        pack.HERE = str(scripts)
        replaced = {"_px08_handdigits3.py", "build_px_alm_digits.py"}
        pack.STEPS = [step for step in pack.STEPS if step[1] not in replaced]
        return pack.main([str(entry), str(args.assets.resolve()), str(output)])
    finally:
        shutil.rmtree(staging, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

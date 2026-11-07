# -*- coding: utf-8 -*-
"""打包脚本：用 PyInstaller 把 GUI 打成 exe（不装 pip 包，直接用 _build/pylib 里解包的 wheel）。

用法示例
--------
    python build_exe.py                                   # 单文件版
    python build_exe.py --onedir --dist dist_portable     # 文件夹版
    python build_exe.py --console --name MP3Debug         # 带控制台，看启动报错用
"""
import argparse
import os
import random
import string
import sys
import tempfile
from pathlib import Path

BUILD = Path(__file__).resolve().parent
ROOT = BUILD.parent
LIB = BUILD / "pylib"
sys.path.insert(0, str(LIB))


def _plain_mkdtemp(suffix=None, prefix=None, dir=None):
    """默认 mkdtemp 建的是私有 DACL 目录，在受限令牌下不可写，这里换成普通目录。"""
    suffix = suffix or ""
    prefix = prefix or "tmp"
    base = dir or tempfile.gettempdir()
    os.makedirs(base, exist_ok=True)
    for _ in range(50):
        name = prefix + "".join(random.choices(string.ascii_lowercase + string.digits, k=10)) + suffix
        path = os.path.join(base, name)
        try:
            os.makedirs(path)
            return path
        except FileExistsError:
            continue
    raise FileExistsError("无法创建临时目录")


tempfile.mkdtemp = _plain_mkdtemp

from PyInstaller.__main__ import run  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="MP3随机数字改名器")
    ap.add_argument("--dist", default="dist")
    ap.add_argument("--work", default="work")
    ap.add_argument("--spec", default="spec")
    ap.add_argument("--onedir", action="store_true")
    ap.add_argument("--console", action="store_true")
    ap.add_argument("--icon", default=str(BUILD / "app.ico"))
    ap.add_argument("--version-file", default=str(BUILD / "version.txt"))
    ap.add_argument("--runtime-tmpdir", default=None,
                    help="指定 exe 自我解包的目录（默认用系统临时目录）")
    args = ap.parse_args()

    cmd = [
        str(ROOT / "mp3_renamer_gui.py"),
        "--name", args.name,
        "--paths", str(ROOT),
        "--distpath", str(BUILD / args.dist),
        "--workpath", str(BUILD / args.work),
        "--specpath", str(BUILD / args.spec),
        "--noconfirm",
        "--clean",
        "--log-level", "WARN",
        "--windowed" if not args.console else "--console",
        "--onedir" if args.onedir else "--onefile",
    ]
    if os.path.exists(args.icon) and not args.console:
        cmd += ["--icon", args.icon]
    if os.path.exists(args.version_file) and not args.console:
        cmd += ["--version-file", args.version_file]
    if args.runtime_tmpdir:
        cmd += ["--runtime-tmpdir", args.runtime_tmpdir]

    print("PyInstaller:", " ".join(cmd))
    run(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())

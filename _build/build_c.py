# -*- coding: utf-8 -*-
"""一键构建 C 版的三个平台产物（Windows / Linux / Android-ARM64）。

需要先解压 Zig 到 _toolchain 目录（或自己装 zig 并改 ZIG 路径）：

    https://ziglang.org/download/   → zig-x86_64-windows-<ver>.zip

用法：
    python _build/build_c.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(__file__).resolve().parent
ZIG = ROOT / "_toolchain" / "zig-x86_64-windows-0.17.1" / "zig.exe"


def run(cmd: list[str]) -> None:
    print("$", " ".join(str(c) for c in cmd))
    env = dict(os.environ)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / "_toolchain" / "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / "_toolchain" / "zig-local")
    r = subprocess.run([str(c) for c in cmd], env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)


def main() -> None:
    if not ZIG.exists():
        sys.exit(f"找不到 zig：{ZIG}")

    # 1) 生成资源（webui 头文件 + 图标 .res）
    run([sys.executable, str(BUILD / "embed_webui.py")])
    run([sys.executable, str(BUILD / "make_res.py")])
    RES = BUILD / "app.res"

    # 2) 命令行版：三个平台（Windows 版带图标）
    run([ZIG, "cc", "-O2", "-std=c11", "-o", ROOT / "mp3rn.exe",
         ROOT / "mp3rn.c", RES, "-lshell32"])
    run([ZIG, "cc", "-O2", "-std=c11", "-target", "x86_64-linux-musl", "-static",
         "-o", ROOT / "mp3rn-linux-x64", ROOT / "mp3rn.c"])
    run([ZIG, "cc", "-O2", "-std=c11", "-target", "aarch64-linux-musl", "-static",
         "-o", ROOT / "mp3rn-android-arm64", ROOT / "mp3rn.c"])

    # 3) 图形界面版：三平台通用的本地网页界面
    run([ZIG, "cc", "-O2", "-std=c11", "-o", ROOT / "mp3rn-gui.exe",
         ROOT / "mp3rn_gui.c", RES, "-lshell32", "-lws2_32"])
    run([ZIG, "cc", "-O2", "-std=c11", "-target", "x86_64-linux-musl", "-static",
         "-pthread", "-o", ROOT / "mp3rn-gui-linux-x64", ROOT / "mp3rn_gui.c"])
    run([ZIG, "cc", "-O2", "-std=c11", "-target", "aarch64-linux-musl", "-static",
         "-pthread", "-o", ROOT / "mp3rn-gui-android-arm64", ROOT / "mp3rn_gui.c"])

    # 4) 原生 Win32 界面（仅 Windows）
    #    --subsystem,windows 让 exe 不带控制台；入口仍是 main，所以要显式指定 mainCRTStartup
    run([ZIG, "cc", "-O2", "-std=c11",
         "-Wl,--subsystem,windows", "-Wl,-e,mainCRTStartup",
         "-o", ROOT / "mp3rn-winningui.exe",
         ROOT / "mp3rn_win32.c", RES,
         "-lshell32", "-lole32", "-lcomctl32", "-luuid", "-lgdi32", "-luser32"])

    for name in ("mp3rn.exe", "mp3rn-linux-x64", "mp3rn-android-arm64",
                 "mp3rn-gui.exe", "mp3rn-gui-linux-x64", "mp3rn-gui-android-arm64",
                 "mp3rn-winningui.exe"):
        p = ROOT / name
        if p.exists():
            print(f"  {name:28} {p.stat().st_size // 1024:>6} KB")


if __name__ == "__main__":
    main()

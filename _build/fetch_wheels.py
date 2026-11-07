# -*- coding: utf-8 -*-
"""把打包要用的 wheel 直接下载并解包到 _build/pylib（不经过 pip，避免其私有临时目录）。"""
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

BUILD = Path(__file__).resolve().parent
DEST = BUILD / "dl"
LIB = BUILD / "pylib"

PACKAGES = [
    "pyinstaller",
    "pyinstaller-hooks-contrib",
    "altgraph",
    "packaging",
    "pefile",
    "pywin32-ctypes",
    "setuptools",
]


def pick_wheel(files: list[dict]) -> dict | None:
    """优先 win_amd64 轮子，其次纯 python 轮子。"""
    best: list[tuple[int, dict]] = []
    for f in files:
        name = f["filename"]
        if not name.endswith(".whl"):
            continue
        if any(tag in name for tag in ("manylinux", "musllinux", "macosx")):
            continue
        best.append((0 if "win_amd64" in name else 1, f))
    if not best:
        return None
    best.sort(key=lambda item: item[0])
    return best[0][1]


def download(url: str, dest: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "dsh-build/1.0"})
    with urllib.request.urlopen(req, timeout=90) as resp:
        data = resp.read()
    dest.write_bytes(data)


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    LIB.mkdir(parents=True, exist_ok=True)

    for pkg in PACKAGES:
        meta_url = f"https://pypi.org/pypi/{pkg}/json"
        with urllib.request.urlopen(meta_url, timeout=90) as resp:
            meta = json.load(resp)
        wheel = pick_wheel(meta["urls"])
        if wheel is None:
            print(f"[FAIL] {pkg}: 没有可用的 wheel")
            return 1

        target = DEST / wheel["filename"]
        if not target.exists():
            download(wheel["url"], target)
        print(f"[ok] {pkg} {meta['info']['version']} -> {wheel['filename']} "
              f"({target.stat().st_size // 1024} KB)")

        with zipfile.ZipFile(target) as zf:
            zf.extractall(LIB)

    print(f"\n解包完成：{LIB}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

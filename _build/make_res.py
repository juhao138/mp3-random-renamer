# -*- coding: utf-8 -*-
"""把 app.ico 打包成 Windows 的 .res 资源文件（不需要 windres / llvm-rc）。

关键点：.res 开头必须有那个 32 字节的「空条目」，lld 靠它识别文件类型；
后面才是 RT_ICON(3) 各尺寸 + RT_GROUP_ICON(14)。

    python _build/make_res.py
    zig cc -O2 -std=c11 -o mp3rn-win32.exe mp3rn_win32.c _build/app.res ...
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ICO = ROOT / "_build" / "app.ico"
OUT = ROOT / "_build" / "app.res"

RT_ICON = 3
RT_GROUP_ICON = 14
MEMFLAGS = 0x1030
LANG = 0x0409


def read_ico(path: Path):
    data = path.read_bytes()
    reserved, type_, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or type_ != 1:
        raise SystemExit("不是合法的 .ico")
    items = []
    for i in range(count):
        off = 6 + i * 16
        w, h, colors, _r, planes, bits, size, offset = struct.unpack_from("<BBBBHHII", data, off)
        items.append(dict(w=w, h=h, colors=colors, planes=planes, bits=bits,
                          size=size, image=data[offset:offset + size]))
    return items


def entry(payload: bytes, type_id: int, name_id: int) -> bytes:
    t = struct.pack("<HH", 0xFFFF, type_id)
    n = struct.pack("<HH", 0xFFFF, name_id)
    body = (struct.pack("<II", len(payload), 8 + len(t) + len(n) + 16)
            + t + n
            + struct.pack("<IHHII", 0, MEMFLAGS, LANG, 0, 0)
            + payload)
    if len(body) % 4:
        body += b"\x00" * (4 - len(body) % 4)
    return body


def main() -> None:
    icons = read_ico(ICO)
    out = bytearray()

    # 空条目（32 字节）：lld 用它判断这是 .res 文件
    out += (struct.pack("<II", 0, 0x20)
            + struct.pack("<HH", 0xFFFF, 0) * 2
            + struct.pack("<IHHII", 0, MEMFLAGS, 0, 0, 0))

    for idx, it in enumerate(icons, start=1):
        out += entry(it["image"], RT_ICON, idx)

    group = bytearray(struct.pack("<HHH", 0, 1, len(icons)))
    for idx, it in enumerate(icons, start=1):
        group += struct.pack("<BBBBHHIH", it["w"], it["h"], it["colors"], 0,
                             it["planes"], it["bits"], it["size"], idx)
    out += entry(bytes(group), RT_GROUP_ICON, 1)

    OUT.write_bytes(bytes(out))
    print(f"{ICO.name} -> {OUT.name}  ({len(icons)} 个尺寸, {len(out)} 字节)")


if __name__ == "__main__":
    main()

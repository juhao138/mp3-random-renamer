# -*- coding: utf-8 -*-
"""把 webui.html 转成一个 C 头文件（字节数组），避免 C 字符串转义和编码问题。"""
from pathlib import Path

BUILD = Path(__file__).resolve().parent
ROOT = BUILD.parent
SRC = ROOT / "webui.html"
DST = ROOT / "webui_html.h"


def main() -> None:
    data = SRC.read_bytes()
    lines = [
        "/* 自动生成，勿手改：由 _build/build_gui.py 从 webui.html 生成 */",
        "#ifndef WEBUI_HTML_H",
        "#define WEBUI_HTML_H",
        "",
        f"static const unsigned char WEBUI_HTML[] = {{",
    ]
    per = 16
    for i in range(0, len(data), per):
        chunk = data[i:i + per]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append(f"static const unsigned int WEBUI_HTML_LEN = {len(data)}u;")
    lines.append("#endif")
    DST.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{SRC.name} -> {DST.name} ({len(data)} 字节)")


if __name__ == "__main__":
    main()

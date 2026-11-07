# -*- coding: utf-8 -*-
"""生成程序图标（需要一个带 Pillow 的 Python）。输出 _build/app.ico。"""
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent / "app.ico"
S = 1024          # 先大图再缩小，边缘更干净
SS = 4            # 超采样倍数


def rr(draw, box, radius, fill):
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def main() -> None:
    size = S * SS
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    pad = int(size * 0.02)
    # 底色：深蓝圆角方块
    rr(d, (pad, pad, size - pad, size - pad), int(size * 0.22), (31, 111, 235, 255))
    # 内部高光，增加层次
    rr(d, (pad * 2, pad * 2, size - pad * 2, size - pad * 2), int(size * 0.19), (43, 128, 255, 255))

    white = (255, 255, 255, 255)

    def P(x: float, y: float) -> tuple[int, int]:
        return int(size * x), int(size * y)

    # 符头（两个椭圆）
    r = size * 0.075
    for cx, cy in ((0.345, 0.700), (0.680, 0.775)):
        x, y = P(cx, cy)
        d.ellipse((x - r, y - r * 0.78, x + r, y + r * 0.78), fill=white)

    # 符干
    w = size * 0.030
    for x, y_top, y_bot in ((0.402, 0.290, 0.700), (0.737, 0.365, 0.775)):
        x0, y0 = P(x, y_top)
        x1, y1 = P(x, y_bot)
        d.rectangle((x0, y0, x0 + w, y1), fill=white)

    # 符梁（两条）
    for top, thick in ((0.290, 0.105), (0.470, 0.075)):
        p1 = P(0.402, top)
        p2 = P(0.737 + 0.030, top + 0.075)
        p3 = P(0.737 + 0.030, top + 0.075 + thick)
        p4 = P(0.402, top + thick)
        d.polygon([p1, p2, p3, p4], fill=white)

    icon = img.resize((S, S), Image.LANCZOS)
    icon.save(OUT, format="ICO",
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    # 顺带存一张 png 方便预览
    icon.resize((256, 256), Image.LANCZOS).save(OUT.with_suffix(".png"))
    print(f"图标已生成：{OUT}")


if __name__ == "__main__":
    main()

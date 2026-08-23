"""Генерує capslang.ico за мотивами референс-зображення:
скруглений квадрат з темною рамкою, чорні "Q" і "Ї", сині swap-стрілки між ними.
Рендер у 1024px, даунскейл у стандартні розміри ICO.
"""
import math
from PIL import Image, ImageDraw, ImageFont

S = 1024  # робочий канвас

DARK = (30, 33, 38, 255)      # рамка
PAPER = (247, 248, 250, 255)  # фон плитки
INK = (23, 25, 29, 255)       # літери
BLUE = (26, 109, 255, 255)    # стрілки

FONT_PATH = r"C:\Windows\Fonts\arialbd.ttf"


def rounded_tile(d: ImageDraw.ImageDraw):
    # зовнішня темна рамка
    d.rounded_rectangle([70, 70, S - 70, S - 70], radius=210, fill=DARK)
    # внутрішня світла плитка
    d.rounded_rectangle([112, 112, S - 112, S - 112], radius=172, fill=PAPER)


def draw_glyphs(d: ImageDraw.ImageDraw):
    font = ImageFont.truetype(FONT_PATH, 370)
    # Q ліворуч, Ї праворуч; anchor "mm" = центр гліфа
    d.text((262, 520), "Q", font=font, fill=INK, anchor="mm")
    d.text((778, 520), "Ї", font=font, fill=INK, anchor="mm")


def make_arrow_layer():
    """Одна стрілка: пласка дуга вигином угору, наконечник праворуч.
    Друга стрілка = поворот цього шару на 180°."""
    W, H = 264, 150
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    lw = 48
    box = (6, 30, W - 26, H + 150)  # низ еліпса за межами шару — лишається пласка верхня дуга
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    rx, ry = (x1 - x0) / 2, (y1 - y0) / 2

    start, end = 205, 322
    d.arc(box, start=start, end=end, fill=BLUE, width=lw)

    # Наконечник на кінці дуги, напрям — дотична, підмішана до горизонталі
    a = math.radians(end)
    tip = (cx + rx * math.cos(a), cy + ry * math.sin(a))
    tx, ty = -rx * math.sin(a), ry * math.cos(a)
    n = math.hypot(tx, ty)
    dx, dy = tx / n + 1.7, ty / n  # тягнемо напрям до "праворуч"
    n = math.hypot(dx, dy)
    dx, dy = dx / n, dy / n

    size = 84
    px, py = -dy, dx
    bx, by = tip[0] - dx * size, tip[1] - dy * size
    w = size * 0.6
    d.polygon(
        [tip, (bx + px * w, by + py * w), (bx - px * w, by - py * w)],
        fill=BLUE,
    )
    return layer


def draw_arrows(img: Image.Image):
    arrow = make_arrow_layer()
    img.alpha_composite(arrow, (392, 382))                 # верхня, вістря праворуч
    img.alpha_composite(arrow.rotate(180), (392, 512))     # нижня, вістря ліворуч


def main():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rounded_tile(d)
    draw_glyphs(d)
    draw_arrows(img)

    img.save("preview.png")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    img.resize((256, 256), Image.LANCZOS).save(
        "capslang.ico", sizes=[(s, s) for s in sizes]
    )
    print("OK: preview.png + capslang.ico")


if __name__ == "__main__":
    main()

"""马戏团皮肤的精灵规格和设计坐标（y=0 在顶部）。"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


DIGITS = {
    "main": ((29, 43), (12, 43), (95, 125, 154, 167, 197), 34),
    "cnt": ((32, 47), (12, 47), (49, 82, 115, 128, 161), 40),
    "alm": ((25, 37), (10, 37), (35, 61, 87, 98, 124), 49),
}
ANIMATIONS = {
    "main": ("时间界面", (146, 111), (67, 62)),
    "cnt": ("倒计时界面", (235, 127), (60, 55)),
    "alm": ("闹钟界面", (225, 153), (63, 60)),
}
DARK = (98, 36, 15)
LIGHT = (255, 248, 219)
PROGRESS = ((241, 44, 36), (255, 197, 27), (53, 190, 241), (121, 201, 24))
COUNTDOWN_TEXT = "主人，准备好开场啦"
ALARM_TEXT = "主人，需要设置闹钟吗？"
WEEKDAY_X = (26, 67, 108, 149, 190, 231, 272)
WEEKDAY_Y = 143
SWITCH_POSITION = (249, 64)


def font(directory, size, weight="Bold"):
    return ImageFont.truetype(str(Path(directory) / f"SourceHanSansSC-{weight}.otf"), size)


def digit_sprite(style, char, typeface):
    standard, colon, _, _ = DIGITS[style]
    width, height = colon if char == ":" else standard
    glyph = Image.new("RGBA", (120, 120))
    if char == ":":
        draw = ImageDraw.Draw(glyph)
        for top in (40, 69):
            draw.ellipse((51, top, 69, top + 18), fill="#f78526", outline="#692810", width=3)
        glyph = glyph.crop((45, 15, 76, 112))
        glyph = glyph.resize((width - 2, height - 2), Image.Resampling.LANCZOS)
    else:
        draw = ImageDraw.Draw(glyph)
        draw.text((60, 50), char, anchor="mm", font=typeface, fill="#fa7d22",
                  stroke_width=3, stroke_fill="#682710")
        glyph = glyph.crop(glyph.getbbox())
        glyph.thumbnail((width - 2, height - 2), Image.Resampling.LANCZOS)
    sprite = Image.new("RGBA", (width, height))
    sprite.paste(glyph, ((width - glyph.width) // 2, (height - glyph.height) // 2))
    alpha = sprite.getchannel("A").point(lambda value: 255 if value >= 128 else 0)
    sprite.putalpha(alpha)
    return sprite

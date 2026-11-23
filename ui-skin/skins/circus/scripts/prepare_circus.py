"""由原始马戏团稿生成标准命名素材和同底全显稿。"""
import argparse
from pathlib import Path

from PIL import GifImagePlugin, Image, ImageDraw, ImageSequence

from circus_skin import (
    ANIMATIONS, DARK, DIGITS, LIGHT, PROGRESS, SWITCH_POSITION, WEEKDAY_X, WEEKDAY_Y,
    digit_sprite, font,
)


SCREENS = ("时间界面", "倒计时界面", "闹钟界面")
FULL_NAMES = ("时间界面（全显）", "倒计时界面（计时）", "闹钟界面（打开状态）")


def transparent_palette(frame):
    indexed = frame.convert("RGB").quantize(colors=255)
    mask = frame.getchannel("A").point(lambda value: 255 if value < 128 else 0)
    indexed.paste(255, mask=mask)
    indexed.info["transparency"] = 255
    return indexed


def save_full_frame_gif(path, frames):
    header, _ = GifImagePlugin.getheader(frames[0], info={"loop": 0})
    with path.open("wb") as output:
        output.writelines(header)
        for index, frame in enumerate(frames):
            output.writelines(GifImagePlugin.getdata(
                frame, duration=120 + 10 * (index % 2), disposal=2,
                transparency=255, include_color_table=True,
            ))
        output.write(b";")


def prepare_animations(source, output):
    mapping = []
    for style, (screen, position, target_size) in ANIMATIONS.items():
        states = ("休眠", "唤醒") if style == "main" else ("休眠", "唤醒", "接收指令")
        sequences = []
        for state in states:
            with Image.open(source / f"{screen}_{state}.gif") as animation:
                sequences.append([frame.convert("RGBA") for frame in ImageSequence.Iterator(animation)])
        boxes = [frame.getbbox() for sequence in sequences for frame in sequence]
        union = tuple(aggregate(box[i] for box in boxes)
                      for i, aggregate in enumerate((min, min, max, max)))
        for index, (state, sequence) in enumerate(zip(states, sequences)):
            name = f"{screen}_{state.replace('接收', '接受')}.gif"
            frames = []
            for frame in sequence:
                resized = frame.crop(union).resize(target_size, Image.Resampling.LANCZOS)
                canvas = Image.new("RGBA", (320, 240))
                canvas.paste(resized, position)
                frames.append(transparent_palette(canvas))
            save_full_frame_gif(output / name, frames)
            mapping.append(f"{style}{index} = {name}")
    return mapping


def control_assets(background, output):
    x, y = SWITCH_POSITION
    off = background.crop((x, y, x + 48, y + 24))
    off.save(output / "开关_关.png")
    on = off.copy()
    draw = ImageDraw.Draw(on)
    draw.rounded_rectangle((1, 2, 46, 21), radius=10, fill="#208bf1", outline="#005db6")
    draw.ellipse((27, 3, 44, 20), fill="#ffffff", outline="#d4ecff")
    on.save(output / "开关_开.png")
    for label, color in (("亮", "#d94828"), ("灭", "#d4b48a")):
        sprite = Image.new("RGBA", (22, 4))
        ImageDraw.Draw(sprite).rounded_rectangle((0, 0, 21, 3), radius=2, fill=color)
        sprite.save(output / f"星期钮_{label}.png")


def put_text(draw, at, text, face, fill=DARK, center=False):
    left, top, right, _ = draw.textbbox((0, 0), text, font=face)
    x, y = at
    if center:
        x -= (right - left) // 2
    draw.text((x - left, y - top), text, font=face, fill=fill)


def preview(screen, style, sample, output, faces):
    image = Image.open(output / f"{screen}.png").convert("RGBA")
    draw = ImageDraw.Draw(image)
    _, _, xs, y = DIGITS[style]
    for x, char in zip(xs, sample):
        image.alpha_composite(digit_sprite(style, char, faces[80]), (x, y))
    with Image.open(output / f"{screen}_休眠.gif") as animation:
        image.alpha_composite(animation.convert("RGBA"))
    if style == "main":
        for at, words, size in (((13, 132), "2026.11.19", 12), ((77, 132), "星期四", 12),
                                 ((23, 151), "农历十月初一", 12), ((261, 106), "24℃", 14),
                                 ((285, 147), "52%", 14)):
            put_text(draw, at, words, faces[size], center=size == 14)
        draw.rounded_rectangle((172, 198, 219, 217), radius=9, fill="#2ca13c", outline="#267824")
        draw.ellipse((201, 200, 217, 216), fill="white")
    elif style == "cnt":
        for point, color in zip((44, 59, 74, 89), PROGRESS):
            draw.ellipse((211, point - 6, 224, point + 6), fill=color)
        put_text(draw, (127, 96), "倒计时中", faces[13], LIGHT, True)
        for center, words in ((60, "开始"), (157, "暂停"), (259, "重置")):
            put_text(draw, (center, 202), words, faces[14], LIGHT, True)
        put_text(draw, (275, 91), "主人，", faces[12], center=True)
        put_text(draw, (275, 105), "准备好开场啦", faces[12], center=True)
    else:
        put_text(draw, (220, 78), "1", faces[12])
        with Image.open(output / "开关_开.png") as sprite:
            image.paste(sprite, SWITCH_POSITION)
        for index, x in enumerate(WEEKDAY_X):
            label = "亮" if index < 4 else "灭"
            with Image.open(output / f"星期钮_{label}.png") as sprite:
                image.alpha_composite(sprite, (x, WEEKDAY_Y))
        put_text(draw, (111, 173), "主人，需要", faces[13], center=True)
        put_text(draw, (111, 189), "设置闹钟吗？", faces[13], center=True)
    return image.convert("RGB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("fonts", type=Path)
    args = parser.parse_args()
    args.assets.mkdir(parents=True, exist_ok=True)
    for screen in SCREENS:
        with Image.open(args.source / f"{screen}（抠图）.png") as base:
            base.convert("RGB").save(args.assets / f"{screen}.png")
    mapping = prepare_animations(args.source, args.assets)
    with Image.open(args.assets / "闹钟界面.png") as background:
        control_assets(background, args.assets)
    faces = {size: font(args.fonts, size, "Bold" if size == 80 else "Medium")
             for size in (12, 13, 14, 80)}
    for screen, style, sample, full_name in zip(SCREENS, DIGITS, ("19:40", "10:00", "19:40"), FULL_NAMES):
        preview(screen, style, sample, args.assets, faces).save(args.assets / f"{full_name}.png")
    lines = [f"bg{i} = {screen}.png" for i, screen in enumerate(SCREENS)]
    lines.extend((
        f"bg0_full = {FULL_NAMES[0]}.png", "sec_patch = none", *mapping,
        "sw_on = 开关_开.png", "sw_off = 开关_关.png",
        "tg_on = 星期钮_亮.png", "tg_off = 星期钮_灭.png",
    ))
    (args.assets / "_清单.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

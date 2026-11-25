"""Compile the optional countdown_layout.json into the firmware's CNTL v1 block."""

import json
import struct
import zlib

LAYOUT_FILE = "countdown_layout.json"
OFFSET = 0x100
DIGIT_IDS = range(150, 160)
COLON_ID = 160


def integers(values, count):
    if len(values) != count or any(type(v) is not int or not 0 <= v <= 65535 for v in values):
        raise ValueError(f"expected {count} unsigned integers")
    return values


def rect(values):
    x, y, w, h = integers(values, 4)
    if w == 0 or h == 0 or x + w > 320 or y + h > 240:
        raise ValueError(f"rectangle outside 320x240: {values}")
    return values


def rgb565(color):
    if not isinstance(color, str) or len(color) != 7 or not color.startswith("#"):
        raise ValueError(f"expected #RRGGBB: {color!r}")
    rgb = int(color[1:], 16)
    return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F)


class Font:
    def __init__(self, data, on_text_overflow=None):
        if data[:4] != b"FONT":
            raise ValueError("bad FONT container")
        self.data = data
        self.on_text_overflow = on_text_overflow
        self.bits = 4 if data[7] == 4 else 1
        count, offset = struct.unpack_from("<II", data, 11)
        self.glyphs = {}
        for i in range(count):
            code, bitmap, w, h = struct.unpack_from("<IIHH", data, offset + 12*i)
            self.glyphs[code] = bitmap, w, h

    def report_text_overflow(self, message):
        if self.on_text_overflow is None:
            raise ValueError(message)
        self.on_text_overflow(message)

    def measure(self, slot, text):
        width, height = 0, 0
        ink_top, ink_bottom = 240, 0
        for ch in text:
            try:
                bitmap, w, h = self.glyphs[(slot << 16) + ord(ch)]
            except KeyError:
                raise ValueError(f"font slot {slot} is missing {ch!r}") from None
            stride = (w * self.bits + 7) // 8
            pixels = self.data[bitmap:bitmap + h*stride]
            if not stride or len(pixels) != h*stride:
                raise ValueError(f"invalid bitmap for {ch!r}")
            width += w
            height = max(height, h)
            for row in range(h):
                if any(pixels[row*stride:(row + 1)*stride]):
                    ink_top = min(ink_top, h - 1 - row)
                    ink_bottom = max(ink_bottom, h - row)
        return width, height, ink_top, ink_bottom


def label(font, style, text_centers, center=0):
    y, slot = integers([style["y"], style["font"]], 2)
    top, bottom = 240, 0
    for text, anchor in text_centers:
        width, height, text_top, text_bottom = font.measure(slot, text)
        left = anchor - width // 2
        if not width or not text_bottom or left < 0 or left + width > 320 or y + height > 240:
            raise ValueError(f"label outside 320x240: {text!r}")
        top, bottom = min(top, text_top), max(bottom, text_bottom)
    return [center, y, top, bottom - top, slot, rgb565(style["color"])]


def pack_layout(config, sprites, font_data, *, full_skin=False, digit_base=150, colon_id=160,
                on_text_overflow=None):
    if config["version"] != 1:
        raise ValueError("expected countdown layout version 1")
    if sprites[:4] != b"SPR1" or struct.unpack_from("<I", sprites, 8)[0] <= max(colon_id, digit_base+9):
        raise ValueError("countdown digit and colon sprites are required")
    sizes = {struct.unpack_from("<HH", sprites, 16 + 16*i) for i in range(digit_base,digit_base+10)}
    if len(sizes) != 1:
        raise ValueError("countdown digit cells must be the same size")
    (digit_w, digit_h), = sizes
    colon_w, colon_h = struct.unpack_from("<HH", sprites, 16 + 16*colon_id)
    if not (0 < colon_w <= digit_w and 0 < colon_h <= digit_h):
        raise ValueError("invalid countdown digit/colon dimensions")

    digits, progress = config["digits"], config["progress"]
    short_x = integers(digits["short_x"], 5)
    long_x = integers(digits["long_x"], 6)
    digit_y, = integers([digits["y"]], 1)
    dx, dy, dw, dh = rect(digits["clear"])
    for positions in (short_x, long_x):
        if positions != sorted(set(positions)):
            raise ValueError("digit x coordinates must increase")
        if positions[0] < dx or positions[-1] + digit_w > dx + dw:
            raise ValueError("digit positions exceed clear rectangle")
    if digit_y < dy or digit_y + digit_h > dy + dh:
        raise ValueError("digit height exceeds clear rectangle")
    bar = [0]*4 if full_skin and progress["rect"] == [0]*4 else rect(progress["rect"])
    if bar != [0]*4 and bar[2] < 7:
        raise ValueError("progress bar needs at least seven pixels for round ends")

    font = Font(font_data, on_text_overflow=on_text_overflow)
    title = config["title"]
    center, = integers([title["center"]], 1)
    title_data = label(font, title, [("倒计时中", center), ("正计时中", center)], center)
    left_rule, right_rule = title["rules"]
    buttons = config["buttons"]
    centers = integers(buttons["centers"], 3)
    buttons_text = (("开始", "暂停"), ("重置",), ("退出",))
    button_data = label(font, buttons, [(text, x) for labels, x in zip(buttons_text, centers)
                                       for text in labels])

    bubble = config["bubble"]
    x, y, w, slot, small, lh, lines = integers(
        [bubble[key] for key in ("x", "y", "width", "font", "small_font", "line_height", "lines")], 7
    )
    if not 1 <= lines <= 4 or type(bubble["centered"]) is not bool:
        raise ValueError("invalid bubble line count or alignment")
    rect([x, y, w, lh*lines])
    for font_slot in (() if full_skin else {slot, small}):
        if font_slot > 4:
            raise ValueError("font slot must be 0–4")
        for ch in "主人，开启国风倒计时吧！":
            char_w, char_h, _, _ = font.measure(font_slot, ch)
            if char_w > w or char_h > lh:
                font.report_text_overflow(
                    f"countdown bubble glyph {ch!r} (font {font_slot}) exceeds {w}×{lh} px"
                )
    if title_data[4] > 4 or button_data[4] > 4:
        raise ValueError("font slot must be 0–4")

    payload = struct.pack(
        "<55H", digit_y, digit_w, digit_h, *short_x, *long_x, dx, dy, dw, dh,
        *bar, rgb565(progress["color"]), *title_data,
        *([0]*4 if full_skin and left_rule is None else rect(left_rule)),
        *([0]*4 if full_skin and right_rule is None else rect(right_rule)),
        *centers, *button_data, x, y, w, slot, small, lh, lines,
        rgb565(bubble["color"]), int(bubble["centered"])
    )
    return struct.pack("<4sHHI", b"CNTL", 1, len(payload), zlib.crc32(payload)) + payload


def pack_file(path, sprites, font, *, on_text_overflow=None):
    try:
        config = json.loads(path.read_text(encoding="utf-8-sig"))
        return pack_layout(config, sprites, font, on_text_overflow=on_text_overflow)
    except (KeyError, ValueError, TypeError, struct.error) as error:
        raise SystemExit(f"{path}: {error}") from error

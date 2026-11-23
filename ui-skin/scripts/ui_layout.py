"""Compile ui_layout.json and optional named overlays for three-screen firmware."""

import json
import struct
import zlib
from pathlib import Path

import countdown_layout as timer
from countdown_layout import Font, integers, rect, rgb565

LAYOUT_FILE = "ui_layout.json"
OFFSET = 0x200
NONE = 0xFFFF
SEGMENTS = 8
SCREEN_NAMES = ("main", "countdown", "alarm")
ROBOT_STATES = ("idle", "wake", "ring", "ack")
BUTTON_ACTIONS = ("toggle", "reset", "exit")


def u16(value):
    return integers([value], 1)[0]


def flag(value):
    if type(value) is not bool:
        raise ValueError("expected true or false")
    return int(value)


def input_file(directory, path):
    root = directory.resolve()
    result = (root / path).resolve()
    if not result.is_relative_to(root) or not result.is_file():
        raise ValueError(f"missing asset inside source directory: {path}")
    return result


class SpriteTable:
    def __init__(self, raw):
        magic, version, count, reserved = struct.unpack_from("<4sIII", raw)
        if magic != b"SPR1" or version != 1 or not 0 < count <= (len(raw)-16)//16:
            raise ValueError("invalid SPR1 file")
        self.entries = [struct.unpack_from("<HHhhII", raw, 16 + 16*i) for i in range(count)]
        self.start = 16 + 16*count
        self.raw = raw
        self.reserved = reserved
        self.names = {}

    def id(self, ref):
        index = self.names[ref] if isinstance(ref, str) else u16(ref)
        if index >= len(self.entries):
            raise ValueError(f"sprite {ref!r} outside SPR1 table")
        return index

    def group(self, first, count, position=None):
        index = self.id(first)
        if count < 1 or index + count > len(self.entries):
            raise ValueError(f"missing sprite frames from {first!r}")
        for w, h, x, y, _, _ in self.entries[index:index+count]:
            a, b = (x,y) if position is None else position
            rect([a, b, w, h])
        return index

    def common_size(self, first, count):
        if first + count > len(self.entries):
            raise ValueError("sprite group outside SPR1 table")
        sizes = {entry[:2] for entry in self.entries[first:first+count]}
        if len(sizes) != 1:
            raise ValueError("sprite group needs a common cell size")
        return next(iter(sizes))

    def append_overlays(self, directory, overlays):
        if not overlays:
            return self.raw
        from PIL import Image

        shift = 16*len(overlays)
        payload = bytearray(self.raw[self.start:])
        self.entries = [(w,h,x,y,p+shift if p else 0,m+shift if m else 0)
                        for w,h,x,y,p,m in self.entries]
        for overlay in overlays:
            name = overlay["name"]
            if not isinstance(name, str) or not name or name in self.names:
                raise ValueError("overlay name must be nonempty and unique")
            with Image.open(input_file(directory, overlay["file"])) as source:
                image = source.convert("RGBA")
            if "crop" in overlay:
                x, y, w, h = rect(overlay["crop"])
                if x+w > image.width or y+h > image.height:
                    raise ValueError(f"crop outside asset: {name}")
                image = image.crop((x,y,x+w,y+h))
            w,h = image.size
            rect([0,0,w,h])
            pixels, masks = bytearray(), bytearray()
            for row in range(h-1,-1,-1):
                mask = bytearray((w+7)//8)
                for x in range(w):
                    r,g,b,a = image.getpixel((x,row))
                    pixels += struct.pack("<H", ((r>>3)<<11) | ((g>>2)<<5) | (b>>3))
                    if a >= 128:
                        mask[x//8] |= 1 << (7-x%8)
                masks += mask
            po = self.start + shift + len(payload)
            mo = po + len(pixels)
            payload += pixels + masks
            self.names[name] = len(self.entries)
            self.entries.append((w,h,0,0,po,mo))
        return (struct.pack("<4sIII", b"SPR1", 1, len(self.entries), self.reserved) +
                b"".join(struct.pack("<HHhhII", *entry) for entry in self.entries) + payload)


def box_or_empty(value):
    return [0,0,0,0] if value is None else rect(value)


def digit_row(spec, table):
    x = integers(spec["x"], 5)
    y = u16(spec["y"])
    base = table.id(spec["digits"])
    colon = table.id(spec["colon"])
    w,h = table.common_size(base, 10)
    cw,ch = table.entries[colon][:2]
    if x != sorted(set(x)) or ch != h:
        raise ValueError("digit positions must increase and colon height must match digits")
    for i, dx in enumerate(x):
        rect([dx,y,cw if i == 2 else w,h])
    return [*x,y,base,w,h,colon,cw]


def text_slot(spec, font, examples=()):
    if spec is None:
        return [0]*7
    x,y,w,h = rect(spec["box"])
    slot = u16(spec["font"])
    if slot >= 5:
        raise ValueError("font index must be 0–4")
    for sample in examples:
        width,_,_,ink_bottom = font.measure(slot, sample)
        if width > w or ink_bottom > h:
            raise ValueError(f"text {sample!r} exceeds box {spec['box']}")
    return [x,y,w,h,slot,rgb565(spec["color"]),flag(spec.get("centered", False))]


def toggle(spec, table):
    if spec is None:
        return [0,0,0,0,NONE,NONE]
    x,y = integers(spec["at"], 2)
    ids = [NONE if spec.get(key) is None else table.id(spec[key]) for key in ("off","on")]
    dims = {table.entries[i][:2] for i in ids if i != NONE}
    if len(dims) != 1:
        raise ValueError("a toggle needs at least one sprite; both frames need equal dimensions")
    (w,h), = dims
    return [*rect([x,y,w,h]),*ids]


def clock_page(spec, table, font):
    row = digit_row(spec["numbers"], table)
    seconds = spec.get("seconds")
    if seconds is None:
        second_data = [0,0,0,NONE,0,0]
    else:
        x = integers(seconds["x"], 2)
        y = u16(seconds["y"])
        base = table.id(seconds["digits"])
        w,h = table.common_size(base, 10)
        if x[0] >= x[1]:
            raise ValueError("seconds x coordinates must increase")
        for dx in x: rect([dx,y,w,h])
        second_data = [*x,y,base,w,h]
    sep = spec.get("date_separator", "-")
    if sep not in ("-",".","/"):
        raise ValueError("date separator must be -, . or /")
    prefix = flag(spec.get("lunar_prefix", False))
    unit = {"split":"°C", "combined":"℃", "none":""}[spec.get("temperature_unit", "split")]
    text = [
        *text_slot(spec.get("date"), font, (sep.join(("2026","12","31")),)),
        *text_slot(spec.get("weekday"), font, tuple("星期"+day for day in "一二三四五六日")),
        *text_slot(spec.get("lunar"), font, (("农历" if prefix else "")+"腊月廿九",)),
        *text_slot(spec.get("temperature"), font, ("-99"+unit,)),
        *text_slot(spec.get("humidity"), font, ("100%",)),
        *text_slot(spec.get("temperature_label"), font, ("温度",)),
        *text_slot(spec.get("humidity_label"), font, ("湿度",)),
        *text_slot(spec.get("assistant_label"), font, ("AI语音助手",)),
    ]
    return [*row,*second_data,*text,*box_or_empty(spec.get("assistant_rule")),
            ord(sep),prefix,("split","combined","none").index(spec.get("temperature_unit","split")),
            *toggle(spec.get("hourly"),table)]


def alarm_page(spec, table, font):
    slot = spec.get("slot", {})
    if "sprites" in slot:
        start = table.id(slot["sprites"])
        w,h = table.common_size(start,3)
        index = [0]*7 + rect([*integers(slot["at"],2),w,h]) + [start]
    else:
        index = text_slot(slot.get("text"),font,("1","2","3")) + [0,0,0,0,NONE]
    weekdays = spec.get("weekdays", [None]*7)
    if len(weekdays) != 7:
        raise ValueError("weekdays must contain Monday through Sunday")
    days = [value for item in weekdays for value in toggle(item,table)]
    cover = spec.get("switch_blink_cover")
    if cover is None:
        cover_data = [0,0,0,0,NONE]
    else:
        idx = table.id(cover["sprite"])
        w,h = table.entries[idx][:2]
        cover_data = [*rect([*integers(cover["at"],2),w,h]),idx]
    return [*digit_row(spec["numbers"],table),*index,*toggle(spec.get("switch"),table),
            *days,*cover_data]


def countdown_page(spec, table):
    digits = spec["digits"]
    start = table.id(digits["digits"])
    table.common_size(start,10)
    colon = table.id(digits["colon"])
    actions = tuple(spec["buttons"].get("actions", BUTTON_ACTIONS))
    if actions != BUTTON_ACTIONS:
        raise ValueError('countdown buttons must be ["toggle", "reset", "exit"] (开始/暂停、重置、退出)')
    progress = spec["progress"]
    kind = ("bar","segments","hidden").index(progress["type"])
    segments = progress.get("segments", [])
    if not (1 <= len(segments) <= SEGMENTS if kind == 1 else not segments):
        raise ValueError("segmented progress needs 1–8 segments")
    words = []
    for segment in segments:
        r = rect(segment["rect"])
        ellipse = ("rectangle","ellipse").index(segment.get("shape","rectangle"))
        if ellipse and max(r[2:]) > 64:
            raise ValueError("ellipse exceeds 64x64")
        words.extend([*r,rgb565(segment["color"]),ellipse])
    return [start,colon,*range(len(BUTTON_ACTIONS)),kind,len(segments),
            *words,*([0]*(6*(SEGMENTS-len(segments))))]


def bubble(spec, font):
    if spec is None:
        return [0]*9, bytes(64)
    x,y,w = integers(spec["area"],3)
    lh,n = integers([spec["line_height"],spec["lines"]],2)
    slot, fallback = integers([spec["font"],spec.get("small_font",spec["font"])],2)
    if n not in range(1,5) or slot >= 5 or fallback >= 5:
        raise ValueError("bubble needs 1–4 lines and font indexes 0–4")
    rect([x,y,w,lh*n])
    text = spec["default_text"]
    encoded = text.encode("utf-8")
    manual_lines = text.split("\n")
    if len(encoded)>63 or "\x00" in text or "\r" in text or len(manual_lines) > n:
        raise ValueError("bubble text exceeds its line count or 63 UTF-8 bytes")
    for variant in {slot,fallback}:
        if len(manual_lines) > 1 and any(font.measure(variant, line)[0] > w for line in manual_lines):
            raise ValueError("manually wrapped bubble line exceeds its width")
        for ch in text.replace("\n", ""):
            cw,_,top,bottom = font.measure(variant,ch)
            if cw>w or bottom-top>lh:
                raise ValueError(f"bubble glyph does not fit the line: {ch}")
    words = [x,y,w,slot,fallback,lh,n,rgb565(spec["color"]),flag(spec.get("centered",False))]
    return words,encoded.ljust(64,b"\0")


def blocks(config, sprites, font_data, directory):
    if config["version"] != 1:
        raise ValueError("expected ui_layout.json version 1")
    table = SpriteTable(sprites)
    packed_sprites = table.append_overlays(directory, config.get("overlays", []))
    font = Font(font_data)
    pages = [config[name] for name in SCREEN_NAMES]
    main, countdown, alarm = pages
    words = [*clock_page(main,table,font), *alarm_page(alarm,table,font), *countdown_page(countdown,table)]
    payload = struct.pack(f"<{len(words)}H",*words)
    for page in pages:
        values,text = bubble(page.get("bubble"), font)
        payload += struct.pack("<9H",*values)+text
    for page in pages:
        for name in ROBOT_STATES:
            frames = page.get("robot",{}).get(name)
            if frames is None:
                payload += bytes(4)
                continue
            count = u16(frames["count"])
            if not 1 <= count <= 16:
                raise ValueError("robot animation needs 1–16 frames per state")
            payload += struct.pack("<HH",table.group(frames["first"],count),count)
    if len(payload) != 728:
        raise ValueError(f"SKIN wire size mismatch: {len(payload)}")
    skin = struct.pack("<4sHHI",b"SKIN",1,len(payload),zlib.crc32(payload)) + payload

    # The countdown geometry retains the CNTL v1 wire format.
    geom = dict(countdown, version=1)
    geom["progress"] = (countdown["progress"] if countdown["progress"]["type"]=="bar"
                        else {"rect":[0,0,0,0],"color":"#000000"})
    geom["bubble"] = {"x":0,"y":0,"width":1,"font":0,"small_font":0,
                      "line_height":1,"lines":1,"color":"#000000","centered":False}
    countdown_blob = timer.pack_layout(
        geom, packed_sprites, font_data, full_skin=True,
        digit_base=table.id(countdown["digits"]["digits"]), colon_id=table.id(countdown["digits"]["colon"]),
    )
    return packed_sprites, countdown_blob, skin


def font_texts(directory):
    """Characters introduced by this profile, before the FONT atlas is generated."""
    path = Path(directory) / LAYOUT_FILE
    if not path.is_file():
        return ()
    config = json.loads(path.read_text(encoding="utf-8-sig"))
    main = config["main"]
    text = [main.get("date_separator", "-")]
    if main.get("lunar_prefix"): text.append("农历")
    if main.get("temperature"): text.append({"split":"°C","combined":"℃","none":""}[main.get("temperature_unit","split")])
    for page in SCREEN_NAMES:
        spec = config[page].get("bubble")
        if spec: text.append(spec["default_text"].replace("\n", ""))
    return text


def pack_file(path, sprites, font):
    try:
        config = json.loads(path.read_text(encoding="utf-8-sig"))
        return blocks(config, sprites, font, path.parent)
    except (KeyError, ValueError, TypeError, struct.error, OSError) as error:
        raise SystemExit(f"{path}: {error}") from error

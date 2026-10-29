#!/usr/bin/env python3
"""Render an asciinema v2 cast to an animated GIF (pyte terminal emulation, PIL drawing).

Usage: render_cast.py <cast> <gif>   (needs: pip install pyte pillow)
"""
import json, sys
import pyte
from PIL import Image, ImageDraw, ImageFont

cast, out = sys.argv[1], sys.argv[2]
lines = open(cast).read().splitlines()
hdr = json.loads(lines[0]); W, H = hdr["width"], hdr["height"]
events = [json.loads(l) for l in lines[1:]]
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13)
BOLD = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 13)
cw, ch = 8, 16
PAL = {"default": (220, 220, 220), "black": (40, 40, 40), "red": (230, 90, 90), "green": (110, 210, 110),
       "brown": (230, 200, 90), "yellow": (240, 210, 100), "blue": (110, 150, 240), "magenta": (200, 120, 220),
       "cyan": (100, 200, 210), "white": (240, 240, 240)}
BG = (24, 26, 30)
screen = pyte.Screen(W, H); stream = pyte.Stream(screen)

def draw():
    img = Image.new("RGB", (W * cw + 16, H * ch + 16), BG)
    d = ImageDraw.Draw(img)
    for y in range(H):
        row = screen.buffer[y]
        for x in range(W):
            c = row[x]
            if c.data.strip():
                d.text((8 + x * cw, 8 + y * ch), c.data, font=BOLD if c.bold else FONT, fill=PAL.get(c.fg, PAL["default"]))
    return img

frames, durs = [], []
MIN = 0.08  # merge output closer than this into one frame
i = 0
while i < len(events):
    t = events[i][0]
    while i < len(events) and events[i][0] - t < MIN:
        stream.feed(events[i][2]); i += 1
    nxt = events[i][0] if i < len(events) else t + 5
    frames.append(draw()); durs.append(int(max(nxt - t, MIN) * 1000))
pal = [f.convert("P", palette=Image.ADAPTIVE, colors=32) for f in frames]
pal[0].save(out, save_all=True, append_images=pal[1:], duration=durs, loop=0, optimize=True)
print(len(frames), "frames", sum(durs) / 1000, "s")

#!/usr/bin/env python3
"""Generate the 10x10 1-bit app icon (a toothbrush).

Flipper renders black pixels (0) as lit. Run: python3 gen_icon.py
"""
from PIL import Image

# 10x10 toothbrush: bristle tufts (top), brush head (right), handle (long bar).
# 1 = ink (black/lit), 0 = background.
BRUSH = [
    "0000000000",
    "0000001010",  # bristle tufts
    "0000001010",
    "0000011111",  # head top
    "0000011111",
    "1111111111",  # handle
    "1111111111",
    "0000011111",  # head bottom
    "0000011111",
    "0000000000",
]

img = Image.new("1", (10, 10), 1)  # start white (1)
px = img.load()
for y, row in enumerate(BRUSH):
    for x, c in enumerate(row):
        if c == "1":
            px[x, y] = 0  # ink
img.save("icon.png")
print("wrote icon.png (10x10, 1-bit)")

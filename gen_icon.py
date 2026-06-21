#!/usr/bin/env python3
"""Generate the 10x10 1-bit app icon (a tooth).

Flipper renders black pixels (0) as lit. Run: python3 gen_icon.py
"""
from PIL import Image

# 10x10 tooth: rounded crown on top, two roots splitting at the bottom.
# 1 = ink (black/lit), 0 = background.
TOOTH = [
    "0011111100",  # crown top (rounded)
    "0111111110",
    "1111111111",
    "1111111111",
    "1111111111",
    "1111111111",
    "1111111111",
    "1111001111",  # roots begin to split
    "1110000111",
    "1100000011",  # two pointed roots
]

img = Image.new("1", (10, 10), 1)  # white background
px = img.load()
for y, row in enumerate(TOOTH):
    for x, c in enumerate(row):
        if c == "1":
            px[x, y] = 0  # ink
img.save("icon.png")
print("wrote icon.png (10x10, 1-bit tooth)")

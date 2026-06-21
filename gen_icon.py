#!/usr/bin/env python3
"""Generate the 10x10 1-bit app icon (a molar tooth).

Flipper renders black pixels (0) as lit. Run: python3 gen_icon.py
"""
from PIL import Image

# 10x10 molar: two cusps on top, a solid body, and two short roots with a
# notch at the bottom. The bumpy crown + dominant body avoid the "bridge" look
# that straight-sided two-legged shapes give.
# 1 = ink (black/lit), 0 = background.
TOOTH = [
    "0111001110",  # two cusps (crown)
    "0111111110",
    "0111111110",
    "0111111110",
    "0111111110",
    "0111111110",
    "0111111110",
    "0111111110",
    "0110000110",  # roots split
    "0100000010",
]

img = Image.new("1", (10, 10), 1)  # white background
px = img.load()
for y, row in enumerate(TOOTH):
    for x, c in enumerate(row):
        if c == "1":
            px[x, y] = 0  # ink
img.save("icon.png")
print("wrote icon.png (10x10, 1-bit molar tooth)")

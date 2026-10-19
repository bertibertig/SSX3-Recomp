#!/usr/bin/env python3
"""Puts frame dumps (BMP) side by side in one PNG, to look at a whole test run at once.
  contact_sheet.py out.png frame1.bmp frame2.bmp ...   (4 per row, 320x240 each)
"""
import sys
from PIL import Image

out, files = sys.argv[1], sys.argv[2:]
if not files:
    sys.exit("no frames")
ims = [Image.open(f).convert("RGB").resize((320, 240)) for f in files]
cols = 4
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (320 * cols, 240 * rows))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * 320, (i // cols) * 240))
sheet.save(out)
print(f"{out}: {len(ims)} frames")

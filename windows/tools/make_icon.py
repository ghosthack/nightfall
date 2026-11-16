"""Generate nightfall.ico (crescent moon) with no dependencies: 16/24/32/48/64/256 px, 32bpp BMP entries."""
import struct, sys, math

def render(size):
    ss = 4
    px = []
    cx, cy, r = 0.5, 0.5, 0.40          # outer disc
    ox, oy, orad = 0.66, 0.38, 0.34     # cut-out disc -> crescent
    for y in range(size - 1, -1, -1):   # BMP rows are bottom-up
        row = []
        for x in range(size):
            hit = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    if (u-cx)**2 + (v-cy)**2 <= r*r and (u-ox)**2 + (v-oy)**2 > orad*orad:
                        hit += 1
            a = round(255 * hit / (ss*ss))
            # pale moonlight colour, premultiplication not required for ICO
            row.append((0xF2, 0xE6, 0xC8, a))  # R,G,B,A
        px.append(row)
    return px

def bmp_entry(size):
    px = render(size)
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size*2, 1, 32, 0, 0, 0, 0, 0, 0)
    data = b''.join(bytes((b, g, r, a)) for row in px for (r, g, b, a) in row)
    mask_stride = ((size + 31) // 32) * 4
    mask = b'\x00' * (mask_stride * size)
    return hdr + data + mask

sizes = [16, 20, 24, 32, 48, 64, 256]
entries = [bmp_entry(s) for s in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for s, e in zip(sizes, entries):
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(e), off)
    off += len(e)
out += b''.join(entries)
open(sys.argv[1], 'wb').write(out)

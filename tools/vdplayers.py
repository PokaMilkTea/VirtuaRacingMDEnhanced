#!/usr/bin/env python3
"""Split a Mega Drive frame into its VDP layers (mode 5), from an `svptrace -v` dump.

usage: vdplayers.py VDP.bin [SHOT.ppm] [OUTPREFIX]

Prints the VDP setup, then renders plane B, plane A (with the window), sprites and their
composite. With SHOT.ppm (svptrace -w of the same frame) the composite is compared line by
line; mismatching lines point at mid-frame (raster) changes the end-of-frame dump misses.
With OUTPREFIX it writes PREFIX_{b,a,s,composite}.ppm, transparent pixels in magenta.
Output is ROM-derived: keep it in out/.
"""
import struct
import sys

MAGENTA = (255, 0, 255)


def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'VDP1', 'not an svptrace -v dump'
    reg = d[4:0x24]
    raw = d[0x24:0x10024]
    vram = bytes(raw[i ^ 1] for i in range(0x10000))   # GPGX keeps host-order words
    cram = struct.unpack('<64H', d[0x10024:0x100A4])
    vsram = struct.unpack('<64H', d[0x100A4:0x10124])
    return reg, vram, cram, vsram


def rgb(c9):
    """CRAM BBBGGGRRR -> RGB the way GPGX 16bpp + svptrace's PPM writer produce it."""
    r, g, b = (c9 & 7) << 1, ((c9 >> 3) & 7) << 1, ((c9 >> 6) & 7) << 1
    r5, g6, b5 = (r << 1) | (r >> 3), (g << 2) | (g >> 2), (b << 1) | (b >> 3)
    return r5 << 3, g6 << 2, b5 << 3


class VDP:
    """The VDP state of a dump and its mode 5 rendering, one pixel at a time"""

    def __init__(self, reg, vram, cram, vsram):
        self.reg, self.vram, self.cram, self.vsram = reg, vram, cram, vsram
        self.h40 = bool(reg[12] & 0x81)
        self.width = 320 if self.h40 else 256
        self.height = 240 if reg[1] & 0x08 else 224
        sizes = (32, 64, 32, 128)                                 # plane size codes, in cells
        self.plane_width_cells = sizes[reg[16] & 3]
        self.plane_height_cells = sizes[(reg[16] >> 4) & 3]
        self.plane_a_table = (reg[2] & 0x38) << 10
        self.plane_b_table = (reg[4] & 0x07) << 13
        self.window_table = (reg[3] & (0x3C if self.h40 else 0x3E)) << 10
        self.sprite_table = (reg[5] & (0x7E if self.h40 else 0x7F)) << 9
        self.hscroll_table = (reg[13] & 0x3F) << 10

    def word(self, address):
        return (self.vram[address & 0xFFFF] << 8) | self.vram[(address + 1) & 0xFFFF]

    def tile_pixel(self, entry, x, y):
        """(palette index 0-63, or 0 if transparent; priority) for pixel x, y inside the cell
        that a name table entry names"""
        if entry & 0x0800:                                        # horizontal flip
            x = 7 - x
        if entry & 0x1000:                                        # vertical flip
            y = 7 - y
        tile_byte = self.vram[((entry & 0x7FF) * 32 + y * 4 + x // 2) & 0xFFFF]
        colour = (tile_byte >> 4) if x % 2 == 0 else (tile_byte & 15)
        palette = (entry >> 13) & 3
        index = (palette << 4 | colour) if colour else 0
        return index, bool(entry & 0x8000)

    def hscroll(self, y):
        """(plane A, plane B) horizontal scroll of line y, by the scroll mode in reg 11"""
        mode = self.reg[11] & 3
        if mode == 0:
            line = 0                    # whole screen
        elif mode == 2:
            line = y & ~7               # per cell row
        elif mode == 3:
            line = y                    # per line
        else:
            line = y & 7                # mode 1: the first 8 entries repeat
        address = self.hscroll_table + line * 4
        return self.word(address) & 0x3FF, self.word(address + 2) & 0x3FF

    def vscroll(self, x):
        """(plane A, plane B) vertical scroll at screen x: whole plane, or per 16-pixel column"""
        column = (x // 16) * 2 if self.reg[11] & 4 else 0
        return self.vsram[column] & 0x7FF, self.vsram[column + 1] & 0x7FF

    def in_window(self, x, y):
        """Whether screen x, y is in the window: whole lines above or below a row (reg 18),
        otherwise the columns left or right of a position (reg 17)"""
        horizontal, vertical = self.reg[17], self.reg[18]
        row_line = (vertical & 0x1F) * 8
        below = vertical & 0x80
        if (below and y >= row_line) or (not below and y < row_line):
            return True
        column_x = (horizontal & 0x1F) * 16
        right = horizontal & 0x80
        return x >= column_x if right else x < column_x

    def plane(self, which, x, y):
        """(index, priority, source) of plane 'A' (or the window, source 'W') or 'B' at x, y"""
        if which == 'A' and self.in_window(x, y):
            window_columns = 64 if self.h40 else 32
            entry = self.word(self.window_table + ((y // 8) * window_columns + x // 8) * 2)
            return self.tile_pixel(entry, x % 8, y % 8) + ('W',)
        plane_index = 0 if which == 'A' else 1
        hscroll = self.hscroll(y)[plane_index]
        vscroll = self.vscroll(x)[plane_index]
        plane_x = (x - hscroll) % (self.plane_width_cells * 8)
        plane_y = (y + vscroll) % (self.plane_height_cells * 8)
        table = self.plane_a_table if which == 'A' else self.plane_b_table
        entry = self.word(table + ((plane_y // 8) * self.plane_width_cells + plane_x // 8) * 2)
        return self.tile_pixel(entry, plane_x % 8, plane_y % 8) + (which,)

    def sprite_order(self):
        """Sprite numbers in drawing priority, following the link field from sprite 0"""
        limit = 80 if self.h40 else 64
        order, number = [], 0
        while len(order) < limit:
            order.append(number)
            number = self.vram[self.sprite_table + number * 8 + 3] & 0x7F
            if number == 0 or number >= limit:
                break
        return order

    def sprites(self):
        """Per pixel (index, priority, sprite number) of the topmost opaque sprite pixel, or None"""
        out = [[None] * self.width for _ in range(self.height)]
        sprites_on_line = [0] * self.height
        max_per_line = 20 if self.h40 else 16
        for number in self.sprite_order():
            entry = self.sprite_table + number * 8
            top = (self.word(entry) & 0x3FF) - 128
            cells_across = ((self.vram[entry + 2] >> 2) & 3) + 1
            cells_down = (self.vram[entry + 2] & 3) + 1
            attributes = self.word(entry + 4)
            left = (self.word(entry + 6) & 0x1FF) - 128
            for sprite_y in range(cells_down * 8):
                y = top + sprite_y
                if not 0 <= y < self.height:
                    continue
                sprites_on_line[y] += 1
                if sprites_on_line[y] > max_per_line:
                    continue
                for sprite_x in range(cells_across * 8):
                    x = left + sprite_x
                    if not 0 <= x < self.width or out[y][x] is not None:
                        continue
                    # the cell's tile: tiles run down each column of cells; flips mirror the cells
                    cell_x, cell_y = sprite_x // 8, sprite_y // 8
                    if attributes & 0x0800:
                        cell_x = cells_across - 1 - cell_x
                    if attributes & 0x1000:
                        cell_y = cells_down - 1 - cell_y
                    tile = ((attributes & 0x7FF) + cell_x * cells_down + cell_y) & 0x7FF
                    index, priority = self.tile_pixel((attributes & 0xF800) | tile, sprite_x % 8, sprite_y % 8)
                    if index:
                        out[y][x] = (index, priority, number)
        return out


def write_ppm(path, w, h, pixels):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(c for p in pixels for c in p))


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, [tuple(parts[3][i:i + 3]) for i in range(0, w * h * 3, 3)]


def composite_index(sprite, plane_a, plane_b, backdrop):
    """The palette index shown, by priority: sprites high > A high > B high > sprites low >
    A low > B low > backdrop. sprite is None or (index, priority, number); planes (index, priority, ...)."""
    sprite_index = sprite[0] if sprite else 0
    sprite_high = bool(sprite and sprite[1])
    candidates = (
        (sprite_index, sprite_high),
        (plane_a[0], plane_a[1]),
        (plane_b[0], plane_b[1]),
        (sprite_index, bool(sprite) and not sprite_high),
        (plane_a[0], not plane_a[1]),
        (plane_b[0], not plane_b[1]),
    )
    for index, applies in candidates:
        if index and applies:
            return index
    return backdrop


def main():
    vdp = VDP(*load(sys.argv[1]))
    shot = read_ppm(sys.argv[2]) if len(sys.argv) > 2 else None
    prefix = sys.argv[3] if len(sys.argv) > 3 else None
    reg = vdp.reg
    print('mode %s %dx%d  display %s  shadow/highlight %s  bg colour %d' % (
        'H40' if vdp.h40 else 'H32', vdp.width, vdp.height, 'on' if reg[1] & 0x40 else 'off',
        bool(reg[12] & 8), reg[7] & 0x3F))
    print('plane %dx%d cells  A @%04x  B @%04x  window @%04x (H %02x V %02x)  sprites @%04x  hscroll @%04x' % (
        vdp.plane_width_cells, vdp.plane_height_cells, vdp.plane_a_table, vdp.plane_b_table,
        vdp.window_table, reg[17], reg[18], vdp.sprite_table, vdp.hscroll_table))
    print('hscroll mode %d  vscroll %s  vsram A %d B %d' % (
        reg[11] & 3, '2-cell' if reg[11] & 4 else 'full', vdp.vsram[0], vdp.vsram[1]))

    sprites = vdp.sprites()
    layers = {name: [[None] * vdp.width for _ in range(vdp.height)] for name in 'BA'}
    composite = []
    for y in range(vdp.height):
        for x in range(vdp.width):
            plane_b = vdp.plane('B', x, y)
            plane_a = vdp.plane('A', x, y)
            layers['B'][y][x], layers['A'][y][x] = plane_b, plane_a
            index = composite_index(sprites[y][x], plane_a, plane_b, reg[7] & 0x3F)
            composite.append(rgb(vdp.cram[index]))

    if shot:
        shot_w, shot_h, shot_pixels = shot
        differing = [y for y in range(min(vdp.height, shot_h))
                     if composite[y * vdp.width:(y + 1) * vdp.width] != shot_pixels[y * shot_w:(y + 1) * shot_w]]
        detail = ''
        if differing:
            detail = '; differing lines %d-%d (%d lines)' % (differing[0], differing[-1], len(differing))
        print('composite vs screenshot: %d / %d lines identical%s' % (vdp.height - len(differing), vdp.height, detail))

    for name in ('B', 'A'):
        grid = layers[name]
        opaque = sum(1 for row in grid for p in row if p[0])
        high = sum(1 for row in grid for p in row if p[0] and p[1])
        window = sum(1 for row in grid for p in row if p[2] == 'W' and p[0])
        print('plane %s: %d opaque pixels (%d high priority%s)' % (
            name, opaque, high, ', %d from the window' % window if name == 'A' else ''))
    sprite_numbers = sorted({p[2] for row in sprites for p in row if p})
    print('sprites: %d opaque pixels from %d sprites' % (
        sum(1 for row in sprites for p in row if p), len(sprite_numbers)))

    if prefix:
        def image(grid, colour_of):
            return [colour_of(p) for row in grid for p in row]

        def plane_colour(p):
            return rgb(vdp.cram[p[0]]) if p[0] else MAGENTA

        def sprite_colour(p):
            return rgb(vdp.cram[p[0]]) if p else MAGENTA

        write_ppm(prefix + '_b.ppm', vdp.width, vdp.height, image(layers['B'], plane_colour))
        write_ppm(prefix + '_a.ppm', vdp.width, vdp.height, image(layers['A'], plane_colour))
        write_ppm(prefix + '_s.ppm', vdp.width, vdp.height, image(sprites, sprite_colour))
        write_ppm(prefix + '_composite.ppm', vdp.width, vdp.height, composite)


if __name__ == '__main__':
    main()

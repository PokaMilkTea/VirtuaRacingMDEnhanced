#!/usr/bin/env python3
"""Analyse an `svptrace -m` log of one SVP command (see docs/svp-commands.md).

usage: svplog.py phases  LOG               memory regions read/written per pipeline phase
       svplog.py polys   LOG               the depth-sorted polygon list (far to near)
       svplog.py compare LOG [DIFF.ppm]    redraw the polygon list, compare with the SVP's own
                                           256x192 tile-buffer output, optionally write a diff
       svplog.py preview LOG FRAME OUTDIR  4x images: SVP output vs polygon list with blended
                                           dithering (FRAME = a -w frame, for its palette)
       svplog.py objects LOG ROM           re-transform and re-project every object polygon
                                           (phase 4) bit-exactly and compare with the list
       svplog.py track   LOG ROM           same for the track sections (phase 3)
       svplog.py matrices LOG ROM          recompute every slot matrix of phase 1 bit-exactly
       svplog.py buckets LOG ROM           recompute each polygon's depth bucket from its flags

The log must cover one command (svptrace -m, with -f at the same frame so that the command's
slot dump comes first) and, for preview, contain a PAL line.
Everything this produces is ROM-derived: keep it local (out/ is git-ignored).
"""
import collections
import math
import struct
import sys

W, H = 256, 192            # SVP 3D viewport
CX, CY = 129, 96           # polygon-list origin inside the viewport (verified, docs 6.3)
TILE_BUFFERS = (0x300000, 0x306000)
TILE_BUFFER_SIZE = 0x6000
MODEL_SLOTS = 0x30EC00     # 64 slots of 0x40 bytes
SLOT_SIZE = 0x40
BUCKETS = 0x310000         # depth buckets of 0x20 bytes: a count, then word pointers to polygons


def s16(value):
    """A 16-bit word as a signed number"""
    return value - 0x10000 if value & 0x8000 else value


def accesses(path):
    """(svp pc, kind R/W/O, byte address, data) of every SVP memory access in the log"""
    for line in open(path):
        if line.startswith('P '):
            _, pc, kind, address, data = line.split()
            yield int(pc, 16), kind, int(address, 16), int(data, 16)


def region(address):
    if address < 0x200000:
        return 'ROM'
    if 0x300000 <= address < 0x320000:
        return 'DRAM'
    if 0x390000 <= address < 0x391000:
        return 'IRAM'
    return '?'


def in_tile_buffers(address):
    return TILE_BUFFERS[0] <= address < TILE_BUFFERS[1] + TILE_BUFFER_SIZE


def geometry_memory(path):
    """DRAM as written by the SVP up to its first tile-buffer write (polygon list + buckets)"""
    memory = {}
    for _, kind, address, data in accesses(path):
        if kind != 'R' and in_tile_buffers(address):
            break
        if kind != 'R':
            memory[address] = data
    return memory


def polygons(memory):
    """(bucket, header, [(x, y)...]) from the depth buckets at 310000, far (high index) first"""
    for bucket_address in range(0x31FFE0, 0x30FFE0, -0x20):
        for i in range(memory.get(bucket_address, 0)):
            pointer = memory.get(bucket_address + 2 + 2 * i)
            if pointer is None:
                continue
            record = 0x300000 + pointer * 2
            header = memory.get(record, 0)
            vertex_count = 3 if header & 0x10 else 4
            vertices = [(s16(memory.get(record + 2 + 4 * k, 0)), s16(memory.get(record + 4 + 4 * k, 0)))
                        for k in range(vertex_count)]
            yield (bucket_address - BUCKETS) // 0x20, header, vertices


def fill(w, h, vertices, plot):
    """Scanline fill of a convex polygon; a pixel is covered if its centre is inside"""
    ys = [v[1] for v in vertices]
    first_y = max(0, math.ceil(min(ys) - 0.5))
    last_y = min(h, math.floor(max(ys) - 0.5) + 1)
    for y in range(first_y, last_y):
        centre_y, crossings = y + 0.5, []
        for i in range(len(vertices)):
            (x0, y0), (x1, y1) = vertices[i], vertices[(i + 1) % len(vertices)]
            if y0 <= centre_y < y1 or y1 <= centre_y < y0:
                crossings.append(x0 + (centre_y - y0) * (x1 - x0) / (y1 - y0))
        if len(crossings) >= 2:
            first_x = max(0, math.ceil(min(crossings) - 0.5))
            last_x = min(w, math.floor(max(crossings) - 0.5) + 1)
            for x in range(first_x, last_x):
                plot(x, y)


def screen_vertices(vertices, scale):
    """Polygon-list vertices (origin CX, CY; y up) to image coordinates at a scale"""
    return [((CX + x) * scale, (CY - y) * scale) for x, y in vertices]


def draw_polygons(polygon_list, scale=1, colour=None):
    """Palette-index image (None = not drawn). colour(header, x, y) -> index, or None for transparent."""
    image = [None] * (W * scale * H * scale)
    for _, header, vertices in polygon_list:
        def plot(x, y, header=header):
            index = colour(header, x // scale, y // scale)
            if index is not None:
                image[y * W * scale + x] = index
        fill(W * scale, H * scale, screen_vertices(vertices, scale), plot)
    return image


def dither(header, x, y):
    """SVP dithering (verified, docs 6.4): colour 1 on even x (lines) or odd x+y (checkerboard)"""
    colour_1, colour_2 = (header >> 12) & 15, (header >> 8) & 15
    if header & 0x20:
        use_second = (x + y + 1) & 1
    else:
        use_second = x & 1
    colour = colour_2 if use_second else colour_1
    return colour or None


def svp_output(path):
    """The SVP's own 256x192 output, from its writes to whichever tile buffer it drew into"""
    image = [None] * (W * H)
    for _, kind, address, data in accesses(path):
        if kind == 'R' or not in_tile_buffers(address):
            continue
        base = TILE_BUFFERS[1] if address >= TILE_BUFFERS[1] else TILE_BUFFERS[0]
        offset = address - base
        # 16-line strips of 0x800 bytes, 8-pixel columns of 64 bytes, 4 bytes (8 pixels) a line
        strip, column, within = offset // 0x800, (offset % 0x800) // 64, offset % 64
        y = strip * 16 + within // 4
        x0 = column * 8 + (within % 4) * 2
        for k in range(4):
            nibble = (data >> (12 - 4 * k)) & 15
            if kind == 'O' and nibble == 0:        # overwrite mode skips zero nibbles
                continue
            image[y * W + x0 + k] = nibble
    return image


def write_ppm(path, w, h, pixels):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(c for p in pixels for c in p))


def palette(path, frame):
    """Palette line 0 of a -w frame's PAL line as RGB (each 3-bit channel scaled to 255)"""
    for line in open(path):
        if line.startswith('PAL %06d' % frame):
            colours = [int(value, 16) for value in line.split()[2:18]]
            return [((c & 7) * 255 // 7, ((c >> 3) & 7) * 255 // 7, ((c >> 6) & 7) * 255 // 7) for c in colours]
    sys.exit('no PAL line for frame %d (run svptrace with -w %d)' % (frame, frame))


def read_rom(rom_path):
    """The ROM as bytes and as big-endian words"""
    rom = open(rom_path, 'rb').read()
    return rom, struct.unpack('>%dH' % (len(rom) // 2), rom)


# --- phases ----------------------------------------------------------------------------------
# The SVP loads an overlay into IRAM for each phase of a command, so a run of IRAM writes
# starts a new phase.

def merged_ranges(addresses, gap=0x40):
    """Sorted address ranges [first, last], merging addresses at most gap apart"""
    ranges = []
    for address in sorted(set(addresses)):
        if ranges and address - ranges[-1][1] <= gap:
            ranges[-1][1] = address
        else:
            ranges.append([address, address])
    return ranges


def cmd_phases(path):
    phases, current, last_was_iram_write = [], None, False
    for pc, kind, address, _ in accesses(path):
        iram_write = kind != 'R' and region(address) == 'IRAM'
        if current is None or (iram_write and not last_was_iram_write):
            current = {'accesses': collections.defaultdict(list), 'pcs': set(), 'load': 0}
            phases.append(current)
        last_was_iram_write = iram_write
        if iram_write:
            current['load'] += 1
        else:
            current['accesses'][('R' if kind == 'R' else 'W', region(address))].append(address)
            current['pcs'].add(pc)

    for number, phase in enumerate(phases):
        pcs = sorted(phase['pcs'])
        pc_range = '%04x-%04x' % (pcs[0], pcs[-1]) if pcs else '-'
        print('== phase %d: IRAM load %d words; pc %s' % (number, phase['load'], pc_range))
        for (kind, area), addresses in sorted(phase['accesses'].items()):
            ranges = merged_ranges(addresses)
            more = ' ...(%d ranges)' % len(ranges) if len(ranges) > 8 else ''
            shown = ' '.join('%06x-%06x' % tuple(r) for r in ranges[:8])
            print('   %s %-4s %5d: %s%s' % (kind, area, len(addresses), shown, more))


def cmd_polys(path):
    for bucket, header, vertices in polygons(geometry_memory(path)):
        print('%4d %04X %s' % (bucket, header, ' '.join('(%d,%d)' % v for v in vertices)))


def cmd_compare(path, diff_path=None):
    svp = svp_output(path)
    ours = draw_polygons(list(polygons(geometry_memory(path))), colour=dither)
    both = [i for i in range(W * H) if ours[i] is not None and svp[i]]
    same = sum(1 for i in both if ours[i] == svp[i])
    only_ours = sum(1 for i in range(W * H) if ours[i] is not None and not svp[i])
    only_svp = sum(1 for i in range(W * H) if ours[i] is None and svp[i])
    print('SVP drew %d nonzero pixels; drawn by both %d, same colour %d (%.2f%%)'
          % (sum(1 for p in svp if p), len(both), same, 100.0 * same / max(len(both), 1)))
    print('only ours: %d   only SVP: %d' % (only_ours, only_svp))
    if diff_path:
        def diff_colour(i):
            if ours[i] is None and not svp[i]:
                return (30, 30, 30)          # neither
            if ours[i] == svp[i]:
                return (0, 160, 0)           # same
            return (220, 0, 0)               # different
        write_ppm(diff_path, W, H, [diff_colour(i) for i in range(W * H)])


def cmd_preview(path, frame, outdir, scale=4):
    colours = palette(path, frame)
    background = (24, 24, 32)
    svp = svp_output(path)
    big = []
    for y in range(H * scale):
        for x in range(W * scale):
            index = svp[(y // scale) * W + x // scale]
            big.append(colours[index] if index else background)
    write_ppm(outdir + '/svp_original_x%d.ppm' % scale, W * scale, H * scale, big)

    # dither of two real colours -> their average, solid; dither with colour 0 -> 50% alpha
    # (docs 6.5); polygons are composited far to near, like the SVP draws them
    big = [background] * (W * scale * H * scale)
    for _, header, vertices in polygons(geometry_memory(path)):
        colour_1, colour_2 = (header >> 12) & 15, (header >> 8) & 15
        used = [colours[c] for c in (colour_1, colour_2) if c]
        if not used:
            continue
        colour = tuple(sum(channel) // len(used) for channel in zip(*used))
        alpha = 0.5 if len(used) == 1 and colour_1 != colour_2 else 1.0

        def plot(x, y, colour=colour, alpha=alpha):
            i = y * W * scale + x
            big[i] = tuple(int(c * alpha + b * (1 - alpha)) for c, b in zip(colour, big[i]))
        fill(W * scale, H * scale, screen_vertices(vertices, scale), plot)
    write_ppm(outdir + '/polylist_x%d_blended.ppm' % scale, W * scale, H * scale, big)


# --- object transform + projection, bit-exact (docs 6.8) -----------------------------------
RECIP_TABLE = 0x5845       # program-memory word address of R(n) = 32768 / n
NEAR_LIMIT = 0x400         # z' at or above this uses R(z' >> 5) (track data pins it to 0x3FC..0x404)


def wrap32(value):
    """Wrap to a signed 32-bit number, like the SVP's accumulator"""
    value &= 0xFFFFFFFF
    return value - (1 << 32) if value & 0x80000000 else value


def high16(accumulator):
    """The signed high word of a 32-bit accumulator"""
    return s16((accumulator >> 16) & 0xFFFF)


def multiply_add(pairs, shift):
    """The SVP's mld/mpya chain: the 32-bit sum of 2 * a * b over the pairs, shifted left,
    high word"""
    accumulator = 0
    for a, b in pairs:
        accumulator = wrap32(accumulator + 2 * s16(a & 0xFFFF) * s16(b & 0xFFFF))
    return high16(wrap32(accumulator << shift))


def to_camera(matrix12, vertex):
    """Slot +0E..+24 (rows of m0 m1 m2 t, 1.7 fixed) applied to a model vertex (overlay 19B5 @0096)"""
    out = []
    for row in range(3):
        m = matrix12[4 * row:4 * row + 4]
        out.append(multiply_add(((m[0], vertex[0]), (m[1], vertex[1]), (m[2], vertex[2]), (m[3], 0x400)), 3))
    return out


def project(rom_words, camera, z_offset=0, limit=NEAR_LIMIT):
    """Perspective divide by reciprocal table (overlay 19B5 @00B8): ~ (64x/z, 80y/z)"""
    x, y, z = camera
    z_shifted = z - z_offset
    if z_shifted - limit >= 0:
        reciprocal = s16(rom_words[(RECIP_TABLE + (z_shifted >> 5)) & 0xFFFF])
        product = 2 * reciprocal * y
        screen_y = high16(wrap32((product >> 1) + (high16(wrap32(product << 1)) << 16)))
        screen_x = high16(wrap32((2 * reciprocal * x) << 1))
    else:
        reciprocal = s16(rom_words[(RECIP_TABLE + z_shifted) & 0xFFFF])
        accumulator = wrap32((2 * reciprocal * y) << 4)
        screen_y = high16(wrap32((accumulator << 2) + (high16(accumulator) << 16)))
        screen_x = high16(wrap32((2 * reciprocal * x) << 6))
    return screen_x, screen_y


def issue_slots(path):
    """Slot words as the 68k left them when the command was issued (the CMD dump)"""
    slots = {}
    for line in open(path):
        if line.startswith('P '):
            break
        if line.startswith('  M '):
            fields = line.split()
            slots.setdefault(int(fields[1]), [int(v, 16) for v in fields[2:]])
    return slots


def track_to_camera(camera12, vertex, camera_position):
    """Track vertex (world) -> camera space with the 2.14 camera matrix (overlay 19B5 @0074)"""
    out = []
    for row in range(3):
        m = camera12[4 * row:4 * row + 4]
        relative = [vertex[k] - camera_position[k] for k in range(3)]
        out.append(multiply_add(((m[0], relative[0]), (m[1], relative[1]), (m[2], relative[2]), (m[3], 0x20)), 1))
    return out


def phase_records(path, wanted_phase):
    """For each polygon record the SVP writes in a phase: (slot, record address, header, number
    of ROM reads so far); and the list of ROM reads"""
    phase, last_was_iram_write, slot, rom_reads, records = 0, False, None, [], []
    for pc, kind, address, data in accesses(path):
        iram_write = kind != 'R' and region(address) == 'IRAM'
        if iram_write and not last_was_iram_write:
            phase += 1
        last_was_iram_write = iram_write
        if phase != wanted_phase:
            continue
        if kind == 'R' and MODEL_SLOTS <= address < MODEL_SLOTS + 64 * SLOT_SIZE:
            slot = (address - MODEL_SLOTS) // SLOT_SIZE
        if kind == 'R' and address < 0x200000:
            rom_reads.append(address)
        if kind != 'R' and pc in (0x011C, 0x0157):        # record header writes
            records.append((slot, address, data, len(rom_reads)))
    return records, rom_reads


def model_vertices(rom, rom_words, rom_reads, header):
    """The vertices of the model polygon just read (its header among the ROM reads), or None"""
    candidates = [r for r in rom_reads if rom_words[r // 2] == header]
    if not candidates:
        return None
    start = candidates[-1]
    vertex_count = 3 if header & 0x10 else 4
    return [struct.unpack('>hhh', rom[start + 2 + 6 * k:start + 8 + 6 * k]) for k in range(vertex_count)]


def check_phase(path, rom_path, wanted_phase, transform):
    """Re-project every record of a phase whose header matches the model polygon just read.
    Records made by near-plane clipping have a modified header or vertices and are counted apart."""
    rom, rom_words = read_rom(rom_path)
    memory = geometry_memory(path)
    records, rom_reads = phase_records(path, wanted_phase)
    exact, clipped, different = 0, 0, []
    for slot, record, header, read_count in records:
        vertices = model_vertices(rom, rom_words, rom_reads[:read_count], header)
        if vertices is None:
            clipped += 1
            continue
        cameras = [transform(memory, slot, v) for v in vertices]
        ours = [project(rom_words, c) for c in cameras]
        svp = [(s16(memory.get(record + 2 + 4 * k, 0)), s16(memory.get(record + 4 + 4 * k, 0)))
               for k in range(len(vertices))]
        if ours == svp:
            exact += 1
        elif min(c[2] for c in cameras) < 16:
            clipped += 1
        else:
            different.append((slot, header, svp, ours))
    print('%d bit-exact, %d near-plane clipped (not compared), %d differ' % (exact, clipped, len(different)))
    for slot, header, svp, ours in different[:5]:
        print('  slot %d %04X svp %s ours %s' % (slot, header, svp, ours))


def camera_world_position(camera_rows, slots):
    """The camera's world position: the player's car + floor(M^T . offset / 16384), M the
    camera's 2.14 rotation rows (overlay 15B5 @0125)"""
    car = [s16(v) for v in slots[1][1:4]]
    offset = [s16(slots[0][1]), s16(slots[0][2]), -s16(slots[0][3])]
    return [car[i] + (sum(camera_rows[j][i] * offset[j] for j in range(3)) >> 14) for i in range(3)]


def cmd_track(path, rom_path):
    slots = issue_slots(path)
    memory = geometry_memory(path)
    camera12 = [memory.get(0x30EC0E + 2 * i, 0) for i in range(12)]
    rows = [[s16(camera12[4 * r + k]) for k in range(3)] for r in range(3)]
    camera = camera_world_position(rows, slots)
    print('camera at', camera, '; sections', [s16(v) for v in slots[0][21:29]])
    check_phase(path, rom_path, 3, lambda memory, slot, v: track_to_camera(camera12, v, camera))


# --- phase 1: slot matrices, bit-exact (docs 6.13) -----------------------------------------
SIN_TABLE = 0x2845         # program-memory word address of sin(i*2pi/4096) * 0x4000


def phase1_written(path):
    """Slot +0E..+24 as written by the SVP in phase 1"""
    out, phase, last_was_iram_write = {}, 0, False
    for pc, kind, address, data in accesses(path):
        iram_write = kind != 'R' and region(address) == 'IRAM'
        if iram_write and not last_was_iram_write:
            phase += 1
        last_was_iram_write = iram_write
        if phase == 1 and kind != 'R' and MODEL_SLOTS <= address < MODEL_SLOTS + 64 * SLOT_SIZE:
            slot, offset = divmod(address - MODEL_SLOTS, SLOT_SIZE)
            if 0x0E <= offset <= 0x24:
                out.setdefault(slot, [0] * 12)[(offset - 0x0E) // 2] = s16(data)
    return out


class MatrixOps:
    """The overlay 15B5 primitives on a 4x3 matrix (3 rotation rows + translation row).
    Each op transforms what the previous ops produced (row-vector order)."""

    def __init__(self, rom_words):
        self.rom_words = rom_words

    def sin_cos(self, angle):                               # 00a8
        sine = s16(self.rom_words[SIN_TABLE + (angle & 0xFFF)])
        cosine = s16(self.rom_words[SIN_TABLE + ((angle + 0x400) & 0xFFF)])
        return sine, cosine

    @staticmethod
    def mac(*pairs):                                        # mld/mpya, shl, high word
        return multiply_add(pairs, 1)

    @staticmethod
    def identity(scale):                                    # 00b7 / 00bb / 00bf
        return [[scale, 0, 0], [0, scale, 0], [0, 0, scale], [0, 0, 0]]

    def rotate_x(self, m, angle):                           # 0067
        s, c = self.sin_cos(angle)
        return [[r[0], self.mac((r[1], c), (r[2], s)), self.mac((r[1], -s), (r[2], c))] for r in m]

    def rotate_y(self, m, angle):                           # 007c
        s, c = self.sin_cos(angle)
        return [[self.mac((r[0], c), (r[2], -s)), r[1], self.mac((r[0], s), (r[2], c))] for r in m]

    def rotate_z(self, m, angle):                           # 0093
        s, c = self.sin_cos(angle)
        return [[self.mac((r[0], c), (r[1], s)), self.mac((r[0], -s), (r[1], c)), r[2]] for r in m]

    @staticmethod
    def translate(m, d):                                    # 001c: quarter units in the matrix
        return m[:3] + [[s16((m[3][k] + 4 * d[k]) & 0xFFFF) for k in range(3)]]

    @staticmethod
    def dram12(m):                                          # 00e0: column k -> m0k m1k m2k tk
        return [m[r][k] for k in range(3) for r in range(4)]


def slot_matrices(rom_words, slots, camera12):
    """Phase-1 matrices for command 2 (overlays 126F/15B5), as the SVP writes them to +0E..+24;
    and the camera's world position"""
    ops = MatrixOps(rom_words)
    signed = {number: [s16(v) for v in words] for number, words in slots.items()}
    camera_slot = signed[0]
    angle_x, angle_y, angle_z = camera_slot[4], camera_slot[5], camera_slot[6]
    rows = [[s16(camera12[4 * r + k]) for k in range(3)] for r in range(3)]
    offset = (camera_slot[1], camera_slot[2], -camera_slot[3])
    camera = [signed[1][1 + i] + (sum(rows[j][i] * offset[j] for j in range(3)) >> 14) for i in range(3)]

    def player(m):         # 015a: player-car frame (slot 1 angles + camera angles), chase offset
        car = signed[1]
        m = ops.rotate_z(m, car[6] + angle_z)
        m = ops.rotate_x(m, car[4] + angle_x)
        m = ops.rotate_y(m, car[5] + angle_y)
        return ops.translate(m, (0, -camera_slot[2], camera_slot[3]))

    def opponent(m, car):  # 01e4/01e8: own rotation, world position, camera rotation
        m = ops.rotate_z(m, car[6])
        m = ops.rotate_x(m, car[4])
        m = ops.rotate_y(m, car[5])
        m = ops.translate(m, [car[1 + k] - camera[k] for k in range(3)])
        m = ops.rotate_y(m, angle_y)
        m = ops.rotate_x(m, angle_x)
        return ops.rotate_z(m, angle_z)

    def child(m, slot):    # 017a/020b: child X, Y rotation and offset inside the parent
        m = ops.rotate_x(m, slot[4])
        m = ops.rotate_y(m, slot[5])
        return ops.translate(m, slot[1:4])

    out = {}
    for number in signed:
        identity = ops.identity(0x80)
        if number in (1, 6, 7, 8, 9, 10):      # 6-10 reuse slot 1's frame (RAM1 still holds it)
            out[number] = player(identity)
        elif number in (2, 3, 4):
            out[number] = player(child(identity, signed[number]))
        elif 11 <= number <= 55 and (number - 11) % 3 == 0:
            out[number] = opponent(identity, signed[number])
        elif 11 <= number <= 55:
            parent = 11 + 3 * ((number - 11) // 3)
            out[number] = opponent(child(identity, signed[number]), signed[parent])
    return {number: ops.dram12(m) for number, m in out.items()}, camera


def cmd_matrices(path, rom_path):
    _, rom_words = read_rom(rom_path)
    written = phase1_written(path)
    ours, camera = slot_matrices(rom_words, issue_slots(path), [v & 0xFFFF for v in written[0]])
    exact = [s for s in written if s in ours and ours[s] == written[s]]
    different = [s for s in written if s in ours and ours[s] != written[s]]
    print('camera at %s; slot matrices bit-exact: %d / %d %s' % (camera, len(exact), len(exact) + len(different), sorted(exact)))
    for s in different:
        print('  slot %d svp %s ours %s' % (s, written[s], ours[s]))


# depth bucket per polygon-flag low 3 bits (docs 6.10); z = camera-space z of the polygon's vertices
DEPTH_KEY = {
    0: lambda z: (sum(z) // len(z)) >> 2,           # average
    1: lambda z: min(z) >> 2,                       # nearest vertex
    2: lambda z: max(z) >> 2,                       # farthest vertex
    3: lambda z: ((sum(z) // len(z)) >> 2) + 512,   # average, pushed back
    4: lambda z: (max(z) >> 2) + 256,               # farthest, pushed back
    5: lambda z: max(z) >> 2,                       # same bucket as 2 in all data seen so far
}


def record_buckets(memory):
    """record address -> the depth bucket it was put in"""
    bucket_of = {}
    for bucket_address in range(BUCKETS, BUCKETS + 0x10000, 0x20):
        for i in range(memory.get(bucket_address, 0)):
            pointer = memory.get(bucket_address + 2 + 2 * i)
            if pointer is not None:
                bucket_of[0x300000 + pointer * 2] = (bucket_address - BUCKETS) // 0x20
    return bucket_of


def cmd_buckets(path, rom_path):
    rom, rom_words = read_rom(rom_path)
    memory = geometry_memory(path)
    written = phase1_written(path)
    camera12 = [v & 0xFFFF for v in written[0]]
    _, camera = slot_matrices(rom_words, issue_slots(path), camera12)
    bucket_of = record_buckets(memory)
    tally = collections.defaultdict(lambda: [0, 0])          # flag -> [matching, total]
    for phase in (3, 4):
        records, rom_reads = phase_records(path, phase)
        for slot, record, header, read_count in records:
            vertices = model_vertices(rom, rom_words, rom_reads[:read_count], header)
            if vertices is None or record not in bucket_of:
                continue
            if phase == 3:
                zs = [track_to_camera(camera12, v, camera)[2] for v in vertices]
            else:
                zs = [to_camera([m & 0xFFFF for m in written[slot]], v)[2] for v in vertices]
            if min(zs) < 16:          # near-plane clipped: the SVP made new vertices
                continue
            flag = header & 7
            tally[flag][1] += 1
            tally[flag][0] += flag in DEPTH_KEY and DEPTH_KEY[flag](zs) == bucket_of[record]
    for flag in sorted(tally):
        print('flag %d: %d / %d buckets match' % (flag, tally[flag][0], tally[flag][1]))


def cmd_objects(path, rom_path):
    def transform(memory, slot, vertex):
        matrix = [memory.get(MODEL_SLOTS + slot * SLOT_SIZE + 0x0E + 2 * i, 0) for i in range(12)]
        return to_camera(matrix, vertex)
    check_phase(path, rom_path, 4, transform)


if __name__ == '__main__':
    command, args = sys.argv[1], sys.argv[2:]
    commands = {
        'phases': cmd_phases,
        'polys': cmd_polys,
        'compare': cmd_compare,
        'objects': cmd_objects,
        'track': cmd_track,
        'matrices': cmd_matrices,
        'buckets': cmd_buckets,
    }
    if command in commands:
        commands[command](*args)
    elif command == 'preview':
        cmd_preview(args[0], int(args[1]), args[2])
    else:
        sys.exit(__doc__)

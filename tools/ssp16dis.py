#!/usr/bin/env python3
"""SSP1601 (SVP) disassembler.

usage: ssp16dis.py rom start_word [end_word]
       ssp16dis.py rom --overlay src_word

Addresses are SVP program-memory word addresses; word W is at ROM byte 2*W
(only valid for W >= 0x400, below that is IRAM, filled at runtime).
IRAM code comes from overlays in ROM, copied by the loader at 0481:
[PMC address (8000 = IRAM word 0)] [length in words] [code...].
--overlay disassembles one at the IRAM address it runs from.
Decoding follows the GPGX interpreter (core/cart_hw/svp/ssp16.c, switch on
op >> 9) and notaz's svpdoc.txt. The output is ROM-derived: keep it local.
"""
import struct
import sys

# Register names by their 4-bit number
REGISTERS = ['-', 'X', 'Y', 'A', 'ST', 'STACK', 'PC', 'P',
             'PM0', 'PM1', 'PM2', 'XST', 'PM4', 'EXT5', 'PMC', 'AL']
ALU_OPERATIONS = {1: 'sub', 3: 'cmp', 4: 'add', 5: 'and', 6: 'or', 7: 'eor'}   # by group >> 4
MOD_OPERATIONS = {2: 'shr', 3: 'shl', 6: 'neg', 7: 'abs'}                    # mod instruction


def pointer_operand(pointer_register, mode):
    """The (rX) operand: pointer_register 0-7, mode 0 plain, 1 '+!', 2 '-', 3 '+'.
    r3 and r7 use the mode bits as an offset instead."""
    if pointer_register & 3 == 3:
        return '(r%d|%s)' % (pointer_register, format(mode, '02b'))
    return '(r%d%s)' % (pointer_register, ['', '+!', '-', '+'][mode])


def ram_operand(op):
    """A direct RAM address: [ram0:nn] or [ram1:nn], bank in bit 8"""
    bank = 'ram1:' if op & 0x100 else 'ram0:'
    return '[%s%02x]' % (bank, op & 0xff)


def condition(op):
    """The condition of a call, bra or mod: 'always', or flag=value (z, n or cX)"""
    flag = (op >> 4) & 0xf
    value = (op >> 8) & 1
    if flag == 0:
        return 'always'
    return {5: 'z', 7: 'n'}.get(flag, 'c%x' % flag) + '=%d' % value


def decode(op, next_word):
    """Return (text, size_in_words, branch_target_or_None) for the instruction op, followed by
    next_word (used by the two-word forms). Bits 9-15 of op select the instruction group."""
    group = op >> 9
    dst, src = (op >> 4) & 0xf, op & 0xf                  # register fields
    pointer_register = (op & 3) | ((op >> 6) & 4)         # for the (ri) forms
    mode = (op >> 2) & 3
    pointer = pointer_operand(pointer_register, mode)

    if group == 0x00:                                     # ld d, s
        if op == 0:
            return 'nop', 1, None
        if op == 0x65:
            return 'ret', 1, None                         # ld PC, STACK
        return 'ld %s, %s' % (REGISTERS[dst], REGISTERS[src]), 1, None
    if group == 0x01: return 'ld %s, %s' % (REGISTERS[dst], pointer), 1, None          # ld d, (ri)
    if group == 0x02: return 'ld %s, %s' % (pointer, REGISTERS[dst]), 1, None          # ld (ri), s
    if group == 0x03: return 'ld A, %s' % ram_operand(op), 1, None                     # ld A, [ram]
    if group == 0x04: return 'ldi %s, %04x' % (REGISTERS[dst], next_word), 2, None     # ldi d, imm
    if group == 0x05: return 'ld %s, (%s)' % (REGISTERS[dst], pointer), 1, None        # ld d, ((ri))
    if group == 0x06: return 'ldi %s, %04x' % (pointer, next_word), 2, None            # ldi (ri), imm
    if group == 0x07: return 'ld %s, A' % ram_operand(op), 1, None                     # ld [ram], A
    if group == 0x09: return 'ld %s, r%d' % (REGISTERS[dst], pointer_register), 1, None
    if group == 0x0a: return 'ld r%d, %s' % (pointer_register, REGISTERS[dst]), 1, None
    if 0x0c <= group <= 0x0f: return 'ldi r%d, %02x' % ((op >> 8) & 7, op & 0xff), 1, None
    if group == 0x24: return 'call %s, %04x' % (condition(op), next_word), 2, next_word
    if group == 0x25: return 'ld %s, (A)' % REGISTERS[dst], 1, None                    # program memory at A
    if group == 0x26: return 'bra %s, %04x' % (condition(op), next_word), 2, next_word
    if group == 0x48:
        operation = MOD_OPERATIONS.get(op & 7, '?%d' % (op & 7))
        return 'mod %s, %s' % (condition(op), operation), 1, None

    if group in (0x1b, 0x4b, 0x5b):                       # multiply forms on two pointers
        name = {0x1b: 'mpys', 0x4b: 'mpya', 0x5b: 'mld'}[group]
        second_register = 4 | ((op >> 4) & 3)
        first = pointer_operand(second_register, (op >> 6) & 3)
        second = pointer_operand(op & 3, (op >> 2) & 3)
        return '%s %s, %s' % (name, first, second), 1, None

    alu = ALU_OPERATIONS.get(group >> 4)                  # A op= operand, by the operand form
    if alu:
        form = group & 0xf
        if form == 0x0: return '%s A, %s' % (alu, REGISTERS[src]), 1, None
        if form == 0x1: return '%s A, %s' % (alu, pointer), 1, None
        if form == 0x3: return '%s A, %s' % (alu, ram_operand(op)), 1, None
        if form == 0x4: return '%si A, %04x' % (alu, next_word), 2, None
        if form == 0x5: return '%s A, (%s)' % (alu, pointer), 1, None
        if form == 0x9: return '%s A, r%d' % (alu, pointer_register), 1, None
        if form == 0xc: return '%si A, %02x' % (alu, op & 0xff), 1, None
    return 'dw %04x  ; ???' % op, 1, None


def pmc_comment(address, mode):
    """Decode a PMC address/mode pair (svpdoc 2.2.7) to a 68k byte address and access mode."""
    increment = [0, 1, 2, 4, 8, 16, 32, 128][(mode >> 11) & 7]
    text = '-> %06x' % ((((mode & 0x1f) << 16) | address) * 2)
    if mode & 0x4000:
        text += ' special-inc'
    elif increment:
        text += ' inc %s%d' % ('-' if mode & 0x8000 else '+', increment)
    if mode & 0x0400:
        text += ' overwrite'
    return text


def is_ldi_pmc(op):
    """ldi PMC, imm (register 14)"""
    return op >> 9 == 0x04 and (op >> 4) & 0xf == 14


def disassemble(words, start, end, base=0):
    """Print words[start:end]; base is added to the printed address (for overlays).
    Two ldi PMC in a row set up a PMC address and mode: the second is commented with both."""
    pc = start
    pmc_values = []          # immediates written to PMC since the last other instruction
    while pc < end:
        op, next_word = words[pc], words[pc + 1]
        text, size, _ = decode(op, next_word)
        raw = ' '.join('%04x' % words[pc + i] for i in range(size))
        if is_ldi_pmc(op):
            pmc_values.append(next_word)
            if len(pmc_values) == 2:
                text = '%-22s ; %s' % (text, pmc_comment(*pmc_values))
                pmc_values = []
        else:
            pmc_values = []
        print('%04x: %-10s %s' % (pc + base, raw, text))
        pc += size


def main():
    rom = open(sys.argv[1], 'rb').read()
    words = struct.unpack('>%dH' % (len(rom) // 2), rom)
    if sys.argv[2] == '--overlay':
        # an overlay in ROM: [PMC address (8000 = IRAM word 0)] [length in words] [code...]
        source = int(sys.argv[3], 16)
        destination, length = words[source] - 0x8000, words[source + 1]
        print('; overlay @%04x -> IRAM %03x, %d words' % (source, destination, length))
        disassemble(words, source + 2, source + 2 + length, destination - (source + 2))
    else:
        start = int(sys.argv[2], 16)
        end = int(sys.argv[3], 16) if len(sys.argv) > 3 else start + 0x40
        disassemble(words, start, end)


def demo():
    """Self-check against encodings from svpdoc.txt."""
    cases = {
        (0x0000, 0): 'nop',
        (0x0065, 0): 'ret',                       # ld PC, STACK
        (0x0013, 0): 'ld X, A',                   # 0000 0000 dddd ssss
        (0x0830, 0x1234): 'ldi A, 1234',          # 0000 1000 dddd 0000
        (0x1803, 0): 'ldi r0, 03',                # 0001 1jpp iiii iiii
        (0x4800, 0x0400): 'call always, 0400',    # 0100 100f cccc 0000
        (0x4c50, 0x0100): 'bra z=0, 0100',
        (0x9006, 0): 'mod always, neg',           # 1001 000f cccc 0ooo
        (0x8800, 0x0001): 'addi A, 0001',         # ooo0 1000 0000 0000
        (0xb7ff, 0): 'mld (r7|11), (r3|11)',      # 1011 0111 nnjj mmii
        (0x0405, 0): 'ld (r1+!), -',              # 0000 010j ssss mmpp
        (0x020d, 0): 'ld -, (r1+)',               # 0000 001j dddd mmpp
    }
    for (op, next_word), expected in cases.items():
        got = decode(op, next_word)[0]
        assert got == expected, (hex(op), got, expected)
    print('ok')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--test':
        demo()
    else:
        main()

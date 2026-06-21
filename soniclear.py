#!/usr/bin/env python3
"""
Soniclear - NTAG213 brush-head helper.

Computes the per-head NTAG213 write password from UID + MFG code, decodes the
brushing-time counter, and prints the reset page value. Reverse-engineered
password algo by @ATC1441 (Aaron Christophel); verified against a known head.

Reset = write page 0x24 (36) = `00 00 02 00` (counter -> 0) authenticating with PWD.

Usage:
    soniclear.py dump.nfc                 # parse a Flipper .nfc dump
    soniclear.py --uid 0436F47A141F90 --mfg "250625 51T"
"""
import sys, re

def crc16(crc, buf):
    for b in buf:
        crc ^= (b << 8)
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def gen_pwd(uid, mfg):
    c = crc16(0x49A3, uid)
    c = c | (crc16(c, mfg) << 16)
    c = ((c >> 8) & 0x00FF00FF) | ((c << 8) & 0xFF00FF00)
    return '%08X' % c

def parse_nfc(path):
    uid = None; pages = {}
    for ln in open(path):
        m = re.match(r'UID:\s*(.+)', ln)
        if m: uid = [int(x, 16) for x in m.group(1).split()]
        m = re.match(r'Page (\d+):\s*(.+)', ln)
        if m: pages[int(m.group(1))] = [int(x, 16) for x in m.group(2).split()]
    return uid, pages

def mfg_from_pages(pages):
    # pages 33-35 = 12 bytes; the MFG used by the algo = bytes [2..11] (skip 2-byte prefix)
    raw = pages[33] + pages[34] + pages[35]
    return raw[2:12]

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    if a[0] == '--uid':
        uid = [int(a[1][i:i+2], 16) for i in range(0, len(a[1]), 2)]
        mfg = list(a[3].encode())
        print('PWD:', gen_pwd(uid, mfg))
        return
    uid, pages = parse_nfc(a[0])
    mfg = mfg_from_pages(pages)
    pwd = gen_pwd(uid, mfg)
    p36 = pages[36]
    sec = p36[0] | (p36[1] << 8)            # brushing seconds (LE16)
    LIFE = 0x5460                            # 21600 s = 180 x 2min ~ 3 months
    print(f'UID   : {" ".join("%02X"%b for b in uid)}')
    print(f'MFG   : {bytes(mfg).decode(errors="replace")!r}')
    print(f'PWD   : {pwd}    (PWD_AUTH key)')
    print(f'Page36: {" ".join("%02X"%b for b in p36)}  -> {sec} s used '
          f'({sec/60:.0f} min, {100*sec/LIFE:.0f}% of ~3-month life)')
    print(f'RESET : write page 36 = 00 00 02 00  (auth PWD {pwd})')

if __name__ == '__main__':
    main()

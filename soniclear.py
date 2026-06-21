#!/usr/bin/env python3
"""Soniclear - NTAG213 brush-head helper (host side, stdlib only).

Computes the per-head NTAG213 write password from UID + MFG code, decodes the
brushing-time wear counter, and shows the value to write for a reset. The
password algorithm was reverse-engineered by @ATC1441 (Aaron Christophel) and
is verified against real heads; it depends only on the head (UID + MFG code),
not the handle.

Reset = authenticate with PWD, then write page 0x24 (36) = `00 00 02 00`
(counter -> 0).

Examples:
    soniclear.py dump.nfc                  # a Flipper NFC dump (.nfc)
    soniclear.py head.soniclear            # a record saved by the Flipper app
    soniclear.py --uid 04112233445566 --mfg "010203 99Z"
"""
import argparse
import re
import sys

LIFE = 0x5460  # 21600 s = 180 x 2-min sessions ~ 3-month rated life
COUNTER_PAGE = 36


def crc16(crc, buf):
    """CRC16-CCITT (poly 0x1021), no reflection, seeded with `crc`."""
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def gen_pwd(uid, mfg):
    """Return the 8-hex-char PWD_AUTH key for a head (uid: 7 bytes, mfg: bytes)."""
    c = crc16(0x49A3, uid)
    c = c | (crc16(c, mfg) << 16)
    c = ((c >> 8) & 0x00FF00FF) | ((c << 8) & 0xFF00FF00)
    return "%08X" % c


def _hexbytes(s):
    """Parse 'AA BB CC' or 'AABBCC' into a list of byte values."""
    s = s.strip()
    parts = s.split() if " " in s else [s[i : i + 2] for i in range(0, len(s), 2)]
    return [int(x, 16) for x in parts if x]


def parse_nfc(path):
    """Parse a Flipper .nfc dump -> (uid bytes, {page_index: [4 bytes]})."""
    uid, pages = None, {}
    with open(path) as f:
        for ln in f:
            m = re.match(r"UID:\s*(.+)", ln)
            if m and uid is None:
                uid = _hexbytes(m.group(1))
            m = re.match(r"Page\s+(\d+):\s*(.+)", ln)
            if m:
                pages[int(m.group(1))] = _hexbytes(m.group(2))
    return uid, pages


def parse_record(path):
    """Parse a .soniclear record saved by the app -> dict of fields, or None."""
    fields = {}
    with open(path) as f:
        head = f.read(64)
        if "Soniclear head" not in head and "Sonicare head" not in head:
            return None
        f.seek(0)
        for ln in f:
            m = re.match(r"(\w+):\s*(.+)", ln)
            if m:
                fields[m.group(1)] = m.group(2).strip()
    return fields


def mfg_from_pages(pages):
    # pages 33-35 = 12 bytes; the algo uses bytes [2..11] (skip the 2-byte prefix)
    raw = pages[33] + pages[34] + pages[35]
    return raw[2:12]


def report(uid, mfg, seconds, raw_page=None):
    pwd = gen_pwd(uid, mfg)
    print(f'UID    : {" ".join("%02X" % b for b in uid)}')
    print(f'MFG    : {bytes(mfg).decode(errors="replace")!r}')
    print(f"PWD    : {pwd}    (PWD_AUTH key)")
    if seconds is not None:
        pct = 100 * seconds / LIFE
        extra = f'  raw {" ".join("%02X" % b for b in raw_page)}' if raw_page else ""
        print(f"Used   : {seconds} s = {seconds/60:.0f} min, {pct:.0f}% of ~3-month life{extra}")
    print(f"Reset  : write page {COUNTER_PAGE} = 00 00 02 00  (auth with PWD {pwd})")
    return pwd


def main():
    ap = argparse.ArgumentParser(
        description="Compute the NTAG213 brush-head password and decode its wear counter.",
        epilog="With no arguments, pass a .nfc dump or a .soniclear record, "
        "or use --uid/--mfg.",
    )
    ap.add_argument("file", nargs="?", help="a Flipper .nfc dump or a .soniclear record")
    ap.add_argument("--uid", help="7-byte UID as hex, e.g. 04112233445566")
    ap.add_argument("--mfg", help='MFG code, e.g. "010203 99Z"')
    ap.add_argument("--selftest", action="store_true", help="check the algorithm against a fixed vector")
    args = ap.parse_args()

    if args.selftest:
        # synthetic regression vector (not a real head) - guards the algorithm
        got = gen_pwd([0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06], list(b"010203 99Z"))
        ok = got == "80B022C1"
        print(f"selftest: {got} {'OK' if ok else 'FAIL (expected 80B022C1)'}")
        sys.exit(0 if ok else 1)

    if args.uid:
        if not args.mfg:
            ap.error("--uid also needs --mfg")
        report(_hexbytes(args.uid), list(args.mfg.encode()), None)
        return

    if not args.file:
        ap.print_help()
        return

    try:
        rec = parse_record(args.file)
        if rec is not None:
            uid = _hexbytes(rec["UID"])
            mfg = list(rec["MFG"].encode())
            seconds = int(rec["Seconds"]) if "Seconds" in rec else None
            report(uid, mfg, seconds)
            if rec.get("Date"):
                print(f'Saved  : {rec["Date"]}')
            return
        uid, pages = parse_nfc(args.file)
        if uid is None or COUNTER_PAGE not in pages:
            ap.error(f"{args.file}: not a usable NTAG213 dump (need UID + page {COUNTER_PAGE})")
        p = pages[COUNTER_PAGE]
        report(uid, mfg_from_pages(pages), p[0] | (p[1] << 8), p)
    except FileNotFoundError:
        ap.error(f"{args.file}: no such file")
    except (KeyError, ValueError) as e:
        ap.error(f"{args.file}: could not parse ({e})")


if __name__ == "__main__":
    main()

# Brush Head Reset (Flipper Zero)

A [Flipper Zero](https://flipperzero.one/) app to **read and reset the wear counter**
stored on the NTAG213 chip embedded in many smart toothbrush heads — including
heads compatible with **Philips Sonicare BrushSync**.

It lets you keep using a brush head you judge still good (or re-zero a head whose
counter you want to manage yourself), instead of being told to bin it on a fixed
timer. Use it only on brush heads **you own**.

> [!IMPORTANT]
> **Not affiliated.** This is an independent, unofficial project. It is **not
> affiliated with, authorized, sponsored, maintained, or endorsed by Koninklijke
> Philips N.V.** "Philips", "Sonicare" and "BrushSync" are trademarks of their
> respective owners and are used here **only nominatively**, to describe what the
> tool is compatible with. No Philips firmware, artwork, or logo is included or
> distributed.

## What it does

Smart brush heads carry a tiny **NTAG213** NFC tag (NfcA, 13.56 MHz). One page
holds the accumulated **brushing time in seconds**; the handle reads it to decide
when to nag you for a replacement. This app reads that page, shows the usage, and —
with the head's own per-head password — can write it back to a value of your choice
(0 = brand new).

- **Read brush head** — shows model code, usage (% and minutes) and the computed password.
- **Reset to new** — set the counter back to 0%.
- **Save / Restore** — store a read to the SD card and write it back later.
- **Advanced**
  - **Set usage %** / **Set usage min** — write any target you want.
  - **Password calc** — compute a head's password from its UID + MFG code (offline).
- **About**.

The write password is **computed** from the tag's UID + factory code, so it is never
guessed: the app makes **exactly one authentication attempt per write**, which means
the tag's 3-failed-attempts permanent lockout can't be triggered by normal use.

## How the counter is encoded

Reverse-engineered from real heads (no proprietary code used):

| Item | Detail |
|------|--------|
| Tag | NTAG213, 7-byte UID, 45 pages |
| Counter | page `0x24`, bytes 0-1 = brushing **seconds**, little-endian 16-bit; bytes 2-3 = `02 00` (frame, preserved) |
| Fresh head | `00 00 02 00` (counter 0) |
| "Replace" threshold | ~`0x5460` = 21600 s = 180 × 2-min sessions ≈ 3 months |
| Checksum | none on the counter |
| Protection | pages ≥ 16 are password-write-protected (`AUTH0`), `AUTHLIM = 3` |

The per-head password is derived from the UID and the factory MFG string with a pair
of CRC16-CCITT passes (algorithm credit: [@ATC1441](https://github.com/atc1441)). It
depends only on the head, not the handle.

## Build

Built as a standalone `.fap` against the Flipper SDK with [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt):

```sh
ufbt            # builds dist/brush_reset.fap
ufbt launch     # build + install + run on a connected Flipper
```

Copy `dist/brush_reset.fap` to `/ext/apps/NFC/` on the SD card to install manually.

The 10×10 app icon can be regenerated with `python3 gen_icon.py` (needs Pillow).

## Usage

1. Pop the brush head off the handle.
2. Open **NFC → Brush Head Reset** and pick an action.
3. Lay the **base of the head flat on the back of the Flipper** and **hold it still**
   until the operation finishes. The head's coil is tiny — stable coupling matters,
   especially for a write. If a read works but a write says "Auth failed", it's a
   coupling glitch: reposition and try again (the password is correct by construction).

## Disclaimer

Provided "as is", for personal use on hardware you own, for interoperability,
repair and educational purposes. You are responsible for complying with your local
laws and any applicable terms. The authors accept no liability for damage to tags,
devices, or anything else. See [LICENSE](LICENSE).

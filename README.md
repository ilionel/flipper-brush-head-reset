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

## Install (pre-built)

Grab the `.fap` for your firmware from the [**latest release**](../../releases/latest) —
`official-release`, `official-dev`, `unleashed` or `momentum` — copy it to
`SD Card / apps / NFC` (via [qFlipper](https://docs.flipper.net/qflipper) or the SD card),
then open **Apps → NFC → Brush Head Reset**. A step-by-step project page is published with
GitHub Pages from [`docs/`](docs/) (URL: `https://<user>.github.io/<repo>/`).

## What it does

Smart brush heads carry a tiny **NTAG213** NFC tag (NfcA, 13.56 MHz). One page
holds the accumulated **brushing time in seconds**; the handle reads it to decide
when to nag you for a replacement. This app reads that page, shows the usage, and —
with the head's own per-head password — can write it back to a value of your choice
(0 = brand new).

- **Read brush head** — the recognised family name (or the raw model code), a usage
  gauge (% and minutes) and the computed password. Press **Right** for a detail page
  (UID, MFG, raw seconds, PWD); **OK** saves the read to the SD card.
- **Reset to new** — set the counter back to 0%.
- **Save / Restore** — store a read to the SD card and write it back later.
- **Advanced**
  - **Set usage %** / **Set usage min** — write any target you want.
  - **Password calc** — compute a head's password from its UID + MFG code (offline).
  - **Models** — list the brush-head families the app recognises.
- **About**.

### Model / brand database

Each head stores an NDEF URL identifying its ecosystem. On a read, the app matches
that URL against a small registry (`soniclear_models.c`) to show the **family name**
(e.g. *Sonicare BrushSync*) and to use that family's **rated life** for an accurate
usage percentage. Unknown heads still work and show their raw MFG code.

Adding a brand is one table row — a unique substring of its NDEF URL, a friendly
name and the rated brushing life in seconds. Contributions welcome.

Every write (Reset / Set usage / Restore) first shows a **confirmation screen** with the
target value; you confirm *before* placing the head, then make a single still placement
for the whole read → authenticate → write → verify session.

The write password is **computed** from the tag's UID + factory code, so it is never
guessed. The tag only ever processes a single *accepted* authentication; a genuine
rejection is never retried (see [Safety](#safety-against-permanent-lockout)), so the
tag's 3-failed-attempts permanent lockout can't be triggered by normal use.

## How the counter is encoded

Reverse-engineered from real heads (no proprietary code used). The **full page-by-page
layout, field encodings and lock structure are in [MEMORY_MAP.md](MEMORY_MAP.md)**; the
essentials:

| Item | Detail |
|------|--------|
| Tag | NTAG213, 7-byte UID, 45 pages |
| Counter | page `0x24`, bytes 0-1 = brushing **seconds**, little-endian 16-bit |
| Page 0x24 bytes 2-3 | last-session **intensity** + **brush mode** (data, preserved on write — not a fixed frame) |
| Fresh head | counter `00 00` (bytes 2-3 left as-is) |
| "Replace" threshold | rated life at page `0x21` = ~`0x5460` = 21600 s = 180 × 2-min sessions ≈ 3 months |
| Model | page `0x1F` byte 2 = brush-head **type** (`0x01–0x16`) |
| Checksum | none on the counter |
| Protection | pages ≥ 16 are password-write-protected (`AUTH0`), `AUTHLIM = 3`; identity/life block (32-35) and NDEF (4-15) are **locked** |

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

## Host tool

`soniclear.py` (Python 3, standard library only) computes the password and decodes the
counter offline, from a Flipper `.nfc` dump, a `.soniclear` record saved by the app, or
raw values:

```sh
python3 soniclear.py dump.nfc
python3 soniclear.py head.soniclear
python3 soniclear.py --uid 04112233445566 --mfg "010203 99Z"
```

## Usage

1. Pop the brush head off the handle.
2. Open **NFC → Brush Head Reset** and pick an action.
3. Lay the **base of the head flat on the back of the Flipper** and **hold it still**
   until the operation finishes. The head's coil is tiny — stable coupling matters,
   especially for a write. Transient coupling drops are retried automatically; if a
   write still ends with "No tag answer", just reposition and try again (the password
   is correct by construction, so this is harmless).

## Safety against permanent lockout

The tag is configured with `AUTHLIM = 3`: after **3 consecutive failed**
`PWD_AUTH` attempts it locks protected memory **permanently**. A *successful*
auth resets that counter. This app is built so the limit is never reachable:

- The password is **computed** from the head (UID + MFG), never guessed.
- A write authenticates **once**. A transport error (timeout / lost coupling)
  never reaches the tag, so it does not count toward the limit — only these are
  retried automatically. A genuine *key rejection* (the tag answered "no") is
  **never** retried.
- So in normal use you will see at most coupling retries ("No tag answer"),
  which are harmless. "Password rejected" should never appear on a supported
  head; if it does, stop and check the UID/MFG rather than retrying blindly.

## Disclaimer

Provided "as is", for personal use on hardware you own, for interoperability,
repair and educational purposes. You are responsible for complying with your local
laws and any applicable terms. The authors accept no liability for damage to tags,
devices, or anything else. See [LICENSE](LICENSE).

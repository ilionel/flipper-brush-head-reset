# Smart Toothbrush Head — NTAG213 Memory Map

Reverse-engineered layout of the **NTAG213** NFC tag embedded in the base of a smart
toothbrush head (compatible with Philips Sonicare BrushSync). Derived from real heads
(diffing the same head over time and across heads) and cross-checked with public
reverse-engineering ([mbirth, 2026-03](https://blog.mbirth.uk/2026/03/29/sonicare-brush-head-nfc-data.html),
[nfc.cool](https://nfc.cool/blog/reset-sonicare-brush-head-nfc/)) and the
[NXP NTAG213 datasheet](https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf).

> Example UID / MFG / PWD below are **placeholders**. Lock/config bytes, the rated-life
> value and the type table are model constants (not personal data).

## Chip basics

| Property | Value |
|----------|-------|
| IC | NTAG213 (NT2H1311), NFC Forum Type 2 |
| RF | ISO/IEC 14443-3 Type A (NfcA), 13.56 MHz |
| Memory | 45 pages × 4 bytes = 180 bytes (user area 144 B) |
| UID | 7 bytes, double-size, starts `04` (NXP) |
| Write granularity | 1 page (4 bytes) |
| Lock granularity | static: 1 page (3-15); dynamic: 2 pages (16-39) |

The head's tiny tuned coil couples reliably only to a small reader antenna (a Flipper
Zero works; a full-size Proxmark3 antenna struggles).

## Full page map

`R/W` = our write-probe result on a real head (read-back-identical test).
`Lk` = how it is protected. Values shown are a representative head.

| Page | Hex | Bytes | Field | R/W | Lk |
|-----:|-----|-------|-------|-----|----|
| 0  | 00 | `UID0 UID1 UID2 BCC0` | UID bytes 0-2 + check byte BCC0 | — | factory ro |
| 1  | 01 | `UID3 UID4 UID5 UID6` | UID bytes 3-6 | — | factory ro |
| 2  | 02 | `BCC1 INT  L0  L1` | BCC1, internal, **static lock bytes** (L0,L1 lock pages 3-15) | — | factory |
| 3  | 03 | `E1 10 .. ..` | **Capability Container** (NDEF CC) / OTP | — | OTP |
| 4–12 | 04–0C | NDEF | **NDEF message**: TLV `03 20` + URI record, prefix `https://www.` + `philips.com/nfcbrushheadtap` | 🔒 | static lock |
| 13–15 | 0D–0F | NDEF tail | NDEF terminator `FE` + padding | 🔒 | static lock |
| 16–30 | 10–1E | user | free user space (mostly `00`) | ✅ | — |
| 31 | 1F | `00 FL TY 00` | `FL` = unknown flag (`00`/`01`); **`TY` = brush-head TYPE** (`0x01–0x16`) | ✅ | — |
| 32 | 20 | `?? ?? U2 U3` | unknown (`U2`,`U3` ∈ {`00`,`02`,`04`} / {`01`,`02`}); "read-only" | 🔒 | dyn lock |
| 33 | 21 | `LO HI M0 M1` | **`LO HI` = RATED LIFE** (LE16, `0x5460`=21600 s); `M0 M1` = first 2 ASCII of MFG | 🔒 | dyn lock |
| 34–35 | 22–23 | ASCII | **MFG code** (rest), e.g. `DDMMYY NNL` | 🔒 | dyn lock |
| 36 | 24 | `CL CH IN MO` | **`CL CH` = WEAR COUNTER** (brushing seconds, LE16); `IN` = last intensity; `MO` = last brush mode | ✅ | — |
| 37 | 25 | `.. .. PH ..` | `PH` = per-head field (stable `00`/`03`) | ✅ | — |
| 38 | 26 | handle | **LE32 written by the handle** — looks like a Unix epoch but is inconsistent with the MFG date (see below) | ✅ | — |
| 39 | 27 | `.. .. .. 01` | byte 3 = `01` (unknown) | ✅ | — |
| 40 | 28 | `D0 D1 BL RFU` | **Dynamic Lock Bytes** (D0,D1 = lock bits; BL = block-lock) | — | — |
| 41 | 29 | `MIR RFU MP A0` | **CFG0**: MIRROR, MIRROR_PAGE, **`A0` = AUTH0** | — | cfg |
| 42 | 2A | `ACC RFU RFU RFU` | **CFG1**: **`ACC` = ACCESS** (PROT/CFGLCK/AUTHLIM) | — | cfg |
| 43 | 2B | `PWD0..PWD3` | **password** (write-only, reads as `00`) | — | wo |
| 44 | 2C | `PK0 PK1 RFU RFU` | **PACK** (auth ack) + RFUI | — | — |

## Field details

### Brush-head type — page 0x1F byte 2
The single most useful identity field: a 1-byte model code.

| Type | Model | Type | Model |
|------|-------|------|-------|
| 0x01 | Premium Plaque Defence, White | 0x0C | InterCare, White |
| 0x02 | Premium Plaque Defence, Black | 0x0D | InterCare (small), White |
| 0x03 | Premium Gum Care, White | 0x0E | TongueCare+, White |
| 0x04 | Premium Gum Care, Black | 0x0F | TongueCare+, Black |
| 0x05 | Premium White, White | 0x10 | Premium All-in-One, White |
| 0x06 | Premium White, Black | 0x11 | Premium All-in-One, Black |
| 0x07 | Optimal Plaque Defence, White | 0x12 | SimplyClean, White |
| 0x08 | Optimal Gum Care, White | 0x13 | ProResults, White |
| 0x09 | Optimal White, White | 0x14 | Sensitive, White |
| 0x0A | Optimal White, Black | 0x15 | Sensitive, Black |
| 0x0B | Optimal White (small), White | 0x16 | Gentle Clean, White |

### Rated life — page 0x21 bytes 0-1
Little-endian 16-bit **total brushing seconds** the head is rated for. `0x5460` = 21600 s
= 360 min = 180 sessions of 2 min ≈ **3 months** at 2 brushings/day × 2 min (= 4 min/day,
the basis of Philips' "replace every 3 months"). This is the denominator for the usage %.

### Manufacturing code — pages 0x21 b2-3 .. 0x23
10 ASCII chars `DDMMYY NNL`: manufacturing date + production line, e.g. `241206 31K`. The
first 6 chars are always digits (used as a sanity/validity check). Bytes 2-3 of page 0x21
hold the first two characters, so a write to the rated-life field must preserve them.

### Wear counter — page 0x24
- **bytes 0-1**: accumulated **brushing time in seconds**, little-endian 16-bit. Fresh head
  = `0`. Saturates at `0xFFFF` (65535 s ≈ 1092 min). **No checksum.** The handle reads this
  and nags for a replacement once it reaches the rated life.
- **byte 2 — last intensity**: `0x00` Low, `0x01` Med, `0x02` High.
- **byte 3 — last brush mode**: `0x00` Clean, `0x01` White+, `0x02` Gum Health,
  `0x03` Deep Clean+, `0x04` Sensitive.

> Resetting a head = write bytes 0-1 = `00 00`, **preserving** bytes 2-3 (intensity/mode are
> data, not a fixed frame — they are overwritten by the handle on the next brushing anyway).

### Partially-characterised / unknown fields
No public source decodes these; the notes below come from diffing two heads of the same
model (`0x13`) plus the same head over time, and from the lock probe.

| Field | Observation | Best guess |
|-------|-------------|------------|
| 0x1F b1 (`FL`) | `0x01`, constant across heads | a fixed flag (NDEF/feature marker?) |
| 0x20 (page 32) | `00 00 00 02`, constant across heads; **locked** | model/variant config (mbirth: b2∈{00,02,04}, b3∈{01,02} vary by model) |
| 0x25 b2 (page 37) | `0x00` on one head, `0x03` on another (same model); writable | **per-head** attribute (production bin / sub-variant) |
| 0x26 (page 38) | LE32 epoch written by the handle = **"last brushing timestamp"**. Designed for the Bluetooth-connected Sonicare models (DiamondClean Smart, etc.) whose handle syncs the clock from the phone app — there it is a real date used for the app's brushing history. On a **standalone, never-connected handle** the clock is unsynced, so the value is a fixed firmware-epoch base (our dumps cluster on **2024-02-29 01h–05h UTC** = base ≈ 2024-02-29 00:00 + a few hours of session uptime), unrelated to the real date (it even predates the head's 2025 MFG date). The same head/firmware NFC schema is shared across the whole product line, connected or not. | last-brushing timestamp (only meaningful on a time-synced/BT handle) |
| 0x27 b3 (page 39) | `0x01`, constant across heads | unknown constant |

These don't affect reset or the usage %, so they are read-only-of-interest. Künzi noted
"all other readable data is identical between all heads" — true for most, but `0x25 b2`
(per-head) and `0x26` (handle-updated) do vary.

## Locking

### Static lock (page 0x02 bytes 2-3)
Locks the CC and the NDEF area: a representative head reads `FF FF`, i.e. **pages 3-15 are
permanently read-only**. This is the NDEF URL region, fixed at manufacture.

### Dynamic lock (page 0x28)
Covers pages 16-39 with a **granularity of 2 pages** (NXP datasheet):

| Byte | Bit → pages |
|------|-------------|
| 0 (D0) | b0→16-17, b1→18-19, b2→20-21, b3→22-23, b4→24-25, b5→26-27, b6→28-29, b7→30-31 |
| 1 (D1) | b0→32-33, b1→34-35, b2→36-37, b3→38-39 |
| 2 (BL) | block-lock bits: freeze the lock bits themselves |

A representative head reads `00 03 30 BD`:
- D0 = `0x00` → pages **16-31 unlocked** (writable).
- D1 = `0x03` (bits 0,1) → **pages 32-35 locked**; bits 2,3 = 0 → pages **36-39 unlocked**.
- BL = `0x30` → the lock configuration is frozen.

So the factory locks exactly the **identity/rated-life block (pages 32-35)** while leaving
the **wear counter (page 36) writable** — the counter must be writable because the handle
increments it. Dynamic-lock bits are **one-way**: once set they cannot be cleared.

### Net writability (verified by probe)
- 🔒 **Locked**: pages 4-15 (NDEF) and 32-35 (identity + rated life).
- ✅ **Writable** (with the password): 16-31 (incl. the type byte) and 36-39 (counter, session, per-head).

## Configuration & access (pages 0x29–0x2A)

| Field | Page.byte | Value | Meaning |
|-------|-----------|-------|---------|
| AUTH0 | 0x29 b3 | `0x10` | first password-protected page = 16 (pages ≥16 need auth **to write**) |
| PROT | 0x2A b0 bit7 | `0` | password required for **write only** (reads are free) |
| CFGLCK | 0x2A b0 bit6 | `1` | configuration pages locked (AUTH0/ACCESS/PWD frozen) |
| AUTHLIM | 0x2A b0 bits2-0 | `3` | after **3 consecutive failed** PWD_AUTH the tag locks **permanently** |

Because reads are free (PROT=0) the whole tag can be dumped without the password; only
writes to pages ≥16 need it.

## Password (pages 0x2B / 0x2C)

The 4-byte `PWD` is write-only (reads back as `00 00 00 00`); `PACK` is the 2-byte
acknowledge the tag returns on a correct `PWD_AUTH`. The password is **per-head and
computed** (never brute-forced) from the UID + MFG code with two CRC16-CCITT passes
(algorithm by [@ATC1441](https://github.com/atc1441)):

```
c  = crc16_ccitt(init=0x49A3, UID[7])          # poly 0x1021, no reflection
c |= crc16_ccitt(init=c_low, MFG[10]) << 16
PWD = byteswap32(c)                            # ((c>>8)&0x00FF00FF) | ((c<<8)&0xFF00FF00)
```

A *successful* auth resets the AUTHLIM fail counter, so as long as the computed password is
correct the 3-strikes permanent lock can never be reached. Tools here send `PWD_AUTH`
exactly once and never retry it.

## Practical summary

| Goal | Page(s) | Possible? |
|------|---------|-----------|
| Read everything | all | ✅ no password needed (PROT=0) |
| Reset wear to "new" | 0x24 b0-1 = `00 00` | ✅ writable (with PWD) |
| Set any usage value | 0x24 b0-1 | ✅ |
| Reprogram rated life | 0x21 b0-1 | ❌ page 33 dynamically locked (32-35) |
| Change declared model/type | 0x1F b2 | ✅ writable (reversible) |
| Change MFG / identity | 0x21-0x23 | ❌ locked |
| Change the NDEF URL | 0x04-0x0F | ❌ static-locked |

## Sources
- [NXP NTAG213/215/216 datasheet](https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf)
- [mbirth — Sonicare brush head NFC data (2026-03)](https://blog.mbirth.uk/2026/03/29/sonicare-brush-head-nfc-data.html)
- [nfc.cool — reset Sonicare brush head](https://nfc.cool/blog/reset-sonicare-brush-head-nfc/)
- Password algorithm: [@ATC1441](https://github.com/atc1441)

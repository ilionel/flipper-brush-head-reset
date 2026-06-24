# Changelog

## 1.12
- **Set life moved to Advanced** (it is a niche/experimental write, and the lock
  probe confirmed page 33 is locked on these heads, so it rarely succeeds) — the
  main menu is back to Read / Reset / Save / Restore / Advanced / About.
- **Advanced -> Change type.** Writes the brush-head type byte (page 31, which the
  lock probe found writable) to any model value 0x01-0x16, shown with its model
  name. Bytes 0,1,3 of the page are preserved; one-shot auth; verified. It is
  **reversible** (page 31 is writable — just write the original type back). This is
  the one lead for influencing the handle's behaviour now that the rated-life field
  (page 33) is confirmed locked: if the handle derives its replace-threshold from
  the head type rather than the on-tag life, re-typing to a longer-life model could
  extend usable life (experimental, untested; it also misrepresents the head).

## 1.11
- **Lock probe now loops by itself.** Instead of re-launching the probe once per
  page (a tiny coupling window only resolves ~1 page per placement), a single run
  keeps the reader polling and advances the map on **every re-coupling** of the
  head: read the identity once, then each coupling window probes more still-unknown
  pages, accumulating until the map is complete or 50 windows are reached. Just hold
  the head near the sweet spot and let it fill in. The scanning screen shows live
  `Resolved N/36` progress; the per-page map and the `probe_<UID>.txt` report are
  still produced at the end. (The cross-run accumulation from before still works too:
  re-running the probe resumes the same head's map.)

## 1.10
- **Advanced -> Lock probe.** Non-destructive write-map: for each user page (4-39)
  it reads the page then writes the SAME four bytes back and classifies the tag's
  answer — write accepted = **reprogrammable**, NAK = **locked/fixed**, timeout =
  coupling lost. Writing identical bytes changes nothing. Pages 0-3 (UID/lock/OTP),
  40 (dynamic lock) and 41-44 (config/PWD/PACK) are never written — writing the PWD
  page would zero the password and brick the head. The screen summarises
  writable/locked/no-answer counts and the verdict for page 33 (rated life) and
  page 36 (counter); a full per-page map is saved to `probe_<UID>.txt` on the SD.
  This settles, on the actual head, whether the rated-life field is writable.
- **Read now names the model.** The brush-head type byte (page 0x1F) is decoded
  against the full type table (0x01-0x16, from mbirth's RE), so a read shows e.g.
  "ProResults W" or "Prem All-in-One W" instead of just the generic family — on the
  result screen and in Head fields.

## 1.9
- **Fix: heads used at non-default settings were rejected.** Page 36 bytes 2-3 are
  not a fixed `02 00` frame — byte 2 is the last-session **intensity** (0=Low,
  1=Med, 2=High) and byte 3 the last **brush mode** (0..4: Clean, White+, Gum,
  Deep Clean+, Sensitive) (per mbirth's 2026-03 reverse-engineering). The old
  validity check required exactly `02 00`, so a head last used at any other
  intensity/mode was flagged "Unknown tag layout" and could not be reset. Validity
  now keys on the MFG date being 6 ASCII digits plus in-range intensity/mode.
- **Counter writes now preserve intensity/mode** instead of forcing `02 00`
  (High/Clean). Reset / Set usage / Restore zero or set the counter without
  clobbering the head's last-session settings.
- **Head fields** now shows the decoded **Int** and **Mode**, the rated life in
  months, and the **Type** byte without the tentative `?` (it is confirmed to be
  the brush-head type, 0x01-0x16, e.g. 0x10 = Premium All-in-One White).

## 1.8
- **Main menu -> Set life (experimental).** Programs the head's rated-life field
  (page 33 bytes 0-1, LE16) from 100% to 300% of the nominal ~3-month life, in
  25% steps. The chooser shows the target in **months** at an average 4 min/day
  (2 brushings x 2 min — the basis of the "replace every 3 months" rating, so
  100% = 3.0 months), e.g. `150% = 4.5 mo`, plus the session count.
- The write re-reads page 33 under the open auth session and **preserves bytes
  2-3** (the first two ASCII chars of the MFG code share this page), then writes
  and verifies. Same one-shot `PWD_AUTH` lockout discipline as the counter write.
- **Result: page 33 (rated life) is write-locked — confirmed rigorously.** `Set life`
  reaches its write only after `open_auth` returns true, i.e. the tag answered the
  PWD_AUTH with a PACK (password accepted); it then reads page 33 (tag is coupled)
  and writes it back. On a real head this write returns a NAK (`Write refused-
  locked?`) = **authenticated + coupled + write refused** — the case that rules out
  both the "unauthenticated NAK" and "lost coupling" false positives. So the rated
  life is NOT reprogrammable; only the counter (page 36) is writable. (Earlier notes
  that flip-flopped rested on weaker evidence — an ambiguous generic failure and an
  uncertain lock-byte decode; this auth-confirmed NAK supersedes them.) `Set life`
  is therefore effectively a lock diagnostic. The instrumented messages distinguish
  a tag refusal (`refused-locked?`, NAK) from a lost coupling (`lost-hold still`).
- **Backup/Restore now round-trips the rated life.** Saved records store the
  actual on-tag life, and Restore rewrites both the wear counter and the saved
  life in one auth session — so a backup taken before Set-life fully returns the
  head to its original state. Old records without a `Life:` line restore the
  counter only (life left untouched), so nothing breaks.

## 1.7
- **Usage % now reads the head's own rated life.** The head stores its rated
  brushing budget at page 33 bytes 0-1 (LE16) — verified `0x5460` = 21600 s on
  every real head dumped. The read worker now uses that on-tag value for the
  percentage (falling back to the family registry / default only for an odd or
  missing read), so the % is correct for any head, not just default-life ones.
- **Head fields** now shows `Life` (the on-tag rated seconds) and `Type?` (the
  page-31 type/colour byte). `Type?` keeps its question mark: it is consistent
  with open-source notes but cannot be proven from a single model in hand, so it
  is presented tentatively. The handle timestamp / secondary field were dropped
  from this screen (still read, just not shown) — they did not survive dump
  verification as meaningful, and the on-tag life/type are the solid additions.

## 1.6
- **Advanced -> Head fields.** Reads a head and shows the raw on-tag data decoded:
  the model/batch code (shared by heads of the same family), the handle-written
  timestamp (page 38), the secondary field (page 37), and the NTAG213 security
  config — `AUTH0` (first password-protected page), `AUTHLIM` (the 3-strikes
  write-lockout counter) and `PROT`. Verified against real heads, including that
  the computed password matches the key actually stored on the tag.
- The read worker now also fetches pages 37-42 on the full read path (best-effort;
  a miss never fails the read). The write path is unchanged (still skips them to
  keep the read->auth window short).

## 1.5
- **Saved records now match the screen.** The usage % written to the `.soniclear`
  file is computed from the head's recognised rated life (family-aware), instead
  of always using the default ~3-month life. Previously a recognised head with a
  non-default life was saved with a percentage that disagreed with what was just
  shown on screen.
- The saved record gains a `Life:` line (the rated brushing seconds used for the
  percentage) and, when the family is recognised, a `Family:` line — so each file
  is self-describing.

## 1.4
- **Fix: writes never completed.** Auth success is `err == None`; the SDK does
  not set `auth_context.auth_success` for direct callers, so the old check
  always treated a *successful* auth as a failure ("Auth failed" / "Password
  rejected") and skipped the write. The computed password was correct all along
  and was being accepted (which also resets the tag's AUTHLIM counter — no
  lockout ever occurred). Reset/Set now actually write.
- The PWD_AUTH is sent exactly once (the lockout-sensitive step is never
  retried); only the identity read is retried on transient coupling.
- Fix the usage-% being clipped at the screen edge for 3-digit values.

## 1.3
- Model/brand registry (`soniclear_models.c`): the head's NDEF URL is read and
  matched against a table of known families, so a recognised head shows its
  family name (e.g. "Sonicare BrushSync") and the usage % uses that family's
  rated life. Unknown heads fall back to the raw MFG code.
- Advanced -> **Models** lists the known families. The table is easy to extend
  with new brands (a unique NDEF-URL substring + name + rated life).

## 1.2
- Confirmation screen before every write (Reset / Set usage / Restore), showing
  the target as % and minutes; confirm before placing the head so the whole
  read → auth → write happens in one still placement.
- Read result shows a usage gauge; **Right** opens a detail page (UID, MFG, raw
  seconds, PWD), **Left** returns. Unknown tags still show their UID.
- Real (uncapped) usage percentage in text; bar fill capped at 100%.
- Safe auto-retry of reads and of the PWD_AUTH transport on transient coupling
  loss; a real key rejection is never retried, so the AUTHLIM lockout stays
  unreachable. Write failures report "No tag answer" vs "Password rejected".
- Molar-tooth app icon.
- Host tool `soniclear.py` rewritten (argparse; reads `.nfc` dumps and
  `.soniclear` records).

## 1.1
- Single-session writes (read → auth → write in one placement).
- Trademark-safe packaging: neutral naming, README disclaimer, open-source license.

## 1.0
- Initial Flipper app: read & reset the NTAG213 brush-head wear counter,
  on-device password calculator, save/restore.

# Changelog

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
- Trademark-safe packaging: neutral naming, README disclaimer, MIT license.

## 1.0
- Initial Flipper app: read & reset the NTAG213 brush-head wear counter,
  on-device password calculator, save/restore.

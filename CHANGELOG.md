# Changelog

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

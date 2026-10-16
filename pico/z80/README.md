# Z80 CPU emulator (for the tape converter)

- `Z80.c`, `API/Z80.h`: redcode/Z80, https://github.com/redcode/Z80, commit 1d5e1a2 (2026-10-14).
  Copyright (C) Manuel Sainz de Baranda y Goñi. GNU LGPL version 3 or later (`COPYING.LESSER`).
- `zeta/`: the headers of redcode/Zeta that Z80.c uses, https://github.com/redcode/Zeta,
  commit 93ba5ab. Copyright (C) 2006-2025, same author. GNU LGPL version 3.

Unchanged copies. Built with `Z80_STATIC` (no shared-library export macros).
The GNU LGPL lets this be part of the GPLv3 PicoMenu firmware. It is listed, with the
full LGPL text, in THIRD-PARTY-NOTICES.txt (section 8).

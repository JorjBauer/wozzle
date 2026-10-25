// Names for well-known //e addresses: zero page, the page 3 vectors, the
// soft switches, and the Applesoft and monitor ROM entry points. This is
// a copy of the built-in table in aiie-electron's src/debugger/symbols.ts,
// so a disassembly here reads the same as it does in aiie's debugger; a
// change to one belongs in the other.

export const BUILTIN: Record<number, string> = {
  // zero page
  0x20: 'WNDLFT', 0x21: 'WNDWDTH', 0x22: 'WNDTOP', 0x23: 'WNDBTM',
  0x24: 'CH', 0x25: 'CV', 0x26: 'GBASL', 0x27: 'GBASH', 0x28: 'BASL', 0x29: 'BASH',
  0x2c: 'H2', 0x2d: 'V2', 0x30: 'COLOR', 0x32: 'INVFLG', 0x33: 'PROMPT',
  0x36: 'CSWL', 0x37: 'CSWH', 0x38: 'KSWL', 0x39: 'KSWH',
  0x4e: 'RNDL', 0x4f: 'RNDH', 0x67: 'TXTTAB', 0x69: 'VARTAB', 0x6b: 'ARYTAB', 0x6d: 'STREND',
  0x6f: 'FRETOP', 0x73: 'HIMEM', 0x75: 'CURLIN', 0xaf: 'PRGEND', 0xe0: 'HGRX', 0xe2: 'HGRY',
  // page 3 vectors
  0x3d0: 'DOSWARM', 0x3d3: 'DOSCOLD', 0x3ea: 'DOSVEC', 0x3f0: 'BRKV', 0x3f2: 'SOFTEV', 0x3f4: 'PWREDUP',
  0x3f5: 'AMPERV', 0x3f8: 'USRADR', 0x3fb: 'NMIV', 0x3fe: 'IRQV',
  // soft switches
  0xc000: 'KBD', 0xc001: 'SET80STORE', 0xc002: 'RDMAINRAM', 0xc003: 'RDCARDRAM', 0xc004: 'WRMAINRAM', 0xc005: 'WRCARDRAM',
  0xc006: 'SETSLOTCXROM', 0xc007: 'SETINTCXROM', 0xc008: 'SETSTDZP', 0xc009: 'SETALTZP', 0xc00a: 'SETINTC3ROM', 0xc00b: 'SETSLOTC3ROM',
  0xc00c: 'CLR80VID', 0xc00d: 'SET80VID', 0xc00e: 'CLRALTCHAR', 0xc00f: 'SETALTCHAR',
  0xc010: 'KBDSTRB', 0xc011: 'RDLCBNK2', 0xc012: 'RDLCRAM', 0xc013: 'RDRAMRD', 0xc014: 'RDRAMWRT', 0xc015: 'RDCXROM',
  0xc016: 'RDALTZP', 0xc017: 'RDC3ROM', 0xc018: 'RD80STORE', 0xc019: 'RDVBL', 0xc01a: 'RDTEXT', 0xc01b: 'RDMIXED',
  0xc01c: 'RDPAGE2', 0xc01d: 'RDHIRES', 0xc01e: 'RDALTCHAR', 0xc01f: 'RD80VID',
  0xc030: 'SPKR', 0xc050: 'TXTCLR', 0xc051: 'TXTSET', 0xc052: 'MIXCLR', 0xc053: 'MIXSET',
  0xc054: 'LOWSCR', 0xc055: 'HISCR', 0xc056: 'LORES', 0xc057: 'HIRES', 0xc05e: 'SETDHIRES', 0xc05f: 'CLRDHIRES',
  0xc060: 'CASSIN', 0xc061: 'BUTN0', 0xc062: 'BUTN1', 0xc063: 'BUTN2', 0xc064: 'PADDL0', 0xc065: 'PADDL1',
  0xc066: 'PADDL2', 0xc067: 'PADDL3', 0xc070: 'PTRIG',
  0xc080: 'LCBANK2RD', 0xc081: 'ROMIN', 0xc082: 'LCBANK2ROM', 0xc083: 'LCBANK2',
  0xc088: 'LCBANK1RD', 0xc089: 'ROMIN1', 0xc08a: 'LCBANK1ROM', 0xc08b: 'LCBANK1',
  0xc0e0: 'PHASEOFF', 0xc0e8: 'MOTOROFF', 0xc0e9: 'MOTORON', 0xc0ea: 'DRV0EN', 0xc0eb: 'DRV1EN',
  0xc0ec: 'Q6L', 0xc0ed: 'Q6H', 0xc0ee: 'Q7L', 0xc0ef: 'Q7H',
  // 80-column firmware
  0xc300: 'C3ROM', 0xc305: 'BASICIN', 0xc307: 'BASICOUT',
  // Applesoft
  0xd683: 'AS:CHKCOM', 0xdd7b: 'AS:FRMEVL', 0xdec9: 'AS:SYNCHR', 0xe6f8: 'AS:GETBYT', 0xe752: 'AS:GETADR',
  0xe7a0: 'AS:FADD', 0xe7be: 'AS:FSUB', 0xe97f: 'AS:FMULT', 0xea66: 'AS:FDIV', 0xeafa: 'AS:LOAD', 0xeb2b: 'AS:FSTORE',
  0xed34: 'AS:FOUT', 0xf1e5: 'AS:HGR2', 0xf3e2: 'AS:HGR', 0xf3f6: 'AS:HCLR', 0xf6ec: 'AS:HPOSN', 0xf6fe: 'AS:HPLOT',
  0xf730: 'AS:HLIN', 0xf7e7: 'AS:DRAW', 0xf65d: 'AS:XDRAW',
  // F8 ROM
  0xf800: 'F8ROM:PLOT', 0xf80e: 'F8ROM:PLOT1', 0xf819: 'F8ROM:HLINE', 0xf828: 'F8ROM:VLINE', 0xf832: 'F8ROM:CLRSCR',
  0xf836: 'F8ROM:CLRTOP', 0xf847: 'F8ROM:GBASCALC', 0xf85f: 'F8ROM:NEXTCOL', 0xf864: 'F8ROM:SETCOL', 0xf871: 'F8ROM:SCRN',
  0xf941: 'F8ROM:PRNTAX', 0xf948: 'F8ROM:PRBLNK', 0xf94a: 'F8ROM:PRBL2', 0xfa62: 'F8ROM:RESET', 0xfaa6: 'F8ROM:PWRUP',
  0xfb1e: 'F8ROM:PREAD', 0xfb2f: 'F8ROM:INIT', 0xfb39: 'F8ROM:SETTXT', 0xfb40: 'F8ROM:SETGR', 0xfb4b: 'F8ROM:SETWND',
  0xfb5b: 'F8ROM:TABV', 0xfb60: 'F8ROM:APPLEII', 0xfbc1: 'F8ROM:BASCALC', 0xfbd9: 'F8ROM:BELL1', 0xfbdd: 'F8ROM:BELL1.2',
  0xfbe4: 'F8ROM:BELL2', 0xfbf4: 'F8ROM:ADVANCE', 0xfbfd: 'F8ROM:VIDOUT', 0xfc10: 'F8ROM:BS', 0xfc1a: 'F8ROM:UP',
  0xfc22: 'F8ROM:VTAB', 0xfc24: 'F8ROM:VTABZ', 0xfc42: 'F8ROM:CLREOP', 0xfc58: 'F8ROM:HOME', 0xfc62: 'F8ROM:CR',
  0xfc66: 'F8ROM:LF', 0xfc70: 'F8ROM:SCROLL', 0xfc9c: 'F8ROM:CLREOL', 0xfc9e: 'F8ROM:CLEOLZ', 0xfca8: 'F8ROM:WAIT',
  0xfcb4: 'F8ROM:NXTA4', 0xfcba: 'F8ROM:NXTA1', 0xfcc9: 'F8ROM:HEADR', 0xfd0c: 'F8ROM:RDKEY', 0xfd18: 'F8ROM:KEYIN',
  0xfd1b: 'F8ROM:KEYIN2', 0xfd35: 'F8ROM:RDCHAR', 0xfd67: 'F8ROM:GETLNZ', 0xfd6a: 'F8ROM:GETLN', 0xfd6f: 'F8ROM:GETLN1',
  0xfd8e: 'F8ROM:CROUT', 0xfd92: 'F8ROM:PRA1', 0xfdda: 'F8ROM:PRBYTE', 0xfde3: 'F8ROM:PRHEX', 0xfded: 'F8ROM:COUT',
  0xfdf0: 'F8ROM:COUT1', 0xfe2c: 'F8ROM:MOVE', 0xfe36: 'F8ROM:VERIFY', 0xfe5e: 'F8ROM:LIST', 0xfe80: 'F8ROM:SETINV',
  0xfe84: 'F8ROM:SETNORM', 0xfe89: 'F8ROM:SETKBD', 0xfe93: 'F8ROM:SETVID', 0xfeb6: 'F8ROM:GO', 0xfefd: 'F8ROM:READ',
  0xff2d: 'F8ROM:PRERR', 0xff3a: 'F8ROM:BELL', 0xff3f: 'F8ROM:RESTORE', 0xff4a: 'F8ROM:SAVE', 0xff58: 'F8ROM:IORTS',
  0xff59: 'F8ROM:OLDRST', 0xff65: 'F8ROM:MON', 0xff69: 'F8ROM:MONZ', 0xffa7: 'F8ROM:GETNUM',
  0xfffa: 'NMIVEC', 0xfffc: 'RSTVEC', 0xfffe: 'IRQVEC',
};

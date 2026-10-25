import { BUILTIN } from './symbols';

// Two ways to look at a binary file: a hex dump, and a 65C02 disassembly.
// Both number the lines from the address the file loads at, so they line
// up with what you'd see in the monitor.

// Where a file loads: a B (BIN) file's aux type is its load address, and
// a ProDOS SYS file always loads at $2000. Anything else starts at 0.
export function loadAddress(fsType: string | null, fileType: number, auxType: number): number {
  if (fsType === 'pascal') return 0;
  if (fileType === 0x06) return auxType;
  if (fileType === 0xff && fsType === 'prodos') return 0x2000;
  return 0;
}

const h2 = (n: number) => n.toString(16).toUpperCase().padStart(2, '0');
const h4 = (n: number) => n.toString(16).toUpperCase().padStart(4, '0');

// Apple text has the high bit set, so it's stripped before deciding
// whether a byte prints
function shown(b: number): string {
  const c = b & 0x7f;
  return c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : '.';
}

// AAAA xx xx xx xx xx xx xx xx | ........
// Eight bytes a line, like the monitor. Addresses get six digits if the
// file runs past $FFFF.
export function hexDump(bytes: Uint8Array, base: number): string {
  const digits = base + bytes.length > 0x10000 ? 6 : 4;
  const lines: string[] = [];
  for (let at = 0; at < bytes.length; at += 8) {
    const row = bytes.subarray(at, at + 8);
    let hex = '';
    let text = '';
    for (let i = 0; i < 8; i++) {
      hex += i < row.length ? h2(row[i]) + ' ' : '   ';
      if (i < row.length) text += shown(row[i]);
    }
    lines.push(`${(base + at).toString(16).toUpperCase().padStart(digits, '0')} ${hex}| ${text}`);
  }
  return lines.join('\n');
}

// The disassembly matches aiie's debugger (aiie-electron): aiie's opcode
// table, operands padded to the width the instruction implies, addresses
// named from the //e table in symbols.ts, and an Lxxxx label on every
// branch or jump target that has no name. aiie's table writes 6502
// mnemonics in uppercase and the 65C02's additions in lowercase; we show
// them all uppercase and keep the difference as a flag, as the debugger
// does.

type Mode = 'imp' | 'acc' | 'imm' | 'zp' | 'zpx' | 'zpy' | 'abs' | 'absx' | 'absy'
  | 'ind' | 'indx' | 'indy' | 'zpind' | 'absindx' | 'rel' | 'zprel' | 'ill';

const operandBytes: Record<Mode, number> = {
  imp: 0, acc: 0, imm: 1, zp: 1, zpx: 1, zpy: 1, abs: 2, absx: 2, absy: 2,
  ind: 2, indx: 1, indy: 1, zpind: 1, absindx: 2, rel: 1, zprel: 2, ill: 0,
};

// Generated from aiie's opnames[] (disassembler.cpp) and opcodes[]
// (cpu.cpp): opcode, mnemonic, addressing mode. That includes the Rockwell
// bit instructions (BBR, BBS, RMB, SMB), WAI at $CB, and the undocumented
// DCP at $DB that aiie's CPU runs.
const table = `
00 BRK imp|01 ORA indx|02 ??? ill|03 ??? ill|04 tsb zp|05 ORA zp|06 ASL zp|07 rmb0 zp
08 PHP imp|09 ORA imm|0A ASL acc|0B ??? ill|0C tsb abs|0D ORA abs|0E ASL abs|0F bbr0 zprel
10 BPL rel|11 ORA indy|12 ora zpind|13 ??? ill|14 trb zp|15 ORA zpx|16 ASL zpx|17 rmb1 zp
18 CLC imp|19 ORA absy|1A inc acc|1B ??? ill|1C trb abs|1D ORA absx|1E ASL absx|1F bbr1 zprel
20 JSR abs|21 AND indx|22 ??? ill|23 ??? ill|24 BIT zp|25 AND zp|26 ROL zp|27 rmb2 zp
28 PLP imp|29 AND imm|2A ROL acc|2B ??? ill|2C BIT abs|2D AND abs|2E ROL abs|2F bbr2 zprel
30 BMI rel|31 AND indy|32 and zpind|33 ??? ill|34 bit zpx|35 AND zpx|36 ROL zpx|37 rmb3 zp
38 SEC imp|39 AND absy|3A dec acc|3B ??? ill|3C bit absx|3D AND absx|3E ROL absx|3F bbr3 zprel
40 RTI imp|41 EOR indx|42 ??? ill|43 ??? ill|44 ??? ill|45 EOR zp|46 LSR zp|47 rmb4 zp
48 PHA imp|49 EOR imm|4A LSR acc|4B ??? ill|4C JMP abs|4D EOR abs|4E LSR abs|4F bbr4 zprel
50 BVC rel|51 EOR indy|52 eor zpind|53 ??? ill|54 ??? ill|55 EOR zpx|56 LSR zpx|57 rmb5 zp
58 CLI imp|59 EOR absy|5A phy imp|5B ??? ill|5C ??? ill|5D EOR absx|5E LSR absx|5F bbr5 zprel
60 RTS imp|61 ADC indx|62 ??? ill|63 ??? ill|64 stz zp|65 ADC zp|66 ROR zp|67 rmb6 zp
68 PLA imp|69 ADC imm|6A ROR acc|6B ??? ill|6C JMP ind|6D ADC abs|6E ROR abs|6F bbr6 zprel
70 BVS rel|71 ADC indy|72 adc zpind|73 ??? ill|74 stz zpx|75 ADC zpx|76 ROR zpx|77 rmb7 zp
78 SEI imp|79 ADC absy|7A ply imp|7B ??? ill|7C jmp absindx|7D ADC absx|7E ROR absx|7F bbr7 zprel
80 bra rel|81 STA indx|82 ??? ill|83 ??? ill|84 STY zp|85 STA zp|86 STX zp|87 smb0 zp
88 DEY imp|89 bit imm|8A TXA imp|8B ??? ill|8C STY abs|8D STA abs|8E STX abs|8F bbs0 zprel
90 BCC rel|91 STA indy|92 sta zpind|93 ??? ill|94 STY zpx|95 STA zpx|96 STX zpy|97 smb1 zp
98 TYA imp|99 STA absy|9A TXS imp|9B ??? ill|9C stz abs|9D STA absx|9E stz absx|9F bbs1 zprel
A0 LDY imm|A1 LDA indx|A2 LDX imm|A3 ??? ill|A4 LDY zp|A5 LDA zp|A6 LDX zp|A7 smb2 zp
A8 TAY imp|A9 LDA imm|AA TAX imp|AB ??? ill|AC LDY abs|AD LDA abs|AE LDX abs|AF bbs2 zprel
B0 BCS rel|B1 LDA indy|B2 lda zpind|B3 ??? ill|B4 LDY zpx|B5 LDA zpx|B6 LDX zpy|B7 smb3 zp
B8 CLV imp|B9 LDA absy|BA TSX imp|BB ??? ill|BC LDY absx|BD LDA absx|BE LDX absy|BF bbs3 zprel
C0 CPY imm|C1 CMP indx|C2 ??? ill|C3 ??? ill|C4 CPY zp|C5 CMP zp|C6 DEC zp|C7 smb4 zp
C8 INY imp|C9 CMP imm|CA DEX imp|CB wai imp|CC CPY abs|CD CMP abs|CE DEC abs|CF bbs4 zprel
D0 BNE rel|D1 CMP indy|D2 cmp zpind|D3 ??? ill|D4 ??? ill|D5 CMP zpx|D6 DEC zpx|D7 smb5 zp
D8 CLD imp|D9 CMP absy|DA phx imp|DB dcp absy|DC ??? ill|DD CMP absx|DE DEC absx|DF bbs5 zprel
E0 CPX imm|E1 SBC indx|E2 ??? ill|E3 ??? ill|E4 CPX zp|E5 SBC zp|E6 INC zp|E7 smb6 zp
E8 INX imp|E9 SBC imm|EA NOP imp|EB ??? ill|EC CPX abs|ED SBC abs|EE INC abs|EF bbs6 zprel
F0 BEQ rel|F1 SBC indy|F2 sbc zpind|F3 ??? ill|F4 ??? ill|F5 SBC zpx|F6 INC zpx|F7 smb7 zp
F8 SED imp|F9 SBC absy|FA plx imp|FB ??? ill|FC ??? ill|FD SBC absx|FE INC absx|FF bbs7 zprel`;

const opcodes: [string, Mode][] = new Array(256);
for (const op of table.split(/[|\n]/)) {
  const [code, name, mode] = op.trim().split(' ');
  if (code) opcodes[parseInt(code, 16)] = [name, mode as Mode];
}

const signed = (b: number) => (b < 0x80 ? b : b - 0x100);

function operand(mode: Mode, p: Uint8Array, at: number, pc: number): string {
  const lo = p[at + 1], hi = p[at + 2];
  const a = h4(lo | (hi << 8));
  switch (mode) {
    case 'imp': case 'ill': return '';
    // aiie prints a one-byte instruction with no operand, A included
    case 'acc': return '';
    case 'imm': return `#$${h2(lo)}`;
    case 'zp': return `$${h2(lo)}`;
    case 'zpx': return `$${h2(lo)},X`;
    case 'zpy': return `$${h2(lo)},Y`;
    case 'abs': return `$${a}`;
    case 'absx': return `$${a},X`;
    case 'absy': return `$${a},Y`;
    case 'ind': return `($${a})`;
    case 'indx': return `($${h2(lo)},X)`;
    case 'indy': return `($${h2(lo)}),Y`;
    case 'zpind': return `($${h2(lo)})`;
    case 'absindx': return `($${a},X)`;
    // Branch targets count from the next instruction
    case 'rel': return `$${h4((pc + 2 + signed(lo)) & 0xffff)}`;
    // BBR/BBS: the zero page byte to test, then the branch target
    case 'zprel': return `$${h2(lo)},$${h4((pc + 3 + signed(hi)) & 0xffff)}`;
  }
}

export interface AsmLine {
  addr: number;
  bytes: string;  // "20 ED FD"
  label: string;  // the line's own name, if it has one
  text: string;   // "JSR    F8ROM:COUT"
  c02: boolean;   // a 65C02-only instruction or addressing form
}

// Instructions whose operand is a code address worth a label
const FLOW = /^(?:JMP|JSR|BCC|BCS|BEQ|BNE|BMI|BPL|BVC|BVS|BRA)\s+\$([0-9A-F]{4})\b/;

// An instruction cut off by the end of the file shows its bytes and ???.
export function disassemble(bytes: Uint8Array, base: number): AsmLine[] {
  const raw: { addr: number; bytes: string; text: string; c02: boolean }[] = [];
  const auto = new Map<number, string>();
  const name = (addr: number) => BUILTIN[addr] ?? auto.get(addr);

  for (let at = 0; at < bytes.length;) {
    const pc = (base + at) & 0xffff;
    const [mn, mode] = opcodes[bytes[at]];
    const len = 1 + operandBytes[mode];
    if (at + len > bytes.length) {
      raw.push({ addr: pc, bytes: Array.from(bytes.subarray(at), h2).join(' '), text: '???', c02: false });
      break;
    }
    const arg = operand(mode, bytes, at, pc);
    const text = arg ? `${mn.toUpperCase()}    ${arg}` : mn.toUpperCase();
    raw.push({ addr: pc, bytes: Array.from(bytes.subarray(at, at + len), h2).join(' '), text,
               c02: mn !== mn.toUpperCase() });
    const m = FLOW.exec(text);
    if (m) {
      const target = parseInt(m[1], 16);
      if (name(target) === undefined) auto.set(target, 'L' + h4(target));
    }
    at += len;
  }

  // Immediates are values, not addresses, so they keep their digits
  const named = (text: string) => text.replace(/(#?)\$([0-9A-F]{2,4})\b/g,
    (whole, imm: string, h: string) => (imm ? whole : name(parseInt(h, 16)) ?? whole));
  return raw.map((l) => ({ ...l, label: name(l.addr) ?? '', text: named(l.text) }));
}

// As plain text, the way aiie's debugger copies a line
export function asmText(lines: AsmLine[]): string {
  const width = Math.max(0, ...lines.map((l) => l.label.length));
  return lines.map((l) => `${h4(l.addr)}  ${l.bytes.padEnd(9)} ${width ? l.label.padEnd(width) + ' ' : ''}${l.text}`)
    .join('\n');
}

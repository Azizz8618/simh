#!/usr/bin/env python3
"""gen_dks_fw.py - "листинговая сборка" (listing assembly) of the ДКС firmware.

Two-pass MACRO-11-subset assembler for PDP11/dks_fw.mac, the СВЯЗЬ7
protocol engine of stage B1.  Produces:

    PDP11/dks_fw.bin  absolute-loader image (LOAD dks_fw.bin; the end
                      block sets PC to the entry)
    PDP11/dks_fw.lst  listing (address, words, source) for review

Why not macro11: the firmware is small (about 250 words), and a bundled
assembler keeps the patch self-contained - the image is reproducible
from the source with stock python3.

Supported subset (everything dks_fw.mac needs and nothing more):

    instructions  MOV MOVB CMP CMPB BIT BITB BIS BISB BIC BICB ADD SUB
                  CLR CLRB COM COMB INC INCB DEC DECB NEG NEGB ADC SBC
                  TST TSTB SWAB ROR ROL ASR ASL (and .B forms) JMP JSR
                  RTS RTI WAIT SOB MUL DIV ASH ASHC
    branches      BR BNE BEQ BMI BPL BVC BVS BCC BCS BGE BLT BGT BLE
                  BHI BLOS BLO BHIS
    addressing    Rn (Rn) (Rn)+ @(Rn)+ -(Rn) X(Rn) @X(Rn) #expr @#expr
                  expr (PC-relative) @expr
    directives    .ASECT  .=expr / . = expr  SYM = expr  labels:
                  .WORD a,b,...  .BYTE a,b,...  .BLKW n  .EVEN
                  .ASCII /text/  .END
    numbers       octal (default), 25. = decimal, 'C = character
    expressions   terms joined by + - * (left to right, * binds first)

Usage: python3 gen_dks_fw.py [source [binout [lstout]]]
"""

import os
import re
import sys

HERE = os.path.dirname (os.path.abspath (__file__))
SRC  = os.path.join (os.path.dirname (HERE), 'dks_fw.mac')
BIN  = os.path.join (os.path.dirname (HERE), 'dks_fw.bin')
LST  = os.path.join (os.path.dirname (HERE), 'dks_fw.lst')

# ---------------------------------------------------------------- lexer

class AsmError (Exception):
    pass

def tokenize (expr):
    """Split an expression into numbers, symbols, '.', 'C, operators."""
    toks = []
    i, n = 0, len (expr)
    while i < n:
        c = expr[i]
        if c.isspace ():
            i += 1
            continue
        if c in '+-*':
            toks.append (c)
            i += 1
            continue
        if c == "'":                        # 'C = character code
            if i + 1 >= n:
                raise AsmError ("висячая кавычка в %r" % expr)
            toks.append (('num', ord (expr[i + 1]) & 0o177))
            i += 2
            continue
        m = re.match (r'[0-9]+\.', expr[i:])
        if m:                               # decimal
            toks.append (('num', int (m.group (0)[:-1], 10)))
            i += len (m.group (0))
            continue
        m = re.match (r'[0-7]+', expr[i:])
        if m:                               # octal
            toks.append (('num', int (m.group (0), 8)))
            i += len (m.group (0))
            continue
        m = re.match (r'[A-Za-z_.][A-Za-z0-9_.]*', expr[i:])
        if m:                               # symbol or '.'
            toks.append (('sym', m.group (0)))
            i += len (m.group (0))
            continue
        raise AsmError ("не понимаю %r в %r" % (expr[i:], expr))
    return toks

def read_term (toks, i, symbols, pc):
    t = toks[i]
    if t == ('sym', '.'):
        return pc, i + 1
    if isinstance (t, tuple) and t[0] == 'num':
        return t[1], i + 1
    if isinstance (t, tuple) and t[0] == 'sym':
        name = t[1]
        if name not in symbols:
            raise AsmError ("неопределённый символ %s" % name)
        return symbols[name], i + 1
    if t == '-':                            # unary minus
        v, i = read_term (toks, i + 1, symbols, pc)
        return (-v) & 0o177777, i
    raise AsmError ("ожидался член, получено %r" % (t,))

def eval_expr (expr, symbols, pc):
    """Terms bound by *, then + and - strictly left to right."""
    toks = tokenize (expr)
    terms = []                              # (sign, value)
    sign = 1
    val = None
    i = 0
    while i < len (toks):
        t = toks[i]
        if t == '+':
            if val is not None:
                terms.append ((sign, val))
                val = None
            sign = 1
            i += 1
        elif t == '-':
            if val is not None:
                terms.append ((sign, val))
                val = None
            sign = -1
            i += 1
        elif t == '*':
            if val is None:
                raise AsmError ("* без левого члена в %r" % expr)
            val = val * read_term (toks, i + 1, symbols, pc)[0]
            i = i + 2                       # the term is one token here
        else:
            if val is not None:
                terms.append ((sign, val))
            val, i = read_term (toks, i, symbols, pc)
    if val is not None:
        terms.append ((sign, val))
    total = 0
    for s, v in terms:
        total += s * v
    return total & 0o177777

# ------------------------------------------------------------- operands

REGS = {'R0': 0, 'R1': 1, 'R2': 2, 'R3': 3, 'R4': 4, 'R5': 5,
        'SP': 6, 'PC': 7, 'R6': 6, 'R7': 7}

class Operand:
    """mode/reg plus the extra word expression (None if absent)."""
    def __init__ (self, mode, reg, extra = None):
        self.mode, self.reg, self.extra = mode, reg, extra
    def bits (self):
        return (self.mode << 3) | self.reg
    def nwords (self):
        return 0 if self.extra is None else 1

def parse_operand (text):
    t = text.strip ()
    if not t:
        raise AsmError ("пустой операнд")
    r = REGS.get (t.upper ())
    if r is not None:
        return Operand (0, r)
    if t.startswith ('@#'):
        return Operand (3, 7, t[2:])
    if t.startswith ('#'):
        return Operand (2, 7, t[1:])
    if t.startswith ('@'):
        inner = t[1:]
        m = re.match (r'\((R[0-7]|SP|PC)\)\+$', inner, re.I)
        if m:                               # @(Rn)+ - mode 3
            return Operand (3, REGS[m.group (1).upper ()])
        m = re.match (r'(.*)\((R[0-7]|SP|PC|[0-7])\)$', inner, re.I)
        if m:
            reg = REGS.get (m.group (2).upper ())
            if reg is None:
                reg = int (m.group (2), 8)
            return Operand (7, reg, m.group (1) or '0')
        return Operand (7, 7, '.' + inner)  # relative deferred
    m = re.match (r'-\((R[0-7]|SP|PC)\)$', t, re.I)
    if m:
        return Operand (4, REGS[m.group (1).upper ()])
    m = re.match (r'\((R[0-7]|SP|PC|[0-7])\)\+$', t, re.I)
    if m:                                   # (Rn)+ - mode 2
        return Operand (2, REGS[m.group (1).upper ()])
    m = re.match (r'\((R[0-7]|SP|PC|[0-7])\)$', t, re.I)
    if m:                                   # (Rn) - mode 1
        return Operand (1, REGS[m.group (1).upper ()])
    m = re.match (r'(.*)\((R[0-7]|SP|PC|[0-7])\)$', t, re.I)
    if m:
        reg = REGS.get (m.group (2).upper ())
        if reg is None:
            reg = int (m.group (2), 8)
        return Operand (6, reg, m.group (1) or '0')
    return Operand (6, 7, '.' + t)          # plain label: PC-relative

def resolve_extra (op, pc, symbols):
    """The value of the extra word; pc = the address OF that word."""
    expr = op.extra
    if expr.startswith ('.'):               # the PC-relative marker
        # PC-relative displacements are in BYTES (unlike branch offsets,
        # which are in words): disp = target - PC_after_the_word
        target = eval_expr (expr[1:], symbols, pc + 2)
        return (target - (pc + 2)) & 0o177777
    return eval_expr (expr, symbols, pc)

# ---------------------------------------------------------- instruction

OPS2 = {
    'MOV': 0o01, 'MOVB': 0o11, 'CMP': 0o02, 'CMPB': 0o12,
    'BIT': 0o03, 'BITB': 0o13, 'BIC': 0o04, 'BICB': 0o14,
    'BIS': 0o05, 'BISB': 0o15, 'ADD': 0o06, 'SUB': 0o16,
    }
OPS1 = {
    'CLR': 0o050, 'COM': 0o051, 'INC': 0o052, 'DEC': 0o053,
    'NEG': 0o054, 'ADC': 0o055, 'SBC': 0o056, 'TST': 0o057,
    'ROR': 0o060, 'ROL': 0o061, 'ASR': 0o062, 'ASL': 0o063,
    'CLRB': 0o1050, 'COMB': 0o1051, 'INCB': 0o1052, 'DECB': 0o1053,
    'NEGB': 0o1054, 'ADCB': 0o1055, 'SBCB': 0o1056, 'TSTB': 0o1057,
    'RORB': 0o1060, 'ROLB': 0o1061, 'ASRB': 0o1062, 'ASLB': 0o1063,
    }
OPSEIS = {'MUL': 0o70, 'DIV': 0o71, 'ASH': 0o72, 'ASHC': 0o73}
BRANCH = {
    'BR': 0o00400, 'BNE': 0o01000, 'BEQ': 0o01400, 'BGE': 0o02000,
    'BLT': 0o02400, 'BGT': 0o03000, 'BLE': 0o03400, 'BPL': 0o100000,
    'BMI': 0o100400, 'BVC': 0o102000, 'BVS': 0o102400, 'BCC': 0o103000,
    'BCS': 0o103400, 'BHI': 0o101000, 'BLOS': 0o101400,
    'BLO': 0o103400, 'BHIS': 0o103000,
    }
SIMPLE = {'RTI': 0o000002, 'WAIT': 0o000001, 'NOP': 0o000240,
          'RESET': 0o000005, 'HALT': 0o000000}

def parse_reg (text, what):
    reg = REGS.get (text.strip ().upper ())
    if reg is None:
        raise AsmError ("%s: не регистр %r" % (what, text))
    return reg

def encode (mnem, rest, pc, symbols, resolve):
    """-> (word, [Operand...]).  resolve=False (pass 1) skips the branch
    target evaluation: the size does not depend on it."""
    mnem = mnem.upper ()
    ops = split_operands (rest)
    if mnem in SIMPLE:
        if ops:
            raise AsmError ("%s без операндов" % mnem)
        return SIMPLE[mnem], []
    if mnem == 'RTS':
        if len (ops) != 1:
            raise AsmError ("RTS reg")
        return 0o000200 | parse_reg (ops[0], 'RTS'), []
    if mnem in BRANCH:
        if len (ops) != 1:
            raise AsmError ("%s метка" % mnem)
        if resolve:
            target = eval_expr (ops[0], symbols, pc + 2)
            disp = (target - (pc + 2)) >> 1     # word units, PC after = pc+2
            if disp < -128 or disp > 127:
                raise AsmError ("%s: ветвление дальше 128 слов" % mnem)
        else:
            disp = 0
        return BRANCH[mnem] | (disp & 0o377), []
    if mnem == 'SOB':
        if len (ops) != 2:
            raise AsmError ("SOB reg,метка")
        reg = parse_reg (ops[0], 'SOB')
        if resolve:
            target = eval_expr (ops[1], symbols, pc + 2)
            disp = ((pc + 2) - target) >> 1     # word units, PC after = pc+2
            if disp < 0 or disp > 63:
                raise AsmError ("SOB: слишком далеко")
        else:
            disp = 0
        return 0o77000 | (reg << 6) | (disp & 0o77), []
    if mnem in OPS2:
        if len (ops) != 2:
            raise AsmError ("%s требует два операнда" % mnem)
        src = parse_operand (ops[0])
        dst = parse_operand (ops[1])
        return (OPS2[mnem] << 12) | (src.bits () << 6) | dst.bits (), \
               [op for op in (src, dst) if op.nwords ()]
    if mnem == 'SWAB':
        if len (ops) != 1:
            raise AsmError ("SWAB dst")
        dst = parse_operand (ops[0])
        return (0o0003 << 6) | dst.bits (), [dst] if dst.nwords () else []
    if mnem == 'JMP':
        if len (ops) != 1:
            raise AsmError ("JMP dst")
        dst = parse_operand (ops[0])
        return 0o00100 | dst.bits (), [dst] if dst.nwords () else []
    if mnem == 'JSR':
        if len (ops) != 2:
            raise AsmError ("JSR reg,dst")
        reg = parse_reg (ops[0], 'JSR')
        dst = parse_operand (ops[1])
        return 0o04000 | (reg << 6) | dst.bits (), \
               [dst] if dst.nwords () else []
    if mnem in OPS1:
        if len (ops) != 1:
            raise AsmError ("%s требует один операнд" % mnem)
        dst = parse_operand (ops[0])
        return (OPS1[mnem] << 6) | dst.bits (), [dst] if dst.nwords () else []
    if mnem in OPSEIS:
        if len (ops) != 2:
            raise AsmError ("%s src,reg" % mnem)
        reg = parse_reg (ops[1], mnem)
        src = parse_operand (ops[0])
        # EIS: 07 O R SSDD: [11:9]=op (MUL=0..ASHC=3), [8:6]=the register,
        # [5:0]=the source operand in the standard mode/reg encoding.
        word = (0o70000 | ((OPSEIS[mnem] - 0o70) << 9) |
                (reg << 6) | src.bits ())
        return word, [src] if src.nwords () else []
    raise AsmError ("неизвестная команда %r" % mnem)

def split_operands (rest):
    parts, depth, cur = [], 0, ''
    for ch in rest:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append (cur)
            cur = ''
        else:
            cur += ch
    if cur.strip () or parts:
        parts.append (cur)
    return parts

# ------------------------------------------------------------- assembly

def assemble (src_path):
    with open (src_path, encoding = 'utf-8') as f:
        lines = f.readlines ()

    # ---- pass 1: addresses and symbols
    symbols = {}
    prog = []                               # (lineno, kind) kinds below
    pc = 0
    errors = []
    for lineno, raw in enumerate (lines, 1):
        line = raw.split (';')[0].rstrip ()
        text = line.strip ()
        if not text:
            continue
        label = None
        m = re.match (r'^([A-Za-z_][A-Za-z0-9_]*):(?:\s*(.*))?$', text)
        if m:
            label, text = m.group (1), (m.group (2) or '').strip ()
        if label:
            if label in symbols:
                errors.append ((lineno, "дублируется метка %s" % label))
            symbols[label] = pc
        if not text:
            continue
        try:
            up = text.upper ()
            if up.startswith ('.ASECT') or up.startswith ('.PSECT'):
                continue
            if up.startswith ('.END'):
                break
            if up.startswith ('.EVEN'):
                if pc & 1:
                    pc += 1
                prog.append ((lineno, ('EVEN',)))
                continue
            m = re.match (r'^\.\s*=\s*(.+)$', text)
            if m:
                prog.append ((lineno, ('SETPC',
                                       eval_expr (m.group (1), symbols, pc))))
                pc = prog[-1][1][1]
                continue
            m = re.match (r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$', text)
            if m and m.group (1).upper () not in REGS:
                symbols[m.group (1)] = eval_expr (m.group (2), symbols, pc)
                continue
            if up.startswith ('.BLKW'):
                cnt = eval_expr (text[5:].strip (), symbols, pc)
                pc += 2 * cnt
                continue
            if up.startswith ('.WORD') or up.startswith ('.BYTE'):
                n = len (split_operands (text[5:]))
                pc += 2 if up.startswith ('.WORD') else n
                prog.append ((lineno, ('DATA', text)))
                continue
            if up.startswith ('.ASCII'):
                m = re.match (r'\.ASCII\s*(.)(.*?)\1\s*$', text, re.I)
                if not m:
                    raise AsmError (".ASCII /текст/")
                prog.append ((lineno, ('ASCII', m.group (2))))
                pc += len (m.group (2))
                continue
            m = re.match (r'^([A-Za-z][A-Za-z0-9]*)(\s+(.*))?$', text)
            if not m:
                raise AsmError ("не разберу строку: %r" % text)
            mnem = m.group (1)
            rest = (m.group (3) or '').strip ()
            word, extras = encode (mnem, rest, pc, symbols, False)
            prog.append ((lineno, ('INS', mnem, rest)))
            pc += 2 + 2 * len (extras)
        except AsmError as e:
            errors.append ((lineno, str (e)))

    # ---- pass 2: emit
    memory = {}                             # byte address -> byte value
    words_out = []                          # (addr, word, lineno)
    pc = 0
    for lineno, kind in prog:
        try:
            if kind[0] == 'SETPC':
                pc = kind[1]
                continue
            if kind[0] == 'EVEN':
                if pc & 1:
                    pc += 1
                continue
            if kind[0] == 'DATA':
                text = kind[1]
                if text.upper ().startswith ('.WORD'):
                    for part in split_operands (text[5:]):
                        v = eval_expr (part, symbols, pc)
                        memory[pc] = v & 0o377
                        memory[pc + 1] = (v >> 8) & 0o377
                        words_out.append ((pc, v & 0o177777, lineno))
                        pc += 2
                else:
                    for part in split_operands (text[5:]):
                        memory[pc] = eval_expr (part, symbols, pc) & 0o377
                        pc += 1
            elif kind[0] == 'ASCII':
                for ch in kind[1]:
                    memory[pc] = ord (ch) & 0o377
                    pc += 1
            else:
                mnem, rest = kind[1], kind[2]
                word, extras = encode (mnem, rest, pc, symbols, True)
                memory[pc] = word & 0o377
                memory[pc + 1] = (word >> 8) & 0o377
                words_out.append ((pc, word & 0o177777, lineno))
                pc += 2
                for op in extras:
                    v = resolve_extra (op, pc, symbols)
                    memory[pc] = v & 0o377
                    memory[pc + 1] = (v >> 8) & 0o377
                    words_out.append ((pc, v & 0o177777, lineno))
                    pc += 2
        except AsmError as e:
            errors.append ((lineno, str (e)))

    start = symbols.get ('START', 0o200)
    return memory, words_out, symbols, start, errors, lines

# ------------------------------------------------------------- output

def write_binary (memory, start, path):
    """Absolute loader format: [1, X, cnt_lo, cnt_hi, org_lo, org_hi]
    + data + checksum; the end block (cnt = 6) sets PC."""
    addrs = sorted (memory)
    if not addrs:
        raise AsmError ("пустой образ")
    runs, cur = [], [addrs[0]]
    for a in addrs[1:]:
        if a == cur[-1] + 1:
            cur.append (a)
        else:
            runs.append (cur)
            cur = [a]
    runs.append (cur)
    out = bytearray ()
    for run in runs:
        i = 0
        while i < len (run):
            chunk = run[i:i + 32]
            org = chunk[0]
            data = bytes (memory[a] for a in chunk)
            cnt = 6 + len (data)
            hdr = bytes ([1, 0, cnt & 0o377, (cnt >> 8) & 0o377,
                          org & 0o377, (org >> 8) & 0o377])
            csum = sum (hdr) + sum (data)
            out += hdr + data + bytes ([(-csum) & 0o377])
            i += len (chunk)
    out += bytes ([1, 0, 6, 0, start & 0o377, (start >> 8) & 0o377])
    with open (path, 'wb') as f:
        f.write (bytes (out))
    return len (out)

def write_listing (words_out, symbols, errors, lines, src_path, path):
    sym_lines = ["%-8s = %06o" % (n, symbols[n]) for n in sorted (symbols)]
    out = ["dks_fw.lst - листинг сборки %s" % os.path.basename (src_path),
           "(листинговая сборка: PDP11/tests/gen_dks_fw.py)", "", "Символы:"]
    for i in range (0, len (sym_lines), 4):
        out.append ("  " + "  ".join (sym_lines[i:i + 4]))
    out += ["", "№стр  адрес   слова            исходный текст"]
    wi = 0
    for lineno, raw in enumerate (lines, 1):
        text = raw.rstrip ('\n')
        w = []
        while wi < len (words_out) and words_out[wi][2] == lineno:
            addr, word, _ = words_out[wi]
            w.append ("%06o:%06o" % (addr, word))
            wi += 1
        if text.strip ():
            if w:
                out.append ("%-5d %s   %s" % (lineno, " ".join (w), text))
            else:
                out.append ("%-5d %24s%s" % (lineno, "", text))
    out.append ("")
    if errors:
        out.append ("ОШИБКИ:")
        out += ["  строка %d: %s" % (l, m) for l, m in errors]
    else:
        out.append ("Ошибок нет.")
    with open (path, 'w', encoding = 'utf-8') as f:
        f.write ("\n".join (out) + "\n")

def main ():
    src = sys.argv[1] if len (sys.argv) > 1 else SRC
    binp = sys.argv[2] if len (sys.argv) > 2 else BIN
    lstp = sys.argv[3] if len (sys.argv) > 3 else LST
    memory, words_out, symbols, start, errors, lines = assemble (src)
    write_listing (words_out, symbols, errors, lines, src, lstp)
    if errors:
        for lineno, msg in errors:
            print ("строка %d: %s" % (lineno, msg), file = sys.stderr)
        sys.exit (1)
    size = write_binary (memory, start, binp)
    print ("dks_fw.bin: %d bytes, START = %o, %d words" %
           (size, start, len (words_out)))

if __name__ == '__main__':
    main ()

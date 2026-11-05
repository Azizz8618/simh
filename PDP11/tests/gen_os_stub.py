#!/usr/bin/env python3
"""gen_os_stub.py - generate the БЭСМ-6 ОС-заглушка for the stage B2 test.

Emits PDP11/tests/dks_b2_besm6.ini: a small machine program that plays the
СВЯЗЬ7 operating system on the БЭСМ-6 side of the КАДОПАМ bridge:

  * waits for the КРК channel status (cell 0),
  * selects channel 2,
  * drains АБУФС: system requests (abonent 48) and line events
    (abonents 1..24),
  * answers a connect (code 1) with АБФОСH = (2, term, 0) — the marker
    АБФОСH+0 is written LAST (the Э-60 treats it as the completion),
  * for "input ready" (cmd 4): acknowledges STATUS0, copies the input
    array to the output area (echo), posts the line into АБУФH and rings
    the doorbell,
  * for "output done" (cmd 5): acknowledges STATUS1.

The Э-60 side of the protocol is PDP11/pdp11_dks.c (stage A engine).

BESM-6 notes:
  * instruction 032 (э32)  reads  the Э-60 cell  addr+M[reg];
    instruction 132 (э32 with the write bit, no mnemonic in simh —
    emitted in the numeric form "R 132 A") writes the low 16 bits of
    the accumulator there;
  * the э32 result lands in bits 25-40 of the accumulator, hence every
    read is followed by a right shift by 24 (cell D_SH24);
  * мод (0230) loads the modifier of the NEXT instruction from a cell:
    the one-shot way to reach a computed Э-60 address (M[MOD] is a
    separate register 020, М17 stays free for the doorbell base);
  * RAU modes: after xta/аах/аех the mode is logical, so "по" means
    "jump when ACC == 0"; after arx (сдц) the mode is multiplicative,
    so "пе" means "jump when ACC >= 0";
  * besm6_shift: a POSITIVE сд count shifts RIGHT, negative - left
    (the count cell holds (64+n)<<41 for a right shift by n).
"""

import sys
import os

# ---------------- data cells (octal addresses) ----------------

def w48(v):
    """48-bit word as 16 octal digits."""
    assert 0 <= v < (1 << 48), hex(v)
    return format(v, '016o')

def shift_cell(n):
    """сд count cell: the count = (word >> 41) - 64, so word = (64+n)<<41."""
    assert -64 <= n <= 63
    return ((64 + n) & 0o177) << 41

DATA = [
    # addr  value                       comment
    (0o310, 'w', 0o2000 << 24,   'DKS_STATUS<<24: маска статуса канала'),
    (0o311, 'w', 0o400,          'масабн: звонок в Э-60'),
    (0o312, 'w', 0,              'результат реплая (успех)'),
    (0o313, 'w', shift_cell(24), 'сд: >>24 (э32-результат к младшим разрядам)'),
    (0o314, 'w', 0o1400,         'шаг области терминала'),
    (0o315, 'w', 0o1000,         'база областей'),
    (0o316, 'w', (1 << 48) - 2,  '-1 в дополнении до единицы (БЭСМ-6)'),
    (0o317, 'w', 0o142,          'база слотов АБУФС'),
    (0o320, 'w', 0o150,          'база слотов АБУФH'),
    (0o321, 'w', 0o60,           'абонент 48 (десятичное) = системные запросы'),
    (0o322, 'w', 1,              'код запроса "подключить"'),
    (0o323, 'w', 4,              'команда "ввод готов"'),
    (0o324, 'w', 5,              'команда "вывод выполнен"'),
    (0o325, 'w', 0o20000,        'АР_ФЛАГ'),
    (0o326, 'w', (1 << 48) - 1 - 0o20000, '~АР_ФЛАГ'),
    (0o327, 'w', 3,              'маска кольца (4 слота)'),
    (0o330, 'w', 8,              'смещение массива в области (слов)'),
    (0o331, 'w', 1,              'единица'),
    (0o332, 'w', 2,              'маркер реплая в АБФОСH+0'),
    (0o333, 'w', 2,              'номер канала КРК'),
    (0o334, 'w', shift_cell(8), 'сд: >>8 (старший байт слова)'),
    (0o335, 'w', shift_cell(1), 'сд: >>1 (байтный адрес -> словный)'),
    (0o336, 'w', shift_cell(-1), 'сд: <<1 (словный адрес -> байтный)'),
    (0o337, 'w', 0o377,          'маска младшего байта'),
    (0o360, 'w', 0o146,          'конец слотов АБУФС (для заворота)'),
    (0o362, 'w', shift_cell(-8), 'сд: <<8'),
    (0o363, 'w', shift_cell(-9), 'сд: <<9'),
]

VARS = [
    (0o340, 'V_SRC'),   (0o341, 'V_RECV'), (0o342, 'V_CNT'),
    (0o343, 'V_ABON'),  (0o344, 'V_CODE'), (0o345, 'V_LINE'),
    (0o346, 'V_N'),     (0o347, 'V_AREA'), (0o350, 'V_CMD'),
    (0o351, 'V_IP'),    (0o352, 'V_OP'),   (0o353, 'V_OA'),
    (0o354, 'V_LEN'),   (0o355, 'V_W'),    (0o356, 'V_SLOT2'),
    (0o357, 'V_HP'),
    (0o361, 'V_SP'),
    (0o364, 'V_T1'),
]

NAMES = {addr: name for addr, name in VARS}
for addr, _kind, _v, cmt in DATA:
    NAMES[addr] = cmt.split(':')[0].strip() if False else None
# mnemonic names for the data cells used by the program
DN = {
    0o310: 'D_STMASK', 0o311: 'D_BELL',  0o312: 'D_ZERO',
    0o313: 'D_SH24',   0o314: 'D_STRIDE', 0o315: 'D_ABASE',
    0o316: 'D_M1',     0o317: 'D_SBASE', 0o320: 'D_HBASE',
    0o321: 'D_ABOS',   0o322: 'D_CONN',  0o323: 'D_CIN',
    0o324: 'D_COUT',   0o325: 'D_FLAG',  0o326: 'D_CFLAG',
    0o327: 'D_RMASK',  0o330: 'D_EIGHT', 0o331: 'D_ONE',
    0o332: 'D_MARK',   0o333: 'D_CHAN',  0o334: 'D_SH8',
    0o335: 'D_SH1',    0o336: 'D_SHL1',  0o337: 'D_M377',
    0o360: 'D_SEND',
    0o362: 'D_SHL8',   0o363: 'D_SHL9',
}
NAMES.update(DN)

# ---------------- program ----------------
# Each entry: (label or None, left, right); empty string = fill later.
# Two-pass: assign word addresses, resolve labels (word addresses).

PROGRAM = [
    ('L_START', 'э32 0',            'aax D_STMASK'),
    (None,      'по L_START',       'пб L_SEL'),        # ждём статус канала
    ('L_SEL',   'xta D_CHAN',       '0 132 0'),         # канал ← 2
    (None,      'vtm 77777(17)',    ''),                # М17 = 077777 (дверь)

    ('L_MAIN',  'э32 140',          'сд D_SH24'),       # АБУФС.source
    (None,      'atx V_SRC',        'э32 141'),
    (None,      'сд D_SH24',        'atx V_RECV'),
    (None,      'xta V_RECV',       'aex D_RMASK'),     # 3-recv (вч плавучий!)
    (None,      'arx D_ONE',        'arx V_SRC'),       # src+4-recv
    (None,      'aax D_RMASK',      'atx V_CNT'),       # (src-recv) mod 4
    (None,      'по L_MAIN',        'пб L_PREP'),       # пусто -> главный цикл

    ('L_PREP',  'xta V_RECV',       'arx D_SBASE'),     # V_SP = 0142+recv
    (None,      'atx V_SP',         'xta V_RECV'),      # recv += count
    (None,      'arx V_CNT',        'aax D_RMASK'),
    (None,      'atx V_RECV',       '0 132 141'),       # освободить слоты
    (None,      'xta D_BELL',       '17 132 0'),        # звонок: слоты взяты

    ('L_SLOT',  'xta V_CNT',        ''),                # ещё есть?
    (None,      'по L_MAIN',        'пб L_DEC'),
    ('L_DEC',   'xta V_CNT',        'arx D_RMASK'),     # count--: +3 mod 4
    (None,      'aax D_RMASK',      'atx V_CNT'),       # сохранить!
    (None,      'мод V_SP',         'э32 0'),           # абонент из слота
    (None,      'сд D_SH24',        'atx V_ABON'),
    (None,      'xta V_SP',         'arx D_ONE'),       # V_SP++ с заворотом
    (None,      'atx V_SP',         'aex D_SEND'),
    (None,      'по L_RSTSP',       'пб L_DISP'),
    ('L_RSTSP', 'xta D_SBASE',      'atx V_SP'),
    (None,      'пб L_DISP',        'пб L_DISP'),

    ('L_DISP',  'xta V_ABON',       'aex D_ABOS'),      # абонент 48?
    (None,      'по L_SYSREQ',      'пб L_LINE'),

    # --- системный запрос ---
    ('L_SYSREQ', 'э32 154',         'сд D_SH24'),       # код
    (None,      'atx V_CODE',       'xta V_CODE'),
    (None,      'aex D_CONN',       ''),
    (None,      'по L_SYSCONN',     'пб L_SLOT'),       # не 1 (отключение) -> дальше
    ('L_SYSCONN', 'э32 155',        'сд D_SH24'),       # номер линии
    (None,      'atx V_LINE',       'xta V_LINE'),
    (None,      '0 132 160',        ''),                # АБФОСH+1 <- терм (= линия)
    (None,      'xta D_ZERO',       '0 132 161'),       # АБФОСH+2 <- 0
    (None,      'xta D_MARK',       '0 132 157'),       # АБФОСH+0 <- 2  ПОСЛЕДНЕЙ
    (None,      'xta D_BELL',       '17 132 0'),        # звонок: у ОС ответ
    (None,      'пб L_SLOT',        'пб L_SLOT'),

    # --- событие линии ---
    ('L_LINE',  'xta V_ABON',       'atx V_LINE'),
    (None,      'xta V_ABON',       'сд D_SHL9'),       # AREA = 01000 + n*01400
    (None,      'atx V_T1',         'xta V_ABON'),      # = (n<<9)+(n<<8)
    (None,      'сд D_SHL8',        'arx V_T1'),
    (None,      'arx D_ABASE',      'atx V_AREA'),

    ('L_AREDONE', 'xta V_AREA',     'мод V_AREA'),
    (None,      'э32 2',            'сд D_SH24'),       # команда области
    (None,      'atx V_CMD',        'xta V_CMD'),
    (None,      'aex D_CIN',        ''),
    (None,      'по L_INPUT',       'пб L_CHK5'),

    ('L_CHK5',  'xta V_CMD',        'aex D_COUT'),
    (None,      'по L_OUTDONE',     'пб L_SLOT'),

    # вывод выполнен: снять АР_ФЛАГ статуса 1
    ('L_OUTDONE', 'мод V_AREA',     'э32 1'),
    (None,      'сд D_SH24',        'aax D_CFLAG'),
    (None,      'мод V_AREA',       '0 132 1'),
    (None,      'xta D_BELL',       '17 132 0'),        # звонок: флаг снят
    (None,      'пб L_SLOT',        'пб L_SLOT'),

    # ввод готов: ACK статуса 0, эхо массива, АБУФH, звонок
    ('L_INPUT', 'мод V_AREA',       'э32 0'),
    (None,      'сд D_SH24',        'aax D_CFLAG'),
    (None,      'мод V_AREA',       '0 132 0'),         # STATUS0 &= ~flag
    (None,      'мод V_AREA',       'э32 3'),           # байтный адрес буфера
    (None,      'сд D_SH24',        'сд D_SH1'),
    (None,      'atx V_IP',         ''),                # словный адрес
    (None,      'xta V_AREA',       'arx D_EIGHT'),
    (None,      'сд D_SHL1',        'atx V_OA'),        # (AREA+8)<<1
    (None,      'xta V_AREA',       'arx D_EIGHT'),
    (None,      'atx V_OP',         'xta D_ZERO'),
    (None,      'atx V_LEN',        ''),

    ('L_COPY',  'мод V_IP',         'э32 0'),           # слово массива
    (None,      'сд D_SH24',        'atx V_W'),
    (None,      'мод V_OP',         '0 132 0'),         # -> область вывода
    (None,      'xta V_IP',         'arx D_ONE'),
    (None,      'atx V_IP',         'xta V_OP'),
    (None,      'arx D_ONE',        'atx V_OP'),
    (None,      'xta V_LEN',        'arx D_ONE'),
    (None,      'atx V_LEN',        'xta V_W'),
    (None,      'aax D_M377',       ''),                # младший байт == 0?
    (None,      'по L_CEND',        'пб L_CHI'),
    ('L_CHI',   'xta V_W',          'сд D_SH8'),        # старший байт == 0?
    (None,      'aax D_M377',       ''),
    (None,      'по L_CEND',        'пб L_COPY'),

    ('L_CEND',  'xta V_OA',         'мод V_AREA'),      # [AREA+6] <- (AREA+8)<<1
    (None,      '0 132 6',          'xta V_LEN'),
    (None,      'мод V_AREA',       '0 132 7'),         # [AREA+7] <- длина
    (None,      'xta D_CIN',        'мод V_AREA'),
    (None,      '0 132 5',          ''),                # [AREA+5] <- 4
    (None,      'мод V_AREA',       'э32 1'),
    (None,      'сд D_SH24',        'aox D_FLAG'),
    (None,      'мод V_AREA',       '0 132 1'),         # STATUS1 |= flag
    (None,      'э32 146',          'сд D_SH24'),       # слот АБУФH
    (None,      'atx V_SLOT2',      'xta V_SLOT2'),
    (None,      'arx D_HBASE',      'atx V_HP'),
    (None,      'xta V_LINE',       'мод V_HP'),
    (None,      '0 132 0',          ''),                # АБУФH[слот] <- линия
    (None,      'xta V_SLOT2',      'arx D_ONE'),
    (None,      'aax D_RMASK',      '0 132 146'),       # АБУФH.source++
    (None,      'xta D_BELL',       '17 132 0'),        # звонок в Э-60
    (None,      'пб L_SLOT',        'пб L_SLOT'),
]

def assemble():
    # pass 1: addresses
    labels = {}
    addr = 0o100
    words = []
    for label, left, right in PROGRAM:
        if label:
            assert label not in labels, label
            labels[label] = addr
        words.append((addr, left, right))
        addr += 1
    # full name table: program labels + data/var names, longest first so
    # that prefix names never shadow longer ones
    names = dict(labels)
    names.update({name: addr for addr, name in VARS})
    names.update({name: addr for addr, name in DN.items()})
    order = sorted(names, key=len, reverse=True)
    # pass 2: resolve
    out = []
    for a, left, right in words:
        left = resolve(left, names, order)
        right = resolve(right, names, order)
        out.append((a, left, right))
    return out

def resolve(ins, names, order):
    ins = ins.strip()
    if not ins:
        return None
    for name in order:
        if name in ins:
            ins = ins.replace(name, '%o' % names[name])
    return ins

def gen_ini():
    lines = []
    lines.append('; dks_b2_besm6.ini - the БЭСМ-6 side of the stage B2 bridge test.')
    lines.append('; An ОС-заглушка: polls АБУФС, answers system requests, echoes input')
    lines.append('; arrays back.  Every КАДОПАМ access crosses the TCP bridge to the')
    lines.append('; Э-60 simulator (SET DKS BRIDGE=4140 there).')
    lines.append('; Generated by gen_os_stub.py - do not edit by hand.')
    lines.append('set dks enable')
    lines.append('set dks remote=localhost:4140')
    lines.append('set dks debug=bridge;kadopam;ring;irq')
    lines.append('set debug stdout')
    lines.append('')
    lines.append('; constants')
    for addr, _kind, val, _cmt in DATA:
        lines.append('d %o %s' % (addr, w48(val)))
    lines.append('')
    for a, left, right in assemble():
        if right is None:
            lines.append('d -ml %o %s' % (a, left))
        else:
            lines.append('d -ml %o %s, %s' % (a, left, right))
    lines.append('')
    lines.append('run 100')
    lines.append('')
    return '\n'.join(lines)

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, 'dks_b2_besm6.ini')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(gen_ini())
    print('written', path)

if __name__ == '__main__':
    main()

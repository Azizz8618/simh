/*==============================================================
 * ГЛИССАДА-6 / ПРОЕКТ СМ-2. РАСШИРЕННЫЕ КОМАНДЫ СМ-2М.
 *
 * Двухсловные (17) и дополнительные команды по статье А. Савватеева.
 * ВАЖНО: в источнике приведены НОМЕРА кодов операций, но не полные
 * битовые раскладки. Кодировка объявлена в sm2_params.h и является
 * рабочей (?SM2_DECLARE_ENCODING). Для бинарной совместимости с
 * подлинными программами СМ-2 нужна точная таблица кодов — GAP.
 *==============================================================*/
#include <math.h>

#include "hp2100_defs.h"
#include "hp2100_cpu.h"
#include "sm2_params.h"

extern uint32 sm2_rb1, sm2_rb2, sm2_ri1, sm2_ri2;
extern uint32 ABREG[2];
extern HP_WORD PR;
extern t_bool is_sm2;

extern t_stat cpu_resolve_indirects (t_bool interruptible);
extern HP_WORD mem_read (DEVICE *dptr, ACCESS_CLASS classification, HP_WORD address);
extern void mem_write (DEVICE *dptr, ACCESS_CLASS classification, HP_WORD address, HP_WORD value);

#define RA          ABREG[0]
#define SM2_ADDR_MASK (is_sm2 ? SM2_LA_MASK : LA_MASK)
#define RB          ABREG[1]
#define SM2_OP(v)   ((v) & SM2_OPCODE_MASK)

static DEVICE *cpu_dptr (void)
{
extern DEVICE cpu_dev;
return &cpu_dev;
}

/* Преобразование числа СМ-2М (знак бит 0, мантисса 1..23, порядок 24..30,
   знак порядка бит 31) в double и обратно. */
static double sm2_to_double (uint32 hi, uint32 lo)
{
uint32 mant = ((hi & 017777777u) << 8) | ((lo >> 8) & 0xffu);
int    e    = (int)((lo >> 1) & 0x7fu);
if (lo & 1u) e = -e;
double v = (double) mant / (double) (1u << 24);
if (hi & (1u << 31)) v = -v;
return v * pow (2.0, e);
}

static void double_to_sm2 (double v, uint32 *hi, uint32 *lo)
{
int sign = v < 0;
int e = 0;
double m = fabs (v);
if (m != 0.0) m = frexp (m, &e);
uint32 mant = (uint32) (m * (double) (1u << 24)) & 0xffffffu;
*hi = ((uint32) sign << 31) | mant;
*lo = ((uint32) (e & 0x7fu) << 1) | ((e < 0) ? 1u : 0u);
}

/* Выполнить расширенную команду СМ-2М.
   Возвращает TRUE, если команда была распознана. */
t_bool sm2_ext_instruction (t_stat *status)
{
uint32 op = SM2_OP (IR);
uint32 addr, data, hi, lo;
double a, b;

if (!is_sm2)
    return FALSE;

/* Двухсловные команды берут адрес/операнд из слова после инструкции. */
addr = mem_read (cpu_dptr (), Data, PR & SM2_ADDR_MASK);

switch (op) {

    case SM2_OP_DLD:                    /* двойная загрузка */
        RA = mem_read (cpu_dptr (), Data, addr & SM2_ADDR_MASK);
        RB = mem_read (cpu_dptr (), Data, (addr + 1) & SM2_ADDR_MASK);
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_DST:                    /* двойная запись */
        mem_write (cpu_dptr (), Data, addr & SM2_ADDR_MASK, RA);
        mem_write (cpu_dptr (), Data, (addr + 1) & SM2_ADDR_MASK, RB);
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_MPY:                    /* умножение целых */
        {
        t_uint64 prod = (t_uint64) (int32) RA * (t_uint64) (int32) addr;
        RB = (uint32) (prod >> 16);
        RA = (uint32) prod;
        }
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_DIV:                    /* деление целых */
        if (addr != 0) {
            int32 q = (int32) (((t_uint64) RB << 32 | RA) / (int32) addr);
            int32 r = (int32) (((t_uint64) RB << 32 | RA) % (int32) addr);
            RA = (uint32) q;
            RB = (uint32) r;
            }
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_LDX:                    /* загрузка РИ1 */
        sm2_ri1 = addr & 0177776u;
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_LDY:                    /* загрузка РИ2 */
        sm2_ri2 = addr & 0177776u;
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_STX:                    /* запись РИ1 */
        mem_write (cpu_dptr (), Data, addr & SM2_ADDR_MASK, sm2_ri1);
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_STY:                    /* запись РИ2 */
        mem_write (cpu_dptr (), Data, addr & SM2_ADDR_MASK, sm2_ri2);
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_LBN:                    /* загрузка РБ1 */
        sm2_rb1 = (addr >> 8) & 0xffu;
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_LBP:                    /* подготовка РБ2 */
        sm2_rb2 = (addr >> 8) & 3u;
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_FIX:                    /* float -> int */
        {
        data = mem_read (cpu_dptr (), Data, addr & SM2_ADDR_MASK);
        a = sm2_to_double (RA, data);
        RA = (uint32) (int32) a;
        }
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_FLT:                    /* int -> float */
        {
        a = (double) (int32) RA;
        double_to_sm2 (a, &hi, &lo);
        RA = hi;
        RB = lo;
        }
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_FAD:
    case SM2_OP_FSB:
    case SM2_OP_FMP:
    case SM2_OP_FDV:
        data = mem_read (cpu_dptr (), Data, addr & SM2_ADDR_MASK);
        a = sm2_to_double (RA, RB);
        b = sm2_to_double (data, mem_read (cpu_dptr (), Data, (addr + 1) & SM2_ADDR_MASK));
        if (op == SM2_OP_FAD) a += b;
        else if (op == SM2_OP_FSB) a -= b;
        else if (op == SM2_OP_FMP) a *= b;
        else if (b != 0.0) a /= b;
        double_to_sm2 (a, &hi, &lo);
        RA = hi;
        RB = lo;
        PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_TAS:                    /* проверка и установка разряда */
        data = mem_read (cpu_dptr (), Data, addr & SM2_ADDR_MASK);
        if ((data & (1u << 15)) == 0) {
            mem_write (cpu_dptr (), Data, addr & SM2_ADDR_MASK, data | (1u << 15));
            PR = (PR + 1 + SM2_SKIP_STEP) & SM2_ADDR_MASK;
            }
        else
            PR = (PR + 1) & SM2_ADDR_MASK;
        break;

    case SM2_OP_ASR: case SM2_OP_ASL:   /* двойные сдвиги */
    case SM2_OP_LSR: case SM2_OP_LSL:
    case SM2_OP_RRR: case SM2_OP_RRL:
        {
        uint32 cnt = (IR >> 12) & 017u;
        if (cnt == 0) cnt = 16;
        t_uint64 d = ((t_uint64) RB << 32) | RA;
        if (op == SM2_OP_ASR) d = (t_int64) d >> cnt;
        else if (op == SM2_OP_ASL) d <<= cnt;
        else if (op == SM2_OP_LSR) d >>= cnt;
        else if (op == SM2_OP_LSL) d <<= cnt;
        else if (op == SM2_OP_RRR) d = (d >> cnt) | (d << (64 - cnt));
        else d = (d << cnt) | (d >> (64 - cnt));
        RA = (uint32) d;
        RB = (uint32) (d >> 32);
        PR = (PR + 1) & SM2_ADDR_MASK;
        }
        break;

    default:
        return FALSE;
    }

*status = SCPE_OK;
return TRUE;
}

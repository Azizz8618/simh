/* pdp11_dks.h: ДКС terminal concentrator (Электроника-60) definitions

   Copyright (c) 2026, Leonid Broukhis, Serguei K, the SIMH ДКС project

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
   OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

   Except as contained in this notice, the names of the authors shall not be
   used in advertising or otherwise to promote the sale, use or other
   dealings in this Software without prior written authorization from the
   authors.
*/

/* ДКС — терминальный концентратор на микро-ЭВМ «Электроника-60»
   (PDP-11-совместимой), связывающий до 24 терминалов с БЭСМ-6.

   This header is the single source of the protocol constants.  They are
   copied verbatim from BESM6/besm6_dks.c (the reference OS-side
   implementation, tables svyaz7.be: абуфs, абуфh, абфосs, абфосh, адркс)
   and MUST stay in sync with it.  All addresses are word addresses in the
   32K-word ОЗУ of the Э-60, octal.

   The PDP-11 simulates the Э-60 itself; its 32K-word RAM lives inside the
   device (e60_mem) and is accessed by the firmware through the KADDR/KDATA
   window — just like КАДОПАМ in the real hardware reaches the ОЗУ even
   when the CPU is stopped.
*/

#ifndef PDP11_DKS_H
#define PDP11_DKS_H    1

#include "pdp11_defs.h"

/* Number of terminal lines of one Э-60 (TTY_MAX of the reference). */
#ifndef DKS_LINES
#define DKS_LINES       24
#endif

/* КРК channel of the OS's Э-60 and the КАДОПАМ status bit for cell 0. */
#define DKS_CHAN        2
#define DKS_STATUS      (0400 << DKS_CHAN)      /* 020000: channel present */

/* System cells of the Э-60 ОЗУ (svyaz7.be). */
#define A_BUFS          0140    /* ring Э-60 -> БЭСМ-6: +0 source, +1 receiver, +2..+5 slots */
#define A_BUFH          0146    /* ring БЭСМ-6 -> Э-60: +0 source, +1 receiver, +2..+5 slots */
#define A_FOSS          0154    /* system request: +0 code, +1 line/term, +2 area */
#define A_FOSH          0157    /* system reply: +0 = 2, +1 term no., +2 result */
#define RING_MASK       3       /* ring index mask: 4 slots */
#define NOMNPR          00002   /* meaning unknown, 0 keeps рспец out */
#define DOORBELL        077777  /* адркс: the БЭСМ-6 "wakes up" the Э-60 */
#define DOORBELL_RING   0400    /* масабн: the ringing bit */
#define ABONENT_OS      48      /* abonent 48 = system request/reply */

/* Terminal areas: AREA(n) = 01000 + n * 01400, n = 1..24. */
#define AR_STATUS0      0       /* 020000 = input pending, cleared by the OS */
#define AR_STATUS1      1       /* 020000 = output active, cleared by the OS */
#define AR_CMD_IN       2       /* Э-60 -> ОС: 4 input ready, 5 output done */
#define AR_INBUF        3       /* byte address of the input buffer */
#define AR_CMD_OUT      5       /* ОС -> Э-60: 4 or 5 = take an array */
#define AR_OUTADDR      6       /* byte address of the output array */
#define AR_OUTLEN       7       /* its length in words */
#define AR_FLAG         020000

#define AREA_BASE       01000
#define AREA_SIZE       01400
#define AREA_INBUF      01210   /* input buffer: after +8 and the 01200-word output array */
#define INBUF_BYTES     ((AREA_SIZE - AREA_INBUF) * 2 - 1)      /* 239 */
#define AREA(n)         (AREA_BASE + (n) * AREA_SIZE)

/* System request codes (АБФОСS+0). */
#define SYSREQ_CONNECT    1
#define SYSREQ_DISCONNECT 3
/* Terminal events (area +2 / +5). */
#define EVENT_INPUT_READY  4
#define EVENT_OUTPUT_DONE  5

/* Unibus register offsets (window IOLN_DKS bytes long). */
#define IOLN_DKS        022
#define DKS_CSR         000     /* control/status, offset 00 */
#define DKS_RBUF        002     /* receive buffer: line<<8 | char, DV 040000 */
#define DKS_TSEL        004     /* transmit line select, 1..24 */
#define DKS_TBUF        006     /* transmit buffer (character out) */
#define DKS_KADDR       010     /* ОЗУ Э-60 window address; bit 0100000 = AI */
#define DKS_KDATA       012     /* ОЗУ Э-60 window data */
#define DKS_KCTL        014     /* КАДОПАМ control: chan select / signal БЭСМ-6 */
#define DKS_LSTAT0      016     /* telnet lines 1..12 connected */
#define DKS_LSTAT1      020     /* telnet lines 13..24 connected */

/* DKS_CSR bits. */
#define CSR_V_IE        6
#define CSR_IE          0000100 /* interrupt enable (RW) */
#define CSR_RR          0000200 /* receive ready: FIFO non-empty (RO) */
#define CSR_LC          0000400 /* line change: LSTAT0/1 changed (RO, clr on read) */
#define CSR_ATN         0001000 /* attention: doorbell seen (RO, clr on ack/MCLR) */
#define CSR_MCLR        040000  /* master clear (write) */
#define CSR_RW          CSR_IE
#define CSR_MBZ         (CSR_RR | CSR_LC | CSR_ATN)

/* DKS_RBUF bits. */
#define RBUF_CHAR       0000377
#define RBUF_V_LINE     8       /* Э-60 line number, 1..24 (bits 8..12) */
#define RBUF_M_LINE     0007777
#define RBUF_DV         040000  /* data valid */

/* DKS_KADDR bits: 0..077777 word address, 0100000 auto-increment. */
#define KADDR_AI        0100000
#define KADDR_MASK      0077777

/* DKS_KCTL bits. */
#define KCTL_V_CHAN     0       /* bits 0-2: КРК channel selected for KADDR/KDATA */
#define KCTL_M_CHAN     0000007
#define KCTL_SIGNAL     0000400 /* "signal БЭСМ-6": ПРП6 on its side (bridge in B2) */

/* Engine mode. */
#define ENGINE_SIM      0       /* protocol engine in C (device), default */
#define ENGINE_HOST     1       /* protocol engine in the Э-60 firmware (B1) */

/* TESTOS stub modes (stage A). */
#define TESTOS_OFF      0
#define TESTOS_ON       1       /* echo loop: input array comes back as output */
#define TESTOS_PROMPT   2       /* "DKS> " prompt dialog */

/* Debug flags (SET DKS DEBUG=...). */
#define DBG_KADOPAM     001     /* KADDR/KDATA window accesses */
#define DBG_RING        002     /* ring slots, system requests, area writes */
#define DBG_IRQ         004     /* interrupts raised */
#define DBG_DATA        010     /* characters to/from the lines */
#define DBG_PROTO       020     /* line state machine */

#endif /* PDP11_DKS_H */

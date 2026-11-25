/* e10025_dks.c: КАДОПАМ bridge server for the Электроника-100-25 model.
   Protocol is the one used by BESM6/besm6_dks.c REMOTE mode and by
   PDP11/pdp11_dks.c (proven code), reused here with the minimum changes.

   Copyright (c) 1993-2022, Robert M Supnik, Mark Pizzolato and others
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
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "e10025_defs.h"
#include "sim_sock.h"

#define DKS_MAGIC_REQ   0xD6
#define DKS_MAGIC_RSP   0xD7
#define DKS_OP_READ     01
#define DKS_OP_WRITE    02
#define DKS_OP_ATTN     03
#define DKS_OP_PING     04
#define DKS_OP_BREAD    05
#define DKS_OP_BWRITE   06
#define DKS_CHAN        2                               /* КРК channel 2, as Э-60 */
#define DKS_STATUS      (0400 << DKS_CHAN)
#define DKS_DOORBELL    077777                          /* БЭСМ-6 -> ПВС */
#define DKS_MEMMASK     077777                          /* 15-bit КАДОПАМ address */
#define DKS_BULK_MAX    255
#define DKSATN_VEC      0150                            /* local free vector, fixed in test_kadopam_bridge.py */

extern uint16 *M;
extern int32 int_req[IPL_HLVL];
extern int32 int_vec[IPL_HLVL][32];
extern int32 (*int_ack[IPL_HLVL][32]) (void);

static t_stat dks_set_bridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dks_signal_attn (void);
static t_stat dks_attn_inta (void);

static SOCKET dks_lsock = INVALID_SOCKET;
static SOCKET dks_bsock = INVALID_SOCKET;
static char dks_spec[CBUFSIZE] = { 0 };
static uint8 dks_frame[8 + 2*DKS_BULK_MAX];
static int32 dks_n = 0, dks_expect = 8;
static int32 dks_nreads = 0, dks_nwrites = 0, dks_nbulk = 0, dks_nattn = 0;
static int32 dks_nbad = 0;
static int32 dks_doorbell = 0;
static int32 dks_pending_attn = 0;
static int32 dks_poll_div = 0;

static UNIT dks_unit = { UDATA (NULL, 0, 0) };

static REG dks_reg[] = {
    { DRDATAD (READS, dks_nreads, 32, "READ frames served") },
    { DRDATAD (WRITES, dks_nwrites, 32, "WRITE frames served") },
    { DRDATAD (BULK, dks_nbulk, 32, "bulk frames served") },
    { DRDATAD (ATTN, dks_nattn, 32, "ATTN frames sent") },
    { DRDATAD (BAD, dks_nbad, 32, "bad frames") },
    { DRDATAD (BELL, dks_doorbell, 16, "last doorbell word") },
    { NULL }
    };

static MTAB dks_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "BRIDGE", "BRIDGE",
      &dks_set_bridge, NULL, NULL, "TCP port for the BESM-6 КАДОПАМ master" },
    { 0 }
    };

static t_stat dks_reset (DEVICE *dptr)
{
if (dks_bsock != INVALID_SOCKET)
    sim_close_sock (dks_bsock);
if (dks_lsock != INVALID_SOCKET)
    sim_close_sock (dks_lsock);
dks_bsock = dks_lsock = INVALID_SOCKET;
dks_spec[0] = 0;
dks_n = 0;
dks_expect = 8;
dks_nreads = dks_nwrites = dks_nbulk = dks_nattn = dks_nbad = 0;
dks_doorbell = 0;
dks_pending_attn = 0;
return SCPE_OK;
}

DEVICE dks_dev = {
    "DKSE", &dks_unit, dks_reg, dks_mod,
    1, 10, 16, 2, 8, 8,
    NULL, NULL, &dks_reset,
    NULL, NULL, NULL,
    NULL, 0, 0, NULL,
    NULL, NULL, NULL, NULL, NULL
    };

/* SET DKSE BRIDGE=<port>: start listening for the BESM-6 КАДОПАМ master. */
static t_stat dks_set_bridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL || *cptr == 0)
    return SCPE_ARG;
if (dks_lsock != INVALID_SOCKET)
    sim_close_sock (dks_lsock);
strncpy (dks_spec, cptr, sizeof (dks_spec) - 1);
dks_spec[sizeof (dks_spec) - 1] = 0;
dks_lsock = sim_master_sock_ex (dks_spec, &r, SIM_SOCK_OPT_REUSEADDR);
if (dks_lsock == INVALID_SOCKET) {
    dks_spec[0] = 0;
    return r? r: SCPE_OPENERR;
    }
sim_messagef (SCPE_OK, "DKSE listening on port %s\n", dks_spec);
return SCPE_OK;
}

static int32 mem_read (int32 addr)
{
if ((uint32) addr >= (MEMSIZE >> 1))
    return 0;
return M[addr] & DMASK;
}

static void mem_write (int32 addr, int32 val)
{
if ((uint32) addr < (MEMSIZE >> 1))
    M[addr] = val & DMASK;
}

static void dks_send (const uint8 *buf, int32 len)
{
if (dks_bsock != INVALID_SOCKET)
    sim_write_sock (dks_bsock, (const char *) buf, len);
}

/* One complete KADOPAM frame. Bulk frames carry 2*cnt data bytes. */
static void frame (const uint8 *req)
{
uint8 rsp[8 + 2*DKS_BULK_MAX];
int32 op = req[1];
int32 addr = ((req[3] << 8) | req[4]) & DKS_MEMMASK;
int32 val = (req[5] << 8) | req[6];
int32 cnt, i;

rsp[0] = DKS_MAGIC_RSP;
rsp[1] = op | 0200;
rsp[2] = req[2];
rsp[3] = req[3];
rsp[4] = req[4];
rsp[5] = rsp[6] = rsp[7] = 0;
switch (op) {
case DKS_OP_READ:
    dks_nreads++;
    if (addr == 0)
        val = DKS_STATUS;
    else
        val = mem_read (addr);
    break;
case DKS_OP_WRITE:
    dks_nwrites++;
    if (addr == 0)
        break;                                          /* channel select */
    mem_write (addr, val);
    if (addr == DKS_DOORBELL) {
        dks_doorbell = val;
        if (val & 0400)                                 /* the БЭСМ-6 rings */
            dks_pending_attn = 1;
        }
    break;
case DKS_OP_PING:
    break;
case DKS_OP_ATTN:
    dks_nbad++;                                         /* master does not send ATTN */
    break;
case DKS_OP_BREAD:
    cnt = req[5];
    dks_nbulk++;
    if (cnt == 0 || addr == 0 || addr + cnt > DKS_MEMMASK)
        cnt = 0;
    else
        for (i = 0; i < cnt; i++) {
            int32 w = mem_read (addr + i);
            rsp[8 + 2*i] = (w >> 8) & 0377;
            rsp[9 + 2*i] = w & 0377;
            }
    rsp[5] = cnt & 0377;
    dks_send (rsp, 8 + 2*cnt);
    return;
case DKS_OP_BWRITE:
    cnt = req[5];
    dks_nbulk++;
    if (cnt == 0 || addr == 0 || addr + cnt > DKS_MEMMASK)
        cnt = 0;
    else
        for (i = 0; i < cnt; i++) {
            int32 w = (req[8 + 2*i] << 8) | req[9 + 2*i];
            mem_write (addr + i, w);
            }
    rsp[5] = cnt & 0377;
    dks_send (rsp, 8);
    return;
default:
    dks_nbad++;
    break;
    }
rsp[5] = (val >> 8) & 0377;
rsp[6] = val & 0377;
dks_send (rsp, 8);
if (dks_pending_attn) {
    dks_pending_attn = 0;
    dks_signal_attn ();                                 /* response first */
    }
}

/* Accept/assemble/serve frames. Called from the CPU loop. */
void dks_poll (void)
{
int32 n;
if (++dks_poll_div & 07)
    return;
if (dks_lsock != INVALID_SOCKET) {
    char *peer = NULL;
    SOCKET newsock = sim_accept_conn_ex (dks_lsock, &peer, SIM_SOCK_OPT_NODELAY);
    if (newsock != INVALID_SOCKET) {
        if (dks_bsock != INVALID_SOCKET)
            sim_close_sock (dks_bsock);
        dks_bsock = newsock;
        dks_n = 0;
        dks_expect = 8;
        }
    if (peer)
        free (peer);
    }
if (dks_bsock == INVALID_SOCKET)
    return;
while ((n = sim_read_sock (dks_bsock, (char *) dks_frame + dks_n,
                           dks_expect - dks_n)) != 0) {
    if (n < 0) {
        sim_close_sock (dks_bsock);
        dks_bsock = INVALID_SOCKET;
        return;
        }
    dks_n += n;
    if (dks_expect == 8 && dks_n >= 6 &&
        dks_frame[1] == DKS_OP_BWRITE)
        dks_expect = 8 + 2 * dks_frame[5];
    else if (dks_expect == 8 && dks_n >= 6 &&
             dks_frame[1] == DKS_OP_BREAD)
        dks_expect = 8;                                 /* reply carries data */
    if (dks_n == dks_expect) {
        if (dks_frame[0] == DKS_MAGIC_REQ)
            frame (dks_frame);
        else
            dks_nbad++;
        dks_n = 0;
        dks_expect = 8;
        }
    }
}

/* ATTN acknowledge: get_vector already clears the bit, keep the clear here
   so a direct call cannot leave a sticky BR4 request. */
static t_stat dks_attn_inta (void)
{
CLR_INT (DKSATN);
return DKSATN_VEC;
}

/* Send ATTN to the БЭСМ-6 master (ПВС -> БЭСМ-6). */
static t_stat dks_signal_attn (void)
{
uint8 f[8];
if (dks_bsock == INVALID_SOCKET)
    return SCPE_OK;
/* DKSE has no DIB; build_dib_tab() wipes int_vec/int_ack on every GO, so the
   ATTN slot is registered here at runtime, after the frame is sent. */
int_vec[IPL_DKSATN][INT_V_DKSATN] = DKSATN_VEC;
int_ack[IPL_DKSATN][INT_V_DKSATN] = &dks_attn_inta;
f[0] = DKS_MAGIC_REQ;
f[1] = DKS_OP_ATTN;
f[2] = DKS_CHAN;
f[3] = f[4] = f[5] = f[6] = f[7] = 0;
dks_send (f, 8);
dks_nattn++;
SET_INT (DKSATN);                                       /* frame first, then BR4 request */
return SCPE_OK;
}

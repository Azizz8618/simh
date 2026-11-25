/* BESM-6 graph plotter (БЕНСОН / ГП НИТИ) with real internal interrupts.
 *
 * The device models the 18th bit of ГРП as an internal interrupt: the CPU
 * enters through 0500₈ (ВНУПР) and, when the guest has installed a vector in
 * M[04023], dispatches through that cell (analog of пб обвнп1-1(М16) for r=18).
 */
#ifndef BESM6_GP_H
#define BESM6_GP_H

/* 64-word frame assembled before the decoder is called. */
#define GP_WORDS 64

/* Device reset, needed by the SIMH device table. */
t_stat gp_reset (DEVICE *dptr);

/* РЕГ '34' (six configuration words) and РЕГ '240' (next data word). */
void gp_config_write (t_value acc);
t_value gp_data_read (void);

/* Short aliases used by the CPU instruction dispatcher. */
void gp_write (t_value acc);
t_value gp_read (void);

/* Data source: УВВ 0172/0173 and ЭК 01210/01211/01215/01216/01217. */
void gp_data_write (t_value acc);
void gp_extra (int addr, t_value acc, t_value *acc_out);

/* Trace points on the guest interrupt path (set gp debug=IRQ). */
void gp_trace_irq (const char *event, t_value a, t_value b, t_value c);

/*
 * НИТИ interface (БЕНСОН-501 через адреса перфоратора ПИ-80).
 * benson.be: УВВ '155 (биты 22/24 — вкл/выкл), '164/'165 (байты элемента),
 * '166 (состояние), '4102 разряд 13 (готовность), ПРП 19 -> 18Р.
 */
int  gp_niti_active (void);
void gp_niti_control (uint32 acc);          /* УВВ 0155 */
void gp_niti_byte (int addr, t_value acc);  /* УВВ 0164/0165/0166 */
t_value gp_niti_status (int addr);          /* УВВ 04164..04167, 04023 */
int  gp_niti_ready13 (void);                /* разряд 13 для УВВ 04102 */


#endif /* BESM6_GP_H */

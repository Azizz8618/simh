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

/* Guest context saved on interrupt entry, as ВНУПР does (disp70.be:982-993). */
void gp_save_context (void);

/* 18Р entry: save context, call op_int_1(), clear 18Р, dispatch the vector. */
void gp_interrupt (void);

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

/* Opt-in user-mode ЭК 50 compatibility (dispak B2 path). */
int gp_user_compat_enabled (void);
int gp_extra_compat_addr (int addr);

#endif /* BESM6_GP_H */

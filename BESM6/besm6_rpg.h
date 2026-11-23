/*
 * besm6_rpg.h: wide-roll six-colour RPLG (РПГ ВНИИЭФ) on the PL channel
 *
 * The plotter replaces the paper-tape punch: 0170/0171 deliver one byte,
 * 04170..04173 report jam, 4102 reports READY2, PRP 15/16 is raised by PL.
 */
#ifndef _BESM6_RPG_H_
#define _BESM6_RPG_H_

#include "sim_defs.h"

/* Debug flags, reported through the PL device. */
#define DBG_RPG_UK      001
#define DBG_RPG_GEOM    002
#define DBG_RPG_SVG     004

extern DEBTAB rpg_deb[];

void rpg_uk (uint8_t byte);
void rpg_reset (void);
void rpg_finish (void);
uint32 rpg_jam (void);

void rpg_enable (int on);
void rpg_set_wide (int on);
void rpg_set_sixcolor (int on);
void rpg_set_fine_step (int on);
void rpg_set_autoproll (int on);
void rpg_set_mode (int enable, int wide, int sixcolor, int fine_step,
                   int autoproll);

int rpg_is_enabled (void);
uint32 rpg_uk_count (void);
uint32 rpg_stroke_count (void);
int rpg_is_wide (void);
int rpg_is_sixcolor (void);
int rpg_is_fine_step (void);
int rpg_has_autoproll (void);

#endif /* _BESM6_RPG_H_ */

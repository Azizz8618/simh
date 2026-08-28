/* Заглушки для графической панели */
#include <stdio.h>
#include "besm6_defs.h"

/* Функции панели - пустые заглушки для сборки без GUI */
void besm6_draw_panel(int force) {}
t_stat besm6_init_panel(UNIT *u, int32 val, CONST char *cptr, void *desc) { return SCPE_OK; }
t_stat besm6_show_panel(FILE *st, UNIT *up, int32 v, CONST void *dp) { return SCPE_OK; }
t_stat besm6_close_panel(UNIT *u, int32 val, CONST char *cptr, void *desc) { return SCPE_OK; }

/* e10025_cal.c: opcode calibration file for Электроника-100-25

   Mechanism from the 1998 article: measure the time of each command on the
   real machine and on the emulator, write the difference to a file, and read
   that file on every start.  The 75 ns and 150 ns figures are below one SIMH
   tick and sim_timer rounds them up.  Corrections are therefore applied as
   simulator ticks to instruction execution, not as host nanoseconds and not
   as channel phase delays.  sim_interval and channel phases stay untouched.
*/

#include "e10025_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern DEVICE cpu_dev;

struct cal_entry {
    int32 opcode;
    int32 delta;
    };

static struct cal_entry cal_tab[E10025_CAL_MAX];
static uint32 cal_count = 0;
static char cal_path[CBUFSIZE] = "";
static int32 cal_mode = E10025_MODE_FAST;
int32 cal_ticks = 0;

static t_bool cal_find (int32 opcode, int32 *delta)
{
uint32 i;

for (i = 0; i < cal_count; i++) {
    if (cal_tab[i].opcode == opcode) {
        *delta = cal_tab[i].delta;
        return TRUE;
        }
    }
return FALSE;
}

static t_stat cal_bad_line (int32 line_no, const char *path, int32 explicit)
{
cal_count = 0;
cal_ticks = 0;
if (explicit)
    cal_path[0] = 0;
return sim_messagef (SCPE_FMT, "bad calibration line %d in %s\n", (int) line_no, path);
}

t_stat e10025_cal_load (const char *fname)
{
char path[CBUFSIZE];
char line[CBUFSIZE];
FILE *f;
char *comment, *op, *dv, *end;
int32 opcode, delta, line_no = 0;
t_bool duplicate;
uint32 i;

cal_count = 0;
cal_ticks = 0;
if (fname != NULL) {
    if (*fname == 0)
        return SCPE_ARG;
    strncpy (cal_path, fname, CBUFSIZE - 1);
    cal_path[CBUFSIZE - 1] = 0;
    }
strncpy (path, cal_path[0]? cal_path: E10025_CALFILE_DEFAULT, CBUFSIZE - 1);
path[CBUFSIZE - 1] = 0;
if ((f = fopen (path, "r")) == NULL) {
    if (fname != NULL) {
        cal_path[0] = 0;
        return sim_messagef (SCPE_FMT, "cannot open calibration file %s\n", path);
        }
    return SCPE_OK;                                     /* no default file is normal */
    }
while (fgets (line, sizeof (line), f) != NULL) {
    line_no++;
    comment = strpbrk (line, "#;");
    if (comment != NULL)
        *comment = 0;
    if ((op = strtok (line, " \t\r\n")) == NULL)
        continue;                                       /* blank or comment line */
    dv = strtok (NULL, " \t\r\n");
    if (dv == NULL) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    opcode = (int32) strtol (op, &end, 8);               /* opcode is always octal */
    if ((*end != 0) || (opcode < 0) || (opcode > DMASK)) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    delta = (int32) strtol (dv, &end, 0);                /* octal with 0, else decimal */
    if ((*end != 0) || (strtok (NULL, " \t\r\n") != NULL)) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    duplicate = FALSE;
    for (i = 0; i < cal_count; i++) {
        if (cal_tab[i].opcode == opcode) {
            cal_tab[i].delta = delta;                    /* last entry wins */
            duplicate = TRUE;
            break;
            }
        }
    if (!duplicate) {
        if (cal_count >= E10025_CAL_MAX) {
            fclose (f);
            cal_count = 0;
            if (fname != NULL)
                cal_path[0] = 0;
            return sim_messagef (SCPE_INCOMP, "calibration table full in %s\n", path);
            }
        cal_tab[cal_count].opcode = opcode;
        cal_tab[cal_count].delta = delta;
        cal_count++;
        }
    }
fclose (f);
return SCPE_OK;
}

void e10025_cal_apply (int32 IR)
{
int32 delta;

if (cal_mode != E10025_MODE_HIST)
    return;                                             /* fast mode: no ticks, no log */
if (!cal_find (IR & DMASK, &delta))
    return;
cal_ticks += delta;
sim_debug (E10025_DBG_CALIB, &cpu_dev, "calib: op %06o delta %d\n", IR & DMASK, delta);
}

t_stat e10025_cal_set_mode (const char *val)
{
if (val == NULL)
    return SCPE_ARG;
if (strcasecmp (val, "historical") == 0)
    cal_mode = E10025_MODE_HIST;
else if (strcasecmp (val, "fast") == 0)
    cal_mode = E10025_MODE_FAST;
else
    return sim_messagef (SCPE_ARG, "unknown CPU mode %s, expected historical or fast\n", val);
cal_ticks = 0;
return SCPE_OK;
}

const char *e10025_cal_get_mode (void)
{
return (cal_mode == E10025_MODE_HIST)? "historical": "fast";
}

t_stat e10025_cal_set_file (const char *fname)
{
if ((fname == NULL) || (*fname == 0))
    return SCPE_ARG;
return e10025_cal_load (fname);
}

const char *e10025_cal_get_file (void)
{
return cal_path[0]? cal_path: E10025_CALFILE_DEFAULT;
}

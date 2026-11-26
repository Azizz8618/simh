# Local build for the Электроника-100-25 simh simulator.
# Kept inside BESM6/ so the shared simh tree is not modified.
ROOT = ../..
BUILD ?= build
BIN ?= $(BUILD)/e10025
CC ?= gcc
CFLAGS ?= -std=gnu99 -O2 -Wall -I. -I$(ROOT) -D_GNU_SOURCE -D_LARGEFILE64_SOURCE
LDFLAGS ?= -lm -lrt -lpthread

SIMCORE = $(ROOT)/scp.c $(ROOT)/sim_console.c $(ROOT)/sim_fio.c \
          $(ROOT)/sim_timer.c $(ROOT)/sim_sock.c $(ROOT)/sim_tmxr.c \
          $(ROOT)/sim_ether.c $(ROOT)/sim_tape.c $(ROOT)/sim_disk.c \
          $(ROOT)/sim_serial.c $(ROOT)/sim_video.c $(ROOT)/sim_imd.c \
          $(ROOT)/sim_card.c

SIMDEV = e10025_cpu.c e10025_io.c e10025_io_lib.c e10025_stddev.c \
         e10025_sys.c e10025_cpumod.c e10025_chan.c e10025_dks.c \
         e10025_cal.c e10025_fp.c e10025_rk.c e10025_rl.c \
         e10025_cr.c e10025_pt.c e10025_lp.c \
         e10025_rh.c e10025_rp.c e10025_rr.c

OBJS = $(addprefix $(BUILD)/,$(notdir $(SIMDEV:.c=.o) $(SIMCORE:.c=.o)))

VPATH = . $(ROOT)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: clean

CC=gcc
CFLAGS=-std=gnu99 -U__STRICT_ANSI__ -O2 -I .. -D_GNU_SOURCE -DUSE_READER_THREAD -DSIM_ASYNCH_IO -DHAVE_SEMAPHORE -DHAVE_SYS_IOCTL -DHAVE_LINUX_CDROM -DSIM_HAVE_DLOPEN=so -DHAVE_UTIME -DHAVE_GLOB -DHAVE_SHM_OPEN -I .

BESM6_SRCS=besm6_cpu.c besm6_tty.c besm6_arith.c besm6_mmu.c besm6_disk.c besm6_drum.c besm6_mg.c besm6_printer.c besm6_punch.c besm6_punchcard.c besm6_vu.c besm6_sys.c besm6_pl.c besm6_panel_stub.c

SIM_SRCS=../scp.c ../sim_console.c ../sim_fio.c ../sim_timer.c ../sim_sock.c ../sim_tmxr.c ../sim_ether.c ../sim_tape.c ../sim_disk.c ../sim_serial.c ../sim_video.c ../sim_imd.c ../sim_card.c

all: ../BIN/besm6

../BIN/besm6: $(BESM6_SRCS) $(SIM_SRCS)
	mkdir -p ../BIN
	$(CC) $(CFLAGS) $(BESM6_SRCS) $(SIM_SRCS) -o $@ -lm -lrt -lpthread -ldl

clean:
	rm -f ../BIN/besm6

.PHONY: all clean


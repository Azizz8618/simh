# BESM-6 regression tests

Self-checking tests for the BESM-6 simulator. Each test loads a small program,
runs it, and verifies the resulting machine state; on any mismatch it prints a
`FAIL:` line and exits with a non-zero status.

## Running

The suite is wired into the build. After a successful compile, `make besm6`
automatically runs it — SIMH looks for `<simdir>/tests/<simname>_test.ini`, i.e.
`BESM6/tests/besm6_test.ini`, and executes it (right after the internal
`RegisterSanityCheck`). A failing test therefore fails the build.

Run the whole suite by hand:

```sh
cd BESM6/tests
../../BIN/besm6 besm6_test.ini
```

Run a single test by hand:

```sh
cd BESM6/tests
../../BIN/besm6 alu.ini        # or pprog05.ini, aout.ini
```

`besm6_test.ini` is only the runner: it `cd`s into this directory, sets a
`runlimit` so a runaway test can't hang the build, and `do`s each sub-test in
turn. A sub-test's `exit 1` terminates the simulator immediately, so the run
stops at the first failure.

## The tests

| Test          | Fixture             | Checks |
|---------------|---------------------|--------|
| `alu.ini`     | `alu.b6`            | Arithmetic-unit program halts three times at address 032013. |
| `pprog05.ini` | `pprog05.b6`        | Accumulator holds 1.0, 2.0, 3.0, 4.0 across four passes. |
| `div.ini`     | `div.b6`            | Division (АД) against the bit-level model of the real arithmetic unit (`../tools/b6div.py`, which generates `div.b6`): Диспак BEMSH literals Е'Е-10' and Е'.16666666' come out as on the real machine, exact integer quotients, 200 rows in all. |
| `aout.ini`    | `aout/hello`        | The binary a.out loader loads a linked executable that prints `Hello!` to the operator console and starts it at its `a_entry` (010). |
| `kadopam.ini` | `kadopam.b6`        | КАДОПАМ (032/0132): КРК channel selection, channel status, Э-60 memory read/write. |
| `esml.ini`    | `esml.b6`           | ЕС-5017 on MG4 (`set mg4 es`): writes an ES zone, a tape mark and a BESM-6 zone to a scratch tape, rewinds and reads them back, checking `04115`/`04117`/`04012`. |
| `uu.ini`      | `pprog08.b6`, `drum20.bin` | Control-unit test (switch register 5 = 4) from the BESM-6 test programs runs 3M instructions without a stop, with `set mmu cache`: БРС fetch-ahead and the БРЗ queue as in ТО-2 (УУ, кн. 1, 2.3, 2.12). |
| `au_uu.ini`   | `pprog08.b6`, `drum20.bin` | The same program as the arithmetic/control-unit test (switch register 5 = 2). |
| `gp.ini`      | built from `../besm6_gp_decoder.c` | Offline decoder test: feeds the two golden C1-line frames (10,10) and (110,110), checks SHA256 of `plot.dat`, `benson.out` and `benson.svg` and that the SVG has exactly two paths. Compiles under `/var/tmp/besm6-gp-test`; no repository files are written. |

The full plotter acceptance test is standalone, because it boots the DISPAK OS
and feeds a card deck instead of loading a memory image:

```sh
python3 BESM6/tests/run-gp-star.py
```

It runs `gp-star.b6`, a Fortran program that draws a ten-vertex star through
`ЭК 50 1210/1211` (inline MADLEN `PDEV`, FOREX 3 compiler from disk 2148).
The runner boots SIMH with an isolated tmux socket, sets `BESM6_NITI_USER=1`
for the user-mode ЭК 050 compatibility hook, checks the ACPU marker and compares
the SHA256 of `plot.dat`, `benson.out` and `benson.svg` plus the 10 SVG paths.
`SIMH_BIN` and `NITI_2348` override the binary and the disk image; all scratch
files stay in `/var/tmp`. This test is not part of `besm6_test.ini`.

The runner patches two DISPAK constants in memory before injecting the line:
`ПОДПРГ` at `01457₈` gains bit 18, and `ГАШРП1` at `01460₈` loses it. In the
stock OS these constants keep 18 out of the internal mask and preserve it in
the clear mask, so the ГРП line is never cleared and ВНУПР loops. With the
patch the trace shows entry at `0500₈`, dispatch through `M[04023]`, the
handler at `077700₈`, GRP clear and `выпр`.

The GRP 18 interrupt path has its own standalone runner:

```sh
python3 BESM6/tests/run-gp-irq.py
```

It boots DISPAK in an isolated tmux session, loads `gp-irq-handler.b6` at
`077700`, points the `обвнп1+30` vector cell `04023` at that handler, raises the
line with `set gp testirq` and checks the SIMH debug log for the full chain:
`18Р line raised`, `TRACE GRP18 -> op_int_1 0500`, optionally
`TRACE РЕГ 037 GRP clear`, and `TRACE ВЫПР interrupt return`. It prints
`PASS: gp-irq` only when the OS entered the internal interrupt through
`ВНУПР` and returned. Like `run-gp-star.py`, it needs a binary built with
`set gp testirq`; `SIMH_BIN` can override a stale `BIN/besm6`. It is not part of
`besm6_test.ini` because it needs a full DISPAK boot.

The same honest interrupt path is exercised with a real Fortran figure:

```sh
python3 BESM6/tests/run-gp-fig.py
```

`gp-fig-deck.b6` fills `COMMON /PDEVXY/ FRAME(64)` and an `ALL(64,14)` table
with an octagon plus a bounding rectangle (14 frames, 12 pen-down strokes).
DISPAK clears the user area at job end, so the runner reads the `X`/`Y`/`P`
arrays from the deck source and re-materializes the same tagged FOREX frames
as a `.b6` image at `040160₈`.  It loads `gp-fig-handler.b6` at `077700₈`,
points `04023₈` at `077710₈`, patches `01457₈`/`01460₈`, raises the 18Р line
and lets the OS run `ВНУПР -> 04023 -> выпр`.  The guest handler streams each
64-word frame to `УВВ 0172` (`gp_data_write()`), so `benson_frame()` is called
inside the interrupt; a done flag absorbs the extra line the stream raises.
`ЭК 01211` is deliberately not used: `cmd_033()` masks its address with
`04377₈`, making the `01210`/`01211` case labels unreachable in this build.
The `ВЫПР` trace from `077747₈` proves the guest handler really returned.

The runner checks the ACPU marker, the 14 frame deliveries, the three golden
SHA256 values (`plot.dat`, `benson.out`, `benson.svg`) and the 13 SVG paths.
`SIMH_BIN` must point at a simulator built with the `set gp testirq` MTAB
entry and the GP sources; the shared `BIN/besm6` may be stale.  All scratch
files stay in `/var/tmp/gp-fig-*`.  Like the two tests above, this one is not
part of `besm6_test.ini`.

The readable Fortran source is `gp-fig.f77`; the card deck is `gp-fig-deck.b6`
with inline MADLEN `PDEV`. Golden `plot.dat`, `benson.out` and `benson.svg`
are kept in `gp-fig-golden/` with their SHA256 values and geometry in
`gp-fig-golden/EXPECTED.md`. They are etalons only, not inputs to the runner.

## Fixture formats

- **`.b6`** — a textual BESM-6 memory image (address lines `в`, octal words `с`,
  instructions `к`, start address `п`). Loaded by `load`. See the main
  [BESM6/README/README.md](../README/README.md) for the format and `dump` command.
- **`aout/hello`** — a binary `a.out` executable produced by the BESM-6 Unix
  cross toolchain (`b6as`/`b6ld`); `load` auto-detects the `BESM` magic. The
  `aout/` directory keeps the source (`hello.s`), a disassembly (`hello.dis`)
  and a `Makefile` that rebuilds it (`make -C aout`) when the toolchain is
  installed.

## Writing a new test

1. Add the fixture (a `.b6` image, or a binary a.out image under `aout/`).
2. Write `mytest.ini`: `load` the fixture, drive it with `go`/`br`, then assert
   with `if`:

   ```
   load mytest.b6
   go -q
   if (PC != 012345) echof "FAIL: mytest, expected PC 12345, got:"; ex PC; exit 1
   echof "PASS: mytest"
   ```

3. Add `do mytest.ini` to `besm6_test.ini`.

### `if` gotchas (learned the hard way)

- **Numbers are decimal by default.** Write octal with a leading zero:
  `032013`, `010`, `04050000000000000`. A bare `32013` is decimal; a `0o`
  prefix is *not* accepted and raises `Invalid Expression Element`.
- **`if (cond) A; B; C`** runs the whole rest of the line conditionally, so the
  usual pattern is `if (bad) echof "FAIL: ..."; ex REG; exit 1`.
- **No `printf`-style args in `echof`.** `echof "x=%s" PC` does not work; print
  the value with a following `ex REG` instead.
- **Register names.** Latin `PC`, `ACC`, `M1`… work (Cyrillic `СчАС`, `СМ`… too).
  `ex -f ACC` prints the accumulator as floating point; compare the raw 48-bit
  octal word in `if`.

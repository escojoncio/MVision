# Build headless (Linux/macOS). GEN = salida de tools/recomp/recomp.py
GEN ?= ../build/gen
# make define CC=cc por defecto (origin 'default'), así que `CC ?=` nunca aplica: se fuerza clang salvo CC explícito.
ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls
CPPFLAGS += -I. -I$(GEN)
# nogen.c (tablas vacías del modo sin código generado) solo entra en headless_interp.
RT = $(filter-out headless.c test_%.c wip_%.c nogen.c,$(wildcard *.c))
OUT ?= ../build
LDLIBS ?= -lm

# Overlays recompiladas (make gen_ov): objetos en $(GENOV); si existen se enlazan al headless.
GENOV ?= ../build/gen_ov
OVOBJ = $(wildcard $(GENOV)/*.o)

$(OUT)/headless: $(RT) headless.c $(wildcard $(GEN)/*.c) $(wildcard *.h) $(OVOBJ)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA) -o $@ $(RT) headless.c $(GEN)/*.c $(OVOBJ) $(LDLIBS)

$(OUT)/test_runtime: $(RT) test_runtime.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_runtime.c $(LDLIBS)

$(OUT)/test_sys: $(RT) test_sys.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_sys.c $(LDLIBS)

$(OUT)/test_interp: $(RT) test_interp.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_interp.c $(LDLIBS)

$(OUT)/test_cdimg: $(RT) test_cdimg.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_cdimg.c $(LDLIBS)

$(OUT)/test_hlebind: $(RT) test_hlebind.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_hlebind.c $(LDLIBS)

# Runtime sin código generado (MV_NOGEN): nada derivado del juego en el binario. Toda la CPU pasa por el
# intérprete; las HLE se enlazan por firmas en ejecución (--sigs / MV_SIGS). No usa GEN ni overlays.
$(OUT)/headless_interp: $(RT) headless.c nogen.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) -I. -DMV_NOGEN $(CFLAGS) $(EXTRA) -o $@ $(RT) headless.c nogen.c $(LDLIBS)

.PHONY: test test_sys headless_interp
headless_interp: $(OUT)/headless_interp

test: $(OUT)/test_runtime $(OUT)/test_interp $(OUT)/test_hlebind $(OUT)/test_cdimg
	$(OUT)/test_runtime
	$(OUT)/test_interp
	$(OUT)/test_hlebind
	$(OUT)/test_cdimg

# Tabla nombre -> hle_<nombre> (hle_table.c va en el repo; esta regla la regenera tras tocar hle*.txt).
# Incluye las HLE de la capa de cámara VR (vr/camera/hle_vrcam.txt); hle_vrcam_weak.c aporta los símbolos si no se enlaza vr_camera_hle.c.
HLETXT = $(wildcard hle*.txt) ../vr/camera/hle_vrcam.txt
.PHONY: hle_table check_hle_table
hle_table:
	python3 ../tools/recomp/hletab.py $(HLETXT) > hle_table.c
check_hle_table:
	python3 ../tools/recomp/hletab.py $(HLETXT) | diff -q - hle_table.c

test_sys: $(OUT)/test_sys
	$(OUT)/test_sys

# Regenera el C a partir del EXE del disco (local, nunca se sube): make gen EXE=... NAMES=...
EXE ?= ../gamedata/main.exe
NAMES ?= ../gamedata/names_eu.json
# GENFLAGS=--poll-entry añade sondeo de IRQ también en la entrada de funciones.
.PHONY: gen
gen:
	rm -rf $(GEN)
	python3 ../tools/recomp/recomp.py $(EXE) $(GEN) --names $(NAMES) $(GENFLAGS) $(foreach f,$(wildcard hle*.txt),--hle $(f))

# Overlays: extrae el código de zona de la tabla de stages (local, nunca se sube) y lo recompila a la base
# común de carga (ver bitácora §15). make gen_ov [-j8]. OVCFLAGS más ligero que CFLAGS: son ~100 MB de C.
STAGES ?= ../gamedata/stages.bin
OVBIN ?= ../build/ov
OVBASE ?= 0x800c4380
OVCFLAGS ?= -O1 -g0 -w -foptimize-sibling-calls
.PHONY: gen_ov
gen_ov:
	rm -rf $(GENOV)
	mkdir -p $(OVBIN) $(GENOV)
	python3 ../tools/stage/stages.py overlays $(STAGES) $(OVBIN) >/dev/null
	python3 ../tools/recomp/recomp.py --ov-batch $(OVBIN) $(GENOV) --base $(OVBASE) --main-gen $(GEN) $(GENFLAGS) > $(OVBIN)/recomp.log
	tail -1 $(OVBIN)/recomp.log
	$(MAKE) -f Makefile ovobjs
.PHONY: ovobjs
OVSRC = $(wildcard $(GENOV)/*.c)
ovobjs: $(OVSRC:.c=.o)
$(GENOV)/%.o: $(GENOV)/%.c $(GEN)/funcs.h cpu.h
	$(CC) $(CPPFLAGS) $(OVCFLAGS) -c -o $@ $<

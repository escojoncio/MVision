# Build headless (Linux/macOS). GEN = salida de tools/recomp/recomp.py
GEN ?= ../build/gen
CC ?= clang
CFLAGS ?= -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls
CPPFLAGS += -I. -I$(GEN)
RT = $(filter-out headless.c test_%.c wip_%.c,$(wildcard *.c))
OUT ?= ../build

$(OUT)/headless: $(RT) headless.c $(wildcard $(GEN)/*.c) $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA) -o $@ $(RT) headless.c $(GEN)/*.c

$(OUT)/test_runtime: $(RT) test_runtime.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_runtime.c

$(OUT)/test_sys: $(RT) test_sys.c $(wildcard *.h)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_sys.c

.PHONY: test test_sys
test: $(OUT)/test_runtime
	$(OUT)/test_runtime

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

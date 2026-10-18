# Build headless (Linux/macOS). GEN = salida de tools/recomp/recomp.py
GEN ?= ../build/gen
CC ?= clang
CFLAGS ?= -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls
CPPFLAGS += -I. -I$(GEN)
RT = mem.c rt.c gte.c bios.c hle.c fiber.c
OUT ?= ../build

$(OUT)/headless: $(RT) headless.c $(wildcard $(GEN)/*.c) cpu.h hle.h fiber.h
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA) -o $@ $(RT) headless.c $(GEN)/*.c

$(OUT)/test_runtime: $(RT) test_runtime.c cpu.h
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -o $@ $(RT) test_runtime.c

.PHONY: test
test: $(OUT)/test_runtime
	$(OUT)/test_runtime

# Regenera el C a partir del EXE del disco (local, nunca se sube): make gen EXE=... NAMES=...
EXE ?= ../gamedata/main.exe
NAMES ?= ../gamedata/names_eu.json
.PHONY: gen
gen:
	rm -rf $(GEN)
	python3 ../tools/recomp/recomp.py $(EXE) $(GEN) --names $(NAMES) $(foreach f,$(wildcard hle*.txt),--hle $(f))

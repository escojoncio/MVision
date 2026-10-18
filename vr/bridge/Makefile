# Puente RAM (lectura del estado del juego desde la RAM emulada). Uso (desde la raíz del repo):
#   make -C vr/bridge test          pruebas sobre volcados sintéticos (no necesitan datos del juego)
#   make -C vr/bridge test-san      lo mismo con -fsanitize=address,undefined
#   make -C vr/bridge dump          compila bridge_dump enlazando el runtime (necesita `make gen` y, opcional, `gen_ov`)
#   make -C vr/bridge run ARGS="..."  ejecuta bridge_dump con datos locales (ver bridge_dump.c)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
RT := ../../runtime
GEN ?= ../../build/gen
GENOV ?= ../../build/gen_ov
OUT ?= ../../build-bridge
EXE ?= ../../gamedata/main.exe
DISC ?= ../../gamedata/disc

all: $(B)/libbridge.a
$(B)/%.o: %.c bridge.h bridge_addrs.h | $(B)
	$(CC) $(CFLAGS) -I. -c $< -o $@
$(B):
	mkdir -p $(B)
$(B)/libbridge.a: $(B)/bridge.o $(B)/bridge_addrs.o
	rm -f $@ && ar rcs $@ $^
$(B)/test_bridge: test_bridge.c $(B)/libbridge.a
	$(CC) $(CFLAGS) -I. $< $(B)/libbridge.a -o $@
test: $(B)/test_bridge
	$(B)/test_bridge
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

# Herramienta con el runtime: mismas fuentes que runtime/Makefile (headless.c se sustituye por bridge_dump.c).
RTSRC := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c $(RT)/nogen.c,$(wildcard $(RT)/*.c))
OVOBJ := $(wildcard $(GENOV)/*.o)
$(OUT)/bridge_dump: bridge_dump.c bridge.c bridge_addrs.c bridge.h bridge_addrs.h $(RTSRC) $(wildcard $(RT)/*.h) $(wildcard $(GEN)/*.c) $(OVOBJ)
	@mkdir -p $(OUT)
	clang -I. -I$(RT) -I$(GEN) -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ bridge_dump.c bridge.c bridge_addrs.c $(RTSRC) $(GEN)/*.c $(OVOBJ)
dump: $(OUT)/bridge_dump
# Guion que entra en partida (hacia el fotograma 3000-4000); ARGS añade/ajusta opciones
SCRIPT ?= --pad 120:start,400:start --pad-every 97:circle --pad-every 211:left
run: dump
	MV_DISC=$(DISC) $(OUT)/bridge_dump $(EXE) $(ARGS)
clean:
	rm -rf build build-san
.PHONY: all test test-san dump run clean

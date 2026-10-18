# vr_scene: ensamblador de escena estéreo. Uso (desde la raíz del repo):
#   make -C vr/scene test        prueba sintética (no necesita datos del juego)
#   make -C vr/scene test-san    lo mismo con ASan/UBSan (gcc)
#   make -C vr/scene dump        compila scene_dump enlazando el runtime (necesita `make gen` y, opcional, `gen_ov`)
#   make -C vr/scene run ARGS="--at 7800,7860 --out DIR"   ejecuta scene_dump con datos locales
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
B ?= build
RT := ../../runtime
BR := ../bridge
RN := ../../render
GEN ?= ../../build/gen
GENOV ?= ../../build/gen_ov
OUT ?= ../../build-scene
EXE ?= ../../gamedata/main.exe
DISC ?= ../../gamedata/disc
INC := -I. -I$(RT) -I$(BR)

all: $(B)/libvrscene.a
$(B)/%.o: %.c vr_scene.h | $(B)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(B):
	mkdir -p $(B)
$(B)/libvrscene.a: $(B)/vr_scene.o
	rm -f $@ && ar rcs $@ $^
$(B)/test_scene: test_scene.c $(B)/libvrscene.a
	$(CC) $(CFLAGS) $(INC) $< $(B)/libvrscene.a -lm -o $@
test: $(B)/test_scene
	$(B)/test_scene
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

# Herramienta con el runtime: mismas fuentes que runtime/Makefile (headless.c se sustituye por scene_dump.c).
RTSRC := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c $(RT)/nogen.c,$(wildcard $(RT)/*.c))
OVOBJ := $(wildcard $(GENOV)/*.o)
$(OUT)/scene_dump: scene_dump.c vr_scene.c vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(wildcard $(RT)/*.h) $(wildcard $(GEN)/*.c) $(OVOBJ)
	@mkdir -p $(OUT)
	clang $(INC) -I$(GEN) -I$(RN) -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ scene_dump.c vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(GEN)/*.c $(OVOBJ) -lm
dump: $(OUT)/scene_dump
# Guion que entra en partida (hacia el fotograma 6500-9000)
SCRIPT ?= --pad 120:start,400:start --pad-every 97:circle --pad-every 211:left
run: dump
	MV_DISC=$(DISC) $(OUT)/scene_dump $(EXE) $(SCRIPT) $(ARGS)
clean:
	rm -rf build build-san
.PHONY: all test test-san dump run clean

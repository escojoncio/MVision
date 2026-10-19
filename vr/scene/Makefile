# vr_scene: ensamblador de escena estéreo. Uso (desde la raíz del repo):
#   make -C vr/scene test        prueba sintética (no necesita datos del juego)
#   make -C vr/scene test-san    lo mismo con ASan/UBSan (gcc)
#   make -C vr/scene test-split  prueba de vr_split (partir triángulos largos, recuento de cruces del plano cercano)
#   make -C vr/scene test-layer2d  capa 2D fiel (vr_layer2d.c) contra el oráculo gpu.c, escala 1 bit a bit y escala 4
#   make -C vr/scene dump        compila scene_dump enlazando el runtime (necesita `make gen` y, opcional, `gen_ov`)
#   make -C vr/scene run ARGS="--at 7800,7860 --out DIR"   ejecuta scene_dump con datos locales
#   make -C vr/scene probe       (opcional, NO forma parte de `test`) compila hud_probe: panel HUD emulado en CPU (hud_emu.c) vs rasterizador de referencia
#   make -C vr/scene probe-run ARGS="--at 221,261 --out DIR [--ops] [--nofb]"   ejecuta hud_probe con datos locales (MV_DISC / EXE como en `run`)
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
$(B)/test_scene: test_scene.c hud_emu.c hud_emu.h $(B)/libvrscene.a
	$(CC) $(CFLAGS) $(INC) test_scene.c hud_emu.c $(B)/libvrscene.a -lm -o $@
test: $(B)/test_scene
	$(B)/test_scene
test-san:
	$(MAKE) test B=build-san CFLAGS="-O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

$(B)/test_split: test_split.c vr_split.c vr_split.h vr_scene.h | $(B)
	$(CC) $(CFLAGS) $(INC) test_split.c vr_split.c -lm -o $@
test-split: $(B)/test_split
	$(B)/test_split

$(B)/test_layer2d: test_layer2d.c vr_layer2d.c vr_layer2d.h vr_scene.c vr_scene.h $(RT)/gpu.c | $(B)
	$(CC) $(CFLAGS) $(INC) test_layer2d.c vr_layer2d.c vr_scene.c $(RT)/gpu.c -lm -o $@
test-layer2d: $(B)/test_layer2d
	$(B)/test_layer2d

# Herramienta con el runtime: mismas fuentes que runtime/Makefile (headless.c se sustituye por scene_dump.c).
RTSRC := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c $(RT)/nogen.c,$(wildcard $(RT)/*.c))
OVOBJ := $(wildcard $(GENOV)/*.o)
$(OUT)/scene_dump: scene_dump.c vr_scene.c vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(wildcard $(RT)/*.h) $(wildcard $(GEN)/*.c) $(OVOBJ)
	@mkdir -p $(OUT)
	clang $(INC) -I$(GEN) -I$(RN) -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ scene_dump.c vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(GEN)/*.c $(OVOBJ) -lm
dump: $(OUT)/scene_dump
# hud_probe: diagnóstico del camino HUD (hud_emu.c replica vr_vs_hud/vr_fs_scene de Metal a 1x y se compara con ref_raster). Mismas fuentes que scene_dump.
$(OUT)/hud_probe: hud_probe.c hud_emu.c hud_emu.h vr_scene.c vr_scene.h $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(wildcard $(RT)/*.h) $(wildcard $(GEN)/*.c) $(OVOBJ)
	@mkdir -p $(OUT)
	clang $(INC) -I$(GEN) -I$(RN) -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ hud_probe.c hud_emu.c vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(RTSRC) $(GEN)/*.c $(OVOBJ) -lm
probe: $(OUT)/hud_probe
probe-run: probe
	MV_DISC=$(DISC) $(OUT)/hud_probe $(EXE) $(SCRIPT) $(ARGS)
# Guion que entra en partida (hacia el fotograma 6500-9000)
SCRIPT ?= --pad 120:start,400:start --pad-every 97:circle --pad-every 211:left
run: dump
	MV_DISC=$(DISC) $(OUT)/scene_dump $(EXE) $(SCRIPT) $(ARGS)
clean:
	rm -rf build build-san
.PHONY: all test test-san test-split test-layer2d dump run probe probe-run clean

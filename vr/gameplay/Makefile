# Capa de juego VR: disparo con las armas del juego (vr_fire) y recarga (vr_reload). Uso (desde la raíz del repo):
#   make -C vr/gameplay test        pruebas sintéticas (no necesitan datos del juego)
#   make -C vr/gameplay test-san    lo mismo con ASan/UBSan
#   make -C vr/gameplay aatest      herramienta de validación del apuntado del lanzador antiaéreo (guion: ver gptest_aa_hooks.c)
#   make -C vr/gameplay gptest      herramienta de validación con el juego real (intérprete; necesita gamedata/ y firmas HLE)
#   make -C vr/gameplay run SCRIPT=guion.txt   la ejecuta (SIGS = fichero de firmas HLE, DISC = carpeta del disco)
# Nada de lo generado se sube: todo va a OUT (por defecto ../../build/gameplay).
CC ?= gcc
CLANG ?= clang
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
OUT ?= ../../build/gameplay
RT := ../../runtime
BR := ../bridge
CAM := ../camera
WP := ../weapons
INC := -I. -I$(BR) -I$(WP) -I$(CAM)
EXE ?= ../../gamedata/main.exe
DISC ?= ../../gamedata/disc
SIGS ?= ../../gamedata/hle_sigs.json
SCRIPT ?= gp_script.txt
FRAMES ?= 12000

all: $(OUT)/test_fire $(OUT)/test_reload $(OUT)/test_aa
$(OUT)/test_fire: test_fire.c vr_fire.c vr_fire.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_fire.c vr_fire.c -o $@ -lm
$(OUT)/test_reload: test_reload.c vr_reload.c vr_reload.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_reload.c vr_reload.c -o $@ -lm
$(OUT)/test_aa: test_aa_game.c vr_aa.c vr_aa.h vr_fire.c vr_fire.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) test_aa_game.c vr_aa.c vr_fire.c -o $@ -lm
test: $(OUT)/test_fire $(OUT)/test_reload $(OUT)/test_aa
	$(OUT)/test_fire
	$(OUT)/test_reload
	$(OUT)/test_aa
test-san: test_fire.c vr_fire.c vr_fire.h test_reload.c vr_reload.c vr_reload.h test_aa_game.c vr_aa.c vr_aa.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(INC) test_fire.c vr_fire.c -o $(OUT)/test_fire_san -lm
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(INC) test_reload.c vr_reload.c -o $(OUT)/test_reload_san -lm
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(INC) test_aa_game.c vr_aa.c vr_fire.c -o $(OUT)/test_aa_san -lm
	$(OUT)/test_fire_san
	$(OUT)/test_reload_san
	$(OUT)/test_aa_san

# ---- herramienta de validación (gptest): copia del runtime con la tabla HLE ampliada con los sustitutos de disparo ----
RTP := $(OUT)/rt
RTDEPS := $(wildcard $(RT)/*.c $(RT)/*.h $(RT)/hle*.txt)
$(RTP)/stamp: $(RTDEPS) hle_vrfire.txt hle_vrfire_weak.c
	rm -rf $(RTP); mkdir -p $(RTP)
	cp $(RT)/*.c $(RT)/*.h $(RT)/hle*.txt $(RTP)/
	cp hle_vrfire_weak.c $(RTP)/
	python3 ../../tools/recomp/hletab.py $(RTP)/hle*.txt ../camera/hle_vrcam.txt hle_vrfire.txt > $(RTP)/hle_table.c
	touch $@
$(OUT)/gptest: $(RTP)/stamp gptest.c gptest_hooks.c gptest_hooks.h vr_fire.c vr_fire_hle.c vr_aa.c vr_reload.c $(BR)/bridge.c $(BR)/bridge_addrs.c
	$(CLANG) $(INC) -I$(RTP) -DMV_NOGEN -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls -o $@ \
	  $(filter-out $(RTP)/headless.c $(RTP)/test_%.c $(RTP)/wip_%.c $(RTP)/nogen.c,$(wildcard $(RTP)/*.c)) gptest.c gptest_hooks.c vr_fire.c vr_fire_hle.c vr_aa.c vr_reload.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RTP)/nogen.c -lm
# aatest: lo mismo con los ganchos del apuntado del lanzador antiaéreo (gptest_aa_hooks.c) en lugar de los de disparo/recarga
$(OUT)/aatest: $(RTP)/stamp gptest.c gptest_aa_hooks.c gptest_hooks.h vr_fire.c vr_fire_hle.c vr_aa.c $(BR)/bridge.c $(BR)/bridge_addrs.c
	$(CLANG) $(INC) -I$(RTP) -DMV_NOGEN -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls -o $@ \
	  $(filter-out $(RTP)/headless.c $(RTP)/test_%.c $(RTP)/wip_%.c $(RTP)/nogen.c,$(wildcard $(RTP)/*.c)) gptest.c gptest_aa_hooks.c vr_fire.c vr_fire_hle.c vr_aa.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RTP)/nogen.c -lm
gptest:
	@$(MAKE) --no-print-directory $(RTP)/stamp
	@$(MAKE) --no-print-directory $(OUT)/gptest
aatest:
	@$(MAKE) --no-print-directory $(RTP)/stamp
	@$(MAKE) --no-print-directory $(OUT)/aatest
run: gptest
	MV_DISC=$(DISC) $(OUT)/gptest $(EXE) $(FRAMES) --sigs $(SIGS) --script $(SCRIPT)
clean:
	rm -rf $(OUT)
.PHONY: all test test-san gptest aatest run clean

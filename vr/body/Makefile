# vr/body: IK de brazos con el modelo original del jugador.
#   make -C vr/body test        pruebas del solver y del pegamento (RAM sintética; sin datos del juego)
#   make -C vr/body test-san    lo mismo con ASan + UBSan
#   make -C vr/body bodytest    herramienta de validación en el headless (necesita `gen` de vr/camera y datos locales)
#   make -C vr/body run         ejecuta la validación y escribe imágenes en OUT/img
# Nada de lo generado se sube: todo va a build-body/ (en .gitignore del repo).
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
OUT ?= ../../build-body
SRC := vr_body.c vr_pose.c vr_fist.c vr_hand_skin.c vr_hand_asset.c

all: test
$(OUT)/test_body: test_body.c $(SRC) vr_body.h vr_pose.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. test_body.c $(SRC) -o $@ -lm
$(OUT)/test_fist: test_fist.c vr_fist.c vr_fist.h test_fist_mesh.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. test_fist.c vr_fist.c -o $@ -lm
$(OUT)/test_hand_skin: test_hand_skin.c vr_hand_skin.c vr_hand_skin.h vr_hand_asset.c vr_hand_asset.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I. test_hand_skin.c vr_hand_skin.c vr_hand_asset.c -o $@ -lm
test: $(OUT)/test_body $(OUT)/test_fist $(OUT)/test_hand_skin
	$(OUT)/test_body
	$(OUT)/test_fist
	$(OUT)/test_hand_skin
test-san: test_body.c $(SRC) vr_body.h vr_pose.h vr_fist.h test_fist.c test_fist_mesh.h test_hand_skin.c vr_hand_skin.h vr_hand_asset.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. test_fist.c vr_fist.c -o $(OUT)/test_fist_san -lm
	$(OUT)/test_fist_san
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. test_hand_skin.c vr_hand_skin.c vr_hand_asset.c -o $(OUT)/test_hand_skin_san -lm
	$(OUT)/test_hand_skin_san
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. test_body.c $(SRC) -o $(OUT)/test_body_san -lm
	$(OUT)/test_body_san
clean:
	rm -f $(OUT)/test_body $(OUT)/test_body_san $(OUT)/test_fist $(OUT)/test_fist_san $(OUT)/test_hand_skin $(OUT)/test_hand_skin_san
.PHONY: all test test-san clean

# ---------------------------------------------------------------------------------------------------------------
# Validación en el headless con datos locales (EXE, disco). Reutiliza la generación de vr/camera (EXE parcheado,
# nombres fusionados) añadiendo el sustituto vrbody_sort (hle_vrbody.txt, vr_names_body.json) y el sustituto
# de la transformación con vrbody_apply (vr/camera/vr_camera_hle.c ya lo llama).
# ---------------------------------------------------------------------------------------------------------------
CLANG ?= clang
RT := ../../runtime
BR := ../bridge
CAM := ../camera
RD := ../../render
EXE ?= ../../gamedata/main.exe
NAMES ?= ../../gamedata/names_eu.json
GENOV ?= ../../build/gen_ov
DISC ?= ../../gamedata/disc
RECOMP := ../../tools/recomp/recomp.py
FRAMES ?= 9000

$(OUT)/exe/main.exe: $(CAM)/patch_exe.py $(EXE)
	@mkdir -p $(OUT)/exe
	python3 $(CAM)/patch_exe.py $(EXE) $@ --h 160 --hfov 179 --vfov 179 --no-divide
	cp $(dir $(EXE))extra_funcs.txt $(OUT)/exe/ 2>/dev/null || true
$(OUT)/extra_names.json: $(CAM)/vr_names.json vr_names_body.json
	python3 -I -c "import json,sys; d=json.load(open('$(CAM)/vr_names.json')); d.update(json.load(open('vr_names_body.json'))); json.dump(d,open('$@','w'))"
$(OUT)/names.json: $(CAM)/vr_names.py $(OUT)/extra_names.json $(NAMES)
	python3 $(CAM)/vr_names.py $(NAMES) $@ $(OUT)/extra_names.json
$(OUT)/gen/dispatch.c: $(OUT)/exe/main.exe $(OUT)/names.json $(CAM)/hle_vrcam.txt hle_vrbody.txt $(wildcard $(RT)/hle*.txt) $(RECOMP)
	rm -rf $(OUT)/gen
	python3 $(RECOMP) $(OUT)/exe/main.exe $(OUT)/gen --names $(OUT)/names.json $(foreach f,$(wildcard $(RT)/hle*.txt),--hle $(f)) --hle $(CAM)/hle_vrcam.txt --hle hle_vrbody.txt
gen: $(OUT)/gen/dispatch.c
GENC := $(wildcard $(OUT)/gen/*.c)
GENO := $(patsubst $(OUT)/gen/%.c,$(OUT)/obj/%.o,$(GENC))
$(OUT)/obj/%.o: $(OUT)/gen/%.c $(OUT)/gen/funcs.h
	@mkdir -p $(OUT)/obj
	$(CLANG) -I$(RT) -I$(OUT)/gen -O2 -w -foptimize-sibling-calls -c -o $@ $<

# Copia del runtime con el parche gpu_capture_seq (de vr/camera) mientras no esté en el repo. FREEZE_RT=1: no recopiar.
RTP := $(OUT)/rt
RTSRC := $(patsubst $(RT)/%,$(RTP)/%,$(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c $(RT)/nogen.c,$(wildcard $(RT)/*.c)))
RTDEPS := $(if $(FREEZE_RT),,$(wildcard $(RT)/*.c $(RT)/*.h))
$(RTP)/stamp: $(RTDEPS) $(CAM)/runtime_seq.patch
	rm -rf $(RTP); mkdir -p $(RTP)
	cp $(RT)/*.c $(RT)/*.h $(RTP)/
	grep -q gpu_capture_seq $(RTP)/gpu.h || patch -s -p2 -d $(RTP) -i $(abspath $(CAM)/runtime_seq.patch)
	touch $@
$(RTSRC): $(RTP)/stamp
OVOBJ := $(wildcard $(GENOV)/*.o)
BODYSRC := bodytest.c vr_body.c vr_fist.c vr_pose.c vr_hand_skin.c vr_hand_asset.c vr_body_cam.c $(CAM)/vr_camera_hle.c $(CAM)/vr_camera.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RD)/ref_raster.c
$(OUT)/bodytest: $(BODYSRC) vr_body.h vr_pose.h $(RTSRC) $(GENO) $(OVOBJ)
	@mkdir -p $(OUT)
	$(CLANG) -I. -I$(CAM) -I$(BR) -I$(RTP) -I$(RD) -I$(OUT)/gen -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ $(BODYSRC) $(RTSRC) $(GENO) $(OVOBJ) -lm
# GENC se evalúa al leer el Makefile: primero se genera el C y luego se vuelve a invocar make.
bodytest: gen
	$(MAKE) $(OUT)/bodytest
run: bodytest
	MV_DISC=$(DISC) $(OUT)/bodytest $(OUT)/exe/main.exe $(FRAMES) $(OUT)/img
.PHONY: gen bodytest run

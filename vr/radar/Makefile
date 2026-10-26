# Radar de muñeca. Uso (desde la raíz del repo):
#   make -C vr/radar test         pruebas C sintéticas (gcc, ASan/UBSan) + sintaxis MSL con maqueta
#   make -C vr/radar run          runtime real (MV_NOGEN, datos locales): separa el radar del HUD, escribe PPM/PNG en OUTIMG
#   make -C vr/radar ref          imagen de referencia por software del quad en un brazo sintético visto desde el ojo
# Todo lo generado va a OUT (por defecto ../../build-radar, ignorado por git).
CC ?= gcc
CLANG ?= clang
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
RT := ../../runtime
RN := ../../render
SC := ../scene
BR := ../bridge
AI := ../ai
OUT ?= ../../build-radar
EXE ?= ../../gamedata/main.exe
DISC ?= ../../gamedata/disc
SIGS ?= ../../build-host/sigs_hle.json
FRAMES ?= 7600
PRESET ?= vr
OUTIMG ?= $(OUT)/img
INC := -I. -I$(RT) -I$(SC) -I$(BR) -I$(AI) -I$(RN)

# ---- pruebas sintéticas ----
SAN := -O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
$(OUT)/test_radar: test_radar.c vr_radar.c vr_radar_hle.c vr_radar.h $(SC)/vr_scene.c $(SC)/vr_scene.h ../hands/vr_glove.c ../hands/vr_glove.h ../body/vr_fist.c
	@mkdir -p $(OUT)
	$(CC) $(SAN) $(INC) -I../hands -I../body test_radar.c vr_radar.c vr_radar_hle.c $(SC)/vr_scene.c ../hands/vr_glove.c ../body/vr_fist.c -lm -o $@
MOCK := $(OUT)/mock
$(MOCK)/metal_stdlib: msl_mock/metal_stdlib
	@mkdir -p $(MOCK)/simd
	cp msl_mock/metal_stdlib $@
	cp msl_mock/simd.h $(MOCK)/simd/simd.h
msl: $(MOCK)/metal_stdlib ../../render/metal/vr_radar.metal msl_mock/layout_check.cpp
	$(CLANG) -x c++ -std=c++14 -fsyntax-only -Wall -Wno-unused -Wno-unknown-attributes -I$(MOCK) -I../../render/metal ../../render/metal/vr_radar.metal
	$(CLANG) -x c++ -std=c++14 -fsyntax-only -Wall -Wno-unused -Wno-unknown-attributes -I$(MOCK) -I../../render/metal msl_mock/layout_check.cpp
test: $(OUT)/test_radar msl
	$(OUT)/test_radar

# ---- runtime real: copia del runtime con runtime_mark.patch (mientras no esté en el repo) ----
RTP := $(OUT)/rt
FREEZE_RT ?=
RTDEPS := $(if $(FREEZE_RT),,$(wildcard $(RT)/*.c $(RT)/*.h))
$(RTP)/stamp: $(RTDEPS) runtime_mark.patch hle_vrradar.txt
	rm -rf $(RTP); mkdir -p $(RTP)
	cp $(RT)/*.c $(RT)/*.h $(RT)/hle*.txt $(RTP)/
	rm -f $(RTP)/headless.c $(RTP)/test_*.c $(RTP)/wip_*.c
	grep -q gpu_capture_set_mark_hook $(RTP)/gpu.h || patch -s -p2 -d $(RTP) -i $(abspath runtime_mark.patch)
	python3 ../../tools/recomp/hletab.py $(wildcard $(RTP)/hle*.txt) ../camera/hle_vrcam.txt hle_vrradar.txt > $(RTP)/hle_table.c
	touch $@
$(OUT)/radar_run: radar_run.c vr_radar.c vr_radar_hle.c vr_radar.h vr_radar_vrp.h $(SC)/vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(AI)/vr_perception.c $(AI)/vr_ai_scan.c $(RTP)/stamp
	$(CLANG) -I. -I$(RTP) -I$(SC) -I$(BR) -I$(AI) -I$(RN) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ radar_run.c vr_radar.c vr_radar_hle.c $(SC)/vr_scene.c $(BR)/bridge.c $(BR)/bridge_addrs.c $(RN)/ref_raster.c $(AI)/vr_perception.c $(AI)/vr_ai_scan.c \
	  $$(ls $(RTP)/*.c) -lm -lpthread
run: $(OUT)/radar_run
	mkdir -p $(OUTIMG)
	MV_DISC=$(DISC) MV_SIGS=$(SIGS) $(OUT)/radar_run $(EXE) $(DISC) $(FRAMES) $(OUTIMG) --preset $(PRESET)
	python3 -I radar_png.py $(OUTIMG)

ref: $(OUT)/test_radar
	$(OUT)/test_radar --ref $(OUTIMG)
	python3 -I radar_png.py $(OUTIMG)

clean:
	rm -rf $(OUT)/test_radar $(OUT)/radar_run $(OUT)/rt
.PHONY: msl test run ref clean

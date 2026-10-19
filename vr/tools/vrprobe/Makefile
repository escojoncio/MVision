# Probador de la capa VR (herramienta LOCAL: necesita el disco del usuario; no entra en la app ni en CI). Uso (desde la raíz del repo):
#   make -C vr/tools/vrprobe run  OUT=<carpeta de salida> [SIGS_REF=<sigs_ref.json>] [ARGS="--zonas a,b --solo barrido,stick"]
#   make -C vr/tools/vrprobe selftest      comprobaciones del rasterizador y del PNG (sin juego)
# Todo lo generado va a BUILD (ignorado por git). Los datos del juego (gamedata/) y las firmas son locales.
ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
RT := ../../../runtime
VR := ../..
BUILD ?= ../../../build-probe
OUT ?= $(BUILD)/informe
EXE ?= ../../../gamedata/main.exe
DISC ?= ../../../gamedata/disc
SIGS_REF ?= ../../../gamedata/sigs_ref.json
SIGS ?= $(BUILD)/sigs_hle.json
SRC_VR := $(VR)/bridge/bridge.c $(VR)/bridge/bridge_addrs.c $(VR)/camera/vr_camera.c $(VR)/camera/vr_camera_hle.c $(VR)/input/vr_input.c $(VR)/input/vr_gesture.c $(VR)/scene/vr_scene.c \
  $(VR)/body/vr_body.c $(VR)/body/vr_body_cam.c $(VR)/body/vr_pose.c $(VR)/radar/vr_radar.c $(VR)/radar/vr_radar_hle.c \
  $(VR)/hands/vr_hands.c $(filter-out $(VR)/weapons/test_%.c,$(wildcard $(VR)/weapons/*.c)) \
  $(VR)/gameplay/vr_fire.c $(VR)/gameplay/vr_fire_hle.c $(VR)/gameplay/vr_aa.c $(VR)/gameplay/vr_missile.c $(VR)/gameplay/vr_scope.c $(VR)/gameplay/vr_reload.c \
  $(VR)/ai/vrai.c $(VR)/ai/vr_ai_scan.c $(VR)/ai/vr_perception.c $(VR)/ai/vr_perception_rt.c $(VR)/ai/vr_cameras.c $(VR)/ai/vr_activity.c $(VR)/ai/vr_activity_rt.c \
  $(VR)/host/vrgame.c $(VR)/host/vrbodyg.c $(VR)/host/vr_view.c $(VR)/host/vrhost.c
SRC_RT := $(filter-out $(RT)/headless.c $(RT)/test_%.c $(RT)/wip_%.c,$(wildcard $(RT)/*.c))
INC := -I. -I$(RT) -I$(VR)/bridge -I$(VR)/camera -I$(VR)/input -I$(VR)/scene -I$(VR)/body -I$(VR)/radar -I$(VR)/hands -I$(VR)/weapons -I$(VR)/gameplay -I$(VR)/ai -I$(VR)/host

$(BUILD)/vrprobe: vrprobe.c probe_raster.c probe_raster.h $(SRC_VR) $(SRC_RT) $(wildcard $(RT)/*.h) $(wildcard $(VR)/*/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(INC) -DMV_NOGEN $(CFLAGS) -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
	  -o $@ vrprobe.c probe_raster.c $(SRC_VR) $(SRC_RT) -lm -lpthread

$(SIGS): $(SIGS_REF)
	@mkdir -p $(BUILD)
	python3 -I ../../../tools/sig/hlesigs.py $< $@ $(wildcard $(RT)/hle*.txt)

run: $(BUILD)/vrprobe $(SIGS)
	@mkdir -p $(OUT)
	MV_SIGS=$(SIGS) $(BUILD)/vrprobe $(EXE) $(DISC) --out $(OUT) $(ARGS)

$(BUILD)/selftest: selftest.c probe_raster.c probe_raster.h
	@mkdir -p $(BUILD)
	gcc -I. $(CFLAGS) -fsanitize=address,undefined -o $@ selftest.c probe_raster.c -lm

selftest: $(BUILD)/selftest
	cd $(BUILD) && ./selftest
	python3 -I checkpng.py $(BUILD)/selftest.png 8 8

.PHONY: run selftest
